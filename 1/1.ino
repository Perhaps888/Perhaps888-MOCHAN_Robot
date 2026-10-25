/* ============================================================
 *  MOCHAN Robot 主程序 (ESP32-S3)
 * ------------------------------------------------------------
 *  功能概览:
 *    1. WiFi AP/STA 双模式, 手机或电脑浏览器打开控制台
 *    2. DRV8833 双 H 桥驱动电机, 三档 PWM 调速 (慢/中/快)
 *    3. 摄像头 MJPEG 推流 + WebSocket 低延迟视频
 *    4. OLED 眼睛表情 / 中文文字 / 休眠时钟
 *    5. 随机巡航、跳舞动作、OTA 固件升级
 *
 *  代码结构:
 *    [1] 头文件、硬件引脚、全局对象
 *    [2] 电机命令映射、PWM 调速、三档设置
 *    [3] OLED 时钟 / 开机动画 / 文字显示
 *    [4] 摄像头初始化 / MJPEG 推流 / 抓拍
 *    [5] 动作动画 / 跳舞序列
 *    [6] WiFi / 时间同步 / Web 路由 / WebSocket
 *    [7] FreeRTOS 双核任务 / setup / loop
 * ============================================================ */

/* ---------- [1] 头文件 ---------- */
#include <WiFi.h>          // WiFi AP/STA 模式
#include <DNSServer.h>     // AP 模式下的 DNS 强制门户
#include <Update.h>        // OTA 固件写入

#include <Wire.h>          // OLED I2C 总线

#include <Adafruit_SSD1306.h>   // OLED 基础绘图
#include <U8g2lib.h>            // 中文点阵字体渲染
#include <FluxGarage_RoboEyes.h> // 机器人眼睛动画

#include <AsyncTCP.h>           // 异步 TCP
#include <ESPAsyncWebServer.h>  // 异步 Web 服务器 + WebSocket

#include <Preferences.h>   // NVS 掉电保存 (WiFi 配置、转速档位)

#include "esp_chip_info.h" // 芯片型号/核心数
#include "esp_heap_caps.h" // 堆内存信息
#include "esp_system.h"    // 复位原因

#include "web_content.h"   // 控制台网页 (PROGMEM 字符串)
#include "web_content_gz.h" // 同一页面的 gzip 预压缩版本

#include "esp_camera.h"    // 摄像头驱动
#include "fb_gfx.h"        // 摄像头帧缓冲
#include "esp_wifi.h"      // WiFi 底层功耗控制

/* ---------- 摄像头 DVP 并口引脚 (ESP32-S3-CAM / OV2640) ---------- */
#define PWDN_GPIO_NUM -1   // 摄像头掉电脚, -1 = 不使用
#define RESET_GPIO_NUM -1  // 摄像头复位脚, -1 = 不使用
#define XCLK_GPIO_NUM 15   // 摄像头主时钟 (20MHz)
#define SIOD_GPIO_NUM 4    // SCCB(I2C) 数据线
#define SIOC_GPIO_NUM 5    // SCCB(I2C) 时钟线
#define Y9_GPIO_NUM 16     // DVP 数据位 D9~D2
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11
#define VSYNC_GPIO_NUM 6   // 场同步
#define HREF_GPIO_NUM 7    // 行同步
#define PCLK_GPIO_NUM 13   // 像素时钟

/* ---------- OLED SSD1306 (I2C) ---------- */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1      // -1 = 使用软件复位
#define OLED_SDA 40
#define OLED_SCL 41

/* ---------- 三个显示对象 ---------- */
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET); // 普通绘图/文字
RoboEyes<Adafruit_SSD1306> roboEyes(display);                             // 眼睛表情动画
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);        // 中文渲染

/* ---------- 电机驱动引脚 (DRV8833 双 H 桥) ----------
 * 每个电机两个输入脚:
 *   左轮: LF(IN1) / LB(IN2)
 *   右轮: RF(IN3) / RB(IN4)
 * 一端 PWM、另一端 LOW → 正转/反转; 两端 LOW → 滑行停止.
 */
#define LF 14
#define LB 21
#define RF 42
#define RB 1

/* ---------- 电机方向极性开关 ----------
 * 装好线后发现某个轮子转反, 不用重接线, 改这里的 true/false 即可:
 *   INVERT_LEFT_MOTOR  : 左轮正反转对调
 *   INVERT_RIGHT_MOTOR : 右轮正反转对调 (当前右轮接线相反)
 *   SWAP_LEFT_RIGHT_MOTOR : 左右轮整体对调
 */
#define INVERT_LEFT_MOTOR   false
#define INVERT_RIGHT_MOTOR  true
#define SWAP_LEFT_RIGHT_MOTOR false

/* ---------- 把“逻辑方向”换算成真实引脚电平 ----------
 * 入参 lf/lb/rf/rb 表示左前/左后/右前/右后四个桥臂是否导通.
 * 本函数只做交换, 不改变“哪一路该导通”的语义, 方便统一适配接线.
 */
static inline void applyMotorPolarity(int &lf, int &lb, int &rf, int &rb) {

  // ① 左右轮整体对调
  if (SWAP_LEFT_RIGHT_MOTOR) { int t; t=lf; lf=rf; rf=t; t=lb; lb=rb; rb=t; }

  // ② 单轮正反转对调 = 交换该轮两个输入脚
  if (INVERT_LEFT_MOTOR)  { int t=lf; lf=lb; lb=t; }
  if (INVERT_RIGHT_MOTOR) { int t=rf; rf=rb; rb=t; }
}

/* ============================================================
 * [2] DRV8833 三档 PWM 调速
 * ------------------------------------------------------------
 * 速度控制链路:
 *   网页滑块
 *     → WebSocket "spdN" 或 HTTP /speed?level=N
 *     → motorSpeedSet() 更新 motorSpeedLevel
 *     → motorDuty() 查表得到 0~255 的占空比
 *     → motorPwmApply() 把占空比写到对应的 IN 引脚
 *
 * 三档定义:
 *   0 = 慢 65% | 1 = 中 82% | 2 = 快 100%
 *
 * 频率为什么是 20kHz:
 *   超出人耳可听范围, 电机不会发出 PWM 啸叫;
 *   DRV8833 支持到 250kHz, 20kHz 留有充足余量.
 *
 * 为什么用 LEDC 通道 4~7:
 *   摄像头 XCLK 固定占用 LEDC channel 0 / timer 0,
 *   电机改用 4~7 可以彻底避开冲突.
 * ============================================================ */
#define MOTOR_PWM_FREQ      20000   // PWM 频率 20kHz
#define MOTOR_PWM_RES       8       // 8bit 分辨率, duty 范围 0~255
#define MOTOR_SPEED_LEVELS  3       // 三档: 慢 / 中 / 快

// 三档占空比表: 索引 0/1/2 = 慢/中/快.
// 没有从 0% 开始, 是为了保证最低档也能克服电机死区、顺利起步.
static const uint8_t MOTOR_SPEED_DUTY[MOTOR_SPEED_LEVELS] = {166, 209, 255};  // 65% / 82% / 100%
// 百分比表只用于网页显示和串口日志, 不参与硬件计算.
static const uint8_t MOTOR_SPEED_PCT [MOTOR_SPEED_LEVELS] = {65, 82, 100};
// 串口日志标签, 用英文避免日志编码问题.
static const char* const MOTOR_SPEED_TAG[MOTOR_SPEED_LEVELS] = {"SLOW", "MID", "FAST"};

// 当前档位 (0/1/2). 默认「快」100%; 掉电后由 loadMotorSpeed() 从 NVS 恢复.
// volatile: Web 任务写、电机任务读, 需要让编译器每次都重新读取.
volatile uint8_t motorSpeedLevel = 2;
// 当前正在输出 PWM 的桥臂位图: bit0=LF, bit1=LB, bit2=RF, bit3=RB.
// 切档时用它保持方向不变, 只刷新占空比.
static volatile uint8_t motorActiveMask = 0;

// 四个 IN 引脚按 LF/LB/RF/RB 顺序排列, 后面用下标统一访问.
static const int MOTOR_PINS[4] = {LF, LB, RF, RB};

// 四个引脚分别绑定 LEDC 通道 4~7 (避开摄像头的 channel 0).
static const uint8_t MOTOR_LEDC_CH[4] = {4, 5, 6, 7};

/* 读取当前档位, 并把异常值 (>=3) 夹回 0, 防止数组越界.
 * NVS 损坏或旧版本数据都可能出现脏值, 这里做最后一道保护. */
static inline uint8_t motorLevel() {
  uint8_t l = motorSpeedLevel;
  return (l < MOTOR_SPEED_LEVELS) ? l : 0;
}

// 根据当前档位取对应的 LEDC 占空比 (0~255).
static inline uint8_t motorDuty() {
  return MOTOR_SPEED_DUTY[motorLevel()];
}

/* 给第 i 个电机引脚写入占空比.
 * Arduino-ESP32 Core 3.x/4.x 的 ledcWrite() 按 GPIO 号操作;
 * Core 2.x 的 ledcWrite() 按 LEDC 通道号操作, 所以这里做版本分支. */
static inline void motorPwmWriteIdx(uint8_t i, uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWrite(MOTOR_PINS[i], duty);
#else
  ledcWrite(MOTOR_LEDC_CH[i], duty);
#endif
}

/* 按位图一次性写四个桥臂:
 *   mask 的 bit0..3 对应 LF/LB/RF/RB, 置 1 的脚输出 duty, 其余输出 0.
 * 用于“保持当前方向、只改变速度”的场景. */
static inline void motorPwmWriteMask(uint8_t mask, uint8_t duty) {
  for (uint8_t i = 0; i < 4; i++) {
    motorPwmWriteIdx(i, (mask & (1u << i)) ? duty : 0);
  }
}

/* 全部电机立即停转:
 *   清空方向位图, 四路占空比全部写 0.
 * 这是 STOP 和异常保护的统一出口. */
static inline void motorPwmAllOff() {
  motorActiveMask = 0;
  motorPwmWriteMask(0, 0);
}

/* 按方向 + 当前档位输出 PWM, 带安全时序:
 *   1. 先记录目标桥臂位图, 并把 motorActiveMask 清零
 *      (切换过程中禁止调速函数去刷新“旧方向”);
 *   2. 四路全部 0%, 保持 2ms 死区, 防止 H 桥上下管直通;
 *   3. 需要导通的桥臂逐个延迟 1ms 再上 PWM,
 *      避免左右电机同时启动造成电流浪涌;
 *   4. 全部写完后更新 motorActiveMask, 供后续切档使用.
 *
 * 参数 lf/lb/rf/rb 是“逻辑电平”(1=该桥臂导通),
 * 调用前应已经过 applyMotorPolarity() 处理.
 */
static inline void motorPwmApply(int lf, int lb, int rf, int rb) {
  uint8_t mask = (lf ? 0x1 : 0) | (lb ? 0x2 : 0) | (rf ? 0x4 : 0) | (rb ? 0x8 : 0);
  uint8_t duty = motorDuty();

  motorActiveMask = 0;          // 切换期间不响应“只改速度”的刷新
  motorPwmWriteMask(0, 0);      // 四路先关断
  delay(2);                     // 2ms 死区, 等待 H 桥完全关断

  for (uint8_t i = 0; i < 4; i++) {
    if (mask & (1u << i)) {
      delay(1);                 // 逐路软启动, 削峰值电流
      motorPwmWriteIdx(i, duty);
    }
  }
  motorActiveMask = mask;       // 记录当前方向, 供切档时原地刷新占空比
}

/* 上电初始化 PWM:
 *   - 把四个电机脚挂到 LEDC, 设置 20kHz / 8bit;
 *   - 把 GPIO 驱动强度拉到最大, 防止 H 桥输入被反电动势顶回浮空;
 *   - 最后全部输出 0%, 保证上电时轮子不转.
 * 该函数在 setup() 最开头调用, 必须早于开机动画里的电机自检. */
static inline void motorPwmInit() {
  for (uint8_t i = 0; i < 4; i++) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcAttachChannel(MOTOR_PINS[i], MOTOR_PWM_FREQ, MOTOR_PWM_RES, MOTOR_LEDC_CH[i]);
#else
    ledcSetup(MOTOR_LEDC_CH[i], MOTOR_PWM_FREQ, MOTOR_PWM_RES);
    ledcAttachPin(MOTOR_PINS[i], MOTOR_LEDC_CH[i]);
#endif

    // 驱动强度最大 (~40mA), 增强抗干扰能力
    gpio_set_drive_capability((gpio_num_t)MOTOR_PINS[i], GPIO_DRIVE_CAP_3);
  }
  motorPwmAllOff();
}

/* ============================================================
 * [1] 全局对象与共享状态
 * ============================================================ */

// Web 服务器、AP 模式 DNS 门户、NVS 存储
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;

// 双 WebSocket:
//   wsCtrl: 文本控制通道 (方向/模式/表情/调速/文字)
//   wsCam : 二进制视频通道 (JPEG 帧)
AsyncWebSocket wsCtrl("/ws_ctrl");
AsyncWebSocket wsCam("/ws_cam");
volatile uint8_t wsCtrlConnCount = 0;   // 当前控制通道连接数
volatile uint8_t wsCamConnCount = 0;    // 当前视频通道连接数

// AP 热点名称和密码
#define AP_SSID "MOCHAN"
#define AP_PASSWORD "12345678"

// WiFi 工作模式: AP 热点 / STA 连接路由器
enum WiFiMode { MODE_AP, MODE_STA };
WiFiMode currentWiFiMode = MODE_AP;

// STA 模式下保存的路由器账号密码
String sta_ssid = "";
String sta_password = "";

// 网页时间同步: 保存浏览器发来的 Unix 时间戳和当时的 millis()
unsigned long syncTimestamp = 0;
unsigned long syncMillis = 0;
bool timeSynced = false;

// 手动控制状态:
//   manualActive   : 用户正在按方向键
//   autoModePaused : 手动操作后暂停随机巡航
//   lastManualTime : 最后一次手动操作时间
volatile bool manualActive = false;
volatile bool autoModePaused = false;
volatile unsigned long lastManualTime = 0;

// 电机命令队列: Web 回调只写命令, 由 oledMotorTask 真正执行,
// 避免在异步网络任务里做 delay() 阻塞视频流.
volatile bool motorCmdValid = false;    // 是否有待执行命令
volatile byte motorCmdPending = 0;      // 待执行命令 (0~8)
volatile bool motorCmdHasTimed = false; // 是否带持续时间
volatile int  motorCmdTimedMs = 0;      // 持续时间 (毫秒)

// 供 /status 上报的“最后一条电机命令”
volatile byte lastMotorCmd = 0;
volatile unsigned long lastMotorCmdMs = 0;

// 手动控制超时: 松手后超过 200ms 没新命令就自动停车
const unsigned long MANUAL_TIMEOUT = 200;
// 手动操作结束后, 等待 10 秒才恢复随机巡航
const unsigned long AUTO_RESUME_DELAY = 10000;

// 时间同步函数声明 (定义在后面)
unsigned long getCurrentTime();

// FreeRTOS 任务句柄
TaskHandle_t OledMotorTaskHandle = NULL;  // OLED + 电机任务 (Core 0)
TaskHandle_t CameraTaskHandle = NULL;     // 摄像头任务 (Core 1)

// 双核互斥锁: 保护电机和摄像头共享数据
SemaphoreHandle_t cam_mutex = NULL;
SemaphoreHandle_t motor_mutex = NULL;

// 摄像头全局帧缓存: cameraTask 写入, HTTP/WS 读取
uint8_t *global_cam_buf = NULL;
volatile size_t global_cam_len = 0;    // 当前帧长度
volatile size_t global_cam_capacity = 0; // 实际分配到的容量 (PSRAM/DRAM 回退时不同)
volatile uint32_t global_cam_seq = 0;  // 帧序号, 用于判断是否是新帧
// 单帧上限: SVGA q18 通常 <100KB, 192KB 留足余量.
// 该值同时决定每个 MJPEG 客户端缓冲和全局缓冲的大小, 不要设得过大.
const size_t CAM_BUF_MAX_SIZE = 192 * 1024;

/* ============================================================
 * [3] OLED 休眠时钟
 * ------------------------------------------------------------
 * 休眠模式下用“数字雨 + 扫描线 + 大号时间”替代眼睛动画:
 *   - 数字雨: 每列随机落下 0/1 字符
 *   - 扫描线: 水平/垂直来回移动, 碰边后切换方向
 *   - 时钟  : 使用浏览器同步时间, 换算成北京时间
 *   - 进度条: 用当前秒数填充, 一分钟走满一圈
 * 该函数由 oledMotorTask 以约 50Hz 的频率调用.
 * ============================================================ */
void drawSleepScreen() {

  // 扫描线方向: 0=水平, 1=垂直
  enum ScanMode : byte {
    SCAN_HORIZONTAL = 0,
    SCAN_VERTICAL   = 1,
  };
  const int SCAN_DIAG_MAX = SCREEN_WIDTH + SCREEN_HEIGHT - 2; // 历史遗留的对角线长度
  static ScanMode scanMode   = SCAN_HORIZONTAL;
  static int      scanPos    = 0;   // 扫描线当前位置
  static int      scanDir    = 1;   // +1 向下/右, -1 向上/左
  static byte     scanBounces = 0;  // 碰边次数, 用于切换扫描方向
  static unsigned long lastScanTime = 0;
  static unsigned long lastSecond = 0;
  static bool colonVisible = true;

  unsigned long now = millis();

  // ---- 每 30ms 推进一次扫描线 ----
  if (now - lastScanTime > 30) {
    lastScanTime = now;
    scanPos += scanDir;

    bool bounced = false;
    switch (scanMode) {
      case SCAN_HORIZONTAL:
        if (scanPos >= SCREEN_HEIGHT - 1 || scanPos <= 0) { scanDir *= -1; bounced = true; }
        break;
      case SCAN_VERTICAL:
        if (scanPos >= SCREEN_WIDTH  - 1 || scanPos <= 0) { scanDir *= -1; bounced = true; }
        break;
        // 下面这段是历史遗留的对角线分支, 当前枚举里没有对应模式, 不会执行.
        if (scanPos >= SCAN_DIAG_MAX || scanPos <= 0)    { scanDir *= -1; bounced = true; }
        break;
    }

    // 连续碰边 2 次后, 有 2/3 概率换一种扫描方向
    if (bounced) {
      scanBounces++;
      if (scanBounces >= 2) {
        scanBounces = 0;
        if (random(3) != 0) {
          ScanMode newMode;
          do { newMode = (ScanMode)random(4); } while (newMode == scanMode);
          scanMode = newMode;

          // 切换后把扫描线放到新方向的随机起点
          switch (scanMode) {
            case SCAN_HORIZONTAL: scanPos = random(SCREEN_HEIGHT);  break;
            case SCAN_VERTICAL:   scanPos = random(SCREEN_WIDTH);   break;
          }
          scanDir = random(2) ? 1 : -1;
        }
      }
    }
  }

  // ---- 每 1 秒翻转一次时钟冒号 ----
  if (now - lastSecond >= 1000) {
    lastSecond = now;
    colonVisible = !colonVisible;
  }

  // ---- 计算北京时间 (UTC+8) ----
  unsigned long systemTime = getCurrentTime();
  const unsigned long TZ_BEIJING_OFFSET = 8UL * 3600UL;
  systemTime += TZ_BEIJING_OFFSET;
  int hours = (systemTime / 3600) % 24;
  int minutes = (systemTime / 60) % 60;
  int seconds = systemTime % 60;

  display.clearDisplay();

  // ---- 数字雨: 每列 8px, 随机落下 0/1 ----
  static int matrixY[20] = { 0 };
  char matrixChars[] = "01";
  int matrixLen = strlen(matrixChars);
  for (int col = 0; col < SCREEN_WIDTH / 8; col++) {
    if (random(50) == 0) matrixY[col] = 0;
    if (matrixY[col] < SCREEN_HEIGHT) {
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(col * 8, matrixY[col]);
      display.print(matrixChars[random(matrixLen)]);
      matrixY[col] += 8;
    }
  }

  // ---- 画扫描线 (隔一个像素画一个点, 形成虚线效果) ----
  switch (scanMode) {
    case SCAN_HORIZONTAL: {
      for (int x = 0; x < SCREEN_WIDTH; x += 2)
        display.drawPixel(x, scanPos, SSD1306_WHITE);
      break;
    }
    case SCAN_VERTICAL: {
      for (int y = 0; y < SCREEN_HEIGHT; y += 2)
        display.drawPixel(scanPos, y, SSD1306_WHITE);
      break;
    }

  }

  // ---- 外框和双层边线 ----
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.drawFastHLine(1, 1, SCREEN_WIDTH - 2, SSD1306_WHITE);
  display.drawFastHLine(1, SCREEN_HEIGHT - 2, SCREEN_WIDTH - 2, SSD1306_WHITE);
  display.drawFastVLine(1, 1, SCREEN_HEIGHT - 2, SSD1306_WHITE);
  display.drawFastVLine(SCREEN_WIDTH - 2, 1, SCREEN_HEIGHT - 2, SSD1306_WHITE);

  // ---- 大号时间 HH:MM:SS ----
  const int digitW = 12;
  const int colonW = 6;
  const int gap = 6;
  int totalW = 96;
  int startX = (SCREEN_WIDTH - totalW) / 2;
  int digitY = 20;

  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);

  char digitStr[3];
  int x = startX;

  sprintf(digitStr, "%02d", hours);
  display.setCursor(x, digitY);
  x += digitW * 2 + gap;
  display.print(digitStr);

  display.setCursor(x, digitY);
  x += colonW + gap;
  if (colonVisible) display.print(":");

  sprintf(digitStr, "%02d", minutes);
  display.setCursor(x, digitY);
  x += digitW * 2 + gap;
  display.print(digitStr);

  display.setCursor(x, digitY);
  x += colonW + gap;
  if (colonVisible) display.print(":");

  sprintf(digitStr, "%02d", seconds);
  display.setCursor(x, digitY);
  display.print(digitStr);

  // ---- 底部状态文字 ----
  display.setTextSize(1);
  display.setCursor(30, 42);
  display.print("SYSTEM STANDBY");

  // ---- 一秒进度条: 0~59 秒映射到 106px ----
  const int BAR_INNER_W = 106;
  int barFill = (long)seconds * BAR_INNER_W / 59;
  display.drawRect(10, 52, 108, 6, SSD1306_WHITE);
  if (barFill > 0) display.fillRect(11, 53, barFill, 4, SSD1306_WHITE);

  display.display();
}

// 开机动画是否正在播放 / OLED 是否初始化成功
volatile bool isBootPlaying = false;
bool oledInitialized = false;

// Web 文字显示状态:
//   textDisplayActive : 当前是否有文字要显示
//   textDisplayDrawn  : 本条消息是否已经画过 (避免重复刷 I2C)
//   textDisplayStartMs: 开始显示的时间 (5 秒后自动消失)
//   textDisplayBuf    : UTF-8 文本缓存
volatile bool textDisplayActive = false;
volatile bool textDisplayDrawn = false;
unsigned long textDisplayStartMs = 0;
char textDisplayBuf[129] = {0};

// 随机巡航模式: 关闭 / 柔和 / 正常
enum RandomMode { RANDOM_OFF, RANDOM_SOFT, RANDOM_NORMAL };
volatile RandomMode randomMode = RANDOM_NORMAL;

// 摄像头是否初始化 / 休眠模式 / OLED 当前表情
volatile bool cameraInitialized = false;
volatile bool sleepMode = false;
volatile uint8_t currentMood = 0;

/* 把 0~8 的命令翻译成四个桥臂的逻辑电平:
 *   0 STOP, 1 FWD, 2 BACK, 3 LEFT, 4 RIGHT,
 *   5 LBACK, 6 RBACK, 7 LFWD, 8 RFWD
 * lf/lb/rf/rb = 左前/左后/右前/右后, 1 表示该桥臂导通.
 * 返回 false 表示未知命令, 调用方应按停车处理.
 */
static inline bool decodeMotorCmdAll(byte c, int &lf, int &lb, int &rf, int &rb) {
  lf = lb = rf = rb = 0;
  switch (c) {
    case 0: return true;                                  // STOP: 四路全 0
    case 1: lf = 1;            rf = 1;            return true; // FWD: 左前 + 右前
    case 2:            lb = 1;            rb = 1; return true; // BACK: 左后 + 右后
    case 3:            lb = 1; rf = 1;            return true; // LEFT: 左后 + 右前 (原地左转)
    case 4: lf = 1;                      rb = 1; return true; // RIGHT: 左前 + 右后 (原地右转)
    case 5:            lb = 1;                       return true; // 仅左轮后退
    case 6:                            rb = 1;       return true; // 仅右轮后退
    case 7: lf = 1;                                  return true; // 仅左轮前进
    case 8:                      rf = 1;             return true; // 仅右轮前进
    default: return false;
  }
}

/* 执行一条电机命令 (0~8).
 * 流程:
 *   1. 12ms 防抖: 长按连发/网络重传时不重复冲击 H 桥
 *   2. 取电机互斥锁, 防止 Web 任务和电机任务同时改引脚
 *   3. STOP 走快速路径: 直接全关, 不加死区/软启动延时
 *   4. 查表得到逻辑电平 → 极性映射 → 按当前档位输出 PWM
 *   5. 日志限流 200ms, 避免长按连发时串口阻塞
 */
void motorWifi(byte c) {
  static const char *CMD_NAME[] = {"STOP", "FWD", "BACK", "LEFT", "RIGHT", "LBACK", "RBACK", "LFWD", "RFWD"};

  // ---- 12ms 方向切换防抖 (STOP 不受限制) ----
  static unsigned long lastSwitchMs = 0;
  unsigned long now = millis();
  if (c != 0 && now - lastSwitchMs < 12UL) return;
  lastSwitchMs = now;

  // ---- 取互斥锁; 拿不到就丢弃本次命令, 避免阻塞网络任务 ----
  if (xSemaphoreTake(motor_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    Serial.printf("[MOTOR] cmd=%s (%d)  互斥锁超时, 跳过!\n", (c < 9) ? CMD_NAME[c] : "?", c);
    return;
  }

  // ---- STOP 快速路径: 不经过 2ms 死区, 立刻全关 ----
  if (c == 0) {
    motorPwmAllOff();
    xSemaphoreGive(motor_mutex);
    return;
  }

  // ---- 命令 → 四路逻辑电平 ----
  int lf = 0, lb = 0, rf = 0, rb = 0;
  if (!decodeMotorCmdAll(c, lf, lb, rf, rb)) {
    Serial.printf("[MOTOR] 未知命令 %d -> 强制停止\n", c);
    motorPwmAllOff();
    xSemaphoreGive(motor_mutex);
    return;
  }

  // ---- 按接线极性/左右对调做最后映射 ----
  applyMotorPolarity(lf, lb, rf, rb);

  // ---- 按当前三档占空比输出 PWM (内含死区 + 逐路软启动) ----
  motorPwmApply(lf, lb, rf, rb);

  // ---- 串口日志限流: 200ms 最多一条 ----
  static unsigned long lastLogMs = 0;
  unsigned long now2 = millis();
  if (now2 - lastLogMs > 200) {
    lastLogMs = now2;
    Serial.printf("[MOTOR] t=%8lu  cmd=%-6s(%d)  LF=%d LB=%d RF=%d RB=%d  spd=%s(%u%%)\n",
                  now2, (c < 9) ? CMD_NAME[c] : "?", c, lf, lb, rf, rb,
                  MOTOR_SPEED_TAG[motorLevel()], MOTOR_SPEED_PCT[motorLevel()]);
  }
  xSemaphoreGive(motor_mutex);
}

/* 带持续时间的电机命令: 执行 c, 等待 ms 毫秒, 再停车.
 * 注意: 这里的 speed 单位是毫秒, 不是转速档位;
 *       转速档位由全局 motorSpeedLevel 决定.
 * 仅用于开机自检等允许阻塞的场景, Web 路径请用 motorEnqueue*.
 */
void motorWifi(byte c, int speed) {
  motorWifi(c);
  delay(speed);
  motorWifi(0);
}

/* 把电机命令放入队列, 立即返回.
 * 真正带 delay() 的 GPIO 操作由 oledMotorTask 执行, 不阻塞 async_tcp.
 * 如果任务已经在等通知, xTaskNotifyGive 能让它立刻醒来.
 */
static inline void motorEnqueue(byte c) {
  motorCmdPending = c;
  motorCmdHasTimed = false;
  motorCmdValid = true;
  if (OledMotorTaskHandle) xTaskNotifyGive(OledMotorTaskHandle);
}

// 带持续时间的入队版本 (开机自检等场景)
static inline void motorEnqueueTimed(byte c, int ms) {
  motorCmdPending = c;
  motorCmdHasTimed = true;
  motorCmdTimedMs = ms;
  motorCmdValid = true;
  if (OledMotorTaskHandle) xTaskNotifyGive(OledMotorTaskHandle);
}

/* 从 NVS 恢复上次保存的转速档位.
 * 使用独立命名空间 "motor", 这样清除 WiFi 配置时不会把转速一起清掉.
 * key 用 "spd3" (3 档), 避免和旧版 4 档数据混淆.
 * 默认值来自 motorSpeedLevel (当前为 2=快 100%).
 */
static void loadMotorSpeed() {
  Preferences sp;
  if (sp.begin("motor", true)) {
    uint8_t v = sp.getUChar("spd3", motorSpeedLevel);
    sp.end();
    if (v < MOTOR_SPEED_LEVELS) motorSpeedLevel = v;  // 只接受 0~2
  }
  Serial.printf("[MOTOR] 转速档位恢复: %s (%u%%)\n",
                MOTOR_SPEED_TAG[motorLevel()], MOTOR_SPEED_PCT[motorLevel()]);
}

// 把当前档位写回 NVS, 掉电重启后仍保持.
static void saveMotorSpeed() {
  Preferences sp;
  if (sp.begin("motor", false)) {
    sp.putUChar("spd3", motorSpeedLevel);
    sp.end();
  }
}

/* 设置转速档位 (0=慢, 1=中, 2=快).
 * 行为:
 *   - 参数越界时夹到 0~2
 *   - 档位没变化则直接返回, 避免重复写 NVS
 *   - 如果电机正在转, 保持方向不变, 只刷新占空比 (不动 2ms 死区)
 *   - 最后写 NVS 并打印日志
 */
static void motorSpeedSet(int level) {
  if (level < 0) level = 0;
  if (level >= MOTOR_SPEED_LEVELS) level = MOTOR_SPEED_LEVELS - 1;
  if ((uint8_t)level == motorSpeedLevel) return;

  motorSpeedLevel = (uint8_t)level;

  // 行驶中切档: 只改 PWM 占空比, 方向位图保持不变.
  if (xSemaphoreTake(motor_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    uint8_t mask = motorActiveMask;
    if (mask) motorPwmWriteMask(mask, motorDuty());
    xSemaphoreGive(motor_mutex);
  }

  saveMotorSpeed();
  Serial.printf("[MOTOR] 转速档位 -> %s (%u%%, duty=%u)\n",
                MOTOR_SPEED_TAG[motorLevel()], MOTOR_SPEED_PCT[motorLevel()], motorDuty());
}

/* 开机动画:
 *   1. 白屏闪烁 3 次
 *   2. 扫描线 + 网格
 *   3. 随机干扰线 + 品牌名
 *   4. 进度条 + 电机自检
 *   5. 眼睛表情轮播
 *   6. 四方向点动自检
 *   7. 显示 WiFi/IP 信息 3 秒
 * 整个动画在 setup() 中同步执行, 此时还没有启动 RTOS 任务.
 */
void bootAnimation() {
  // 没 OLED 或已经在播放就跳过
  if (!oledInitialized || isBootPlaying) return;
  isBootPlaying = true;

  // 动画期间关闭自动眨眼和空闲动作, 避免干扰
  roboEyes.setAutoblinker(OFF, 0, 0);
  roboEyes.setIdleMode(OFF, 0, 0);

  // ---- 阶段 1: 全屏白闪 3 次 ----
  for (int flash = 0; flash < 3; flash++) {
    display.fillScreen(SSD1306_WHITE);
    display.display();
    delay(30);
    display.clearDisplay();
    display.display();
    delay(30);
  }

  // ---- 阶段 2: 垂直扫描线 + 网格背景 ----
  for (int scan = 0; scan < 128; scan += 4) {
    display.clearDisplay();
    display.drawFastVLine(scan, 0, 64, SSD1306_WHITE);
    for (int h = 0; h < 64; h += 16) display.drawFastHLine(0, h, 128, SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(30, 28);
    display.println("SCANNING...");
    display.display();
    delay(5);
  }

  // ---- 阶段 3: 随机干扰线 + MOCHAN 字样 (故障风) ----
  for (int glitch = 0; glitch < 4; glitch++) {
    display.clearDisplay();

    for (int y = 0; y < 64; y += 8)
      if (random(0, 2)) display.drawFastHLine(random(0, 30), y, random(30, 60), SSD1306_WHITE);
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 24);
    display.println("MOCHAN");
    display.display();
    delay(60);
  }

  // ---- 阶段 4: 进度条 + 每隔 20% 让电机轻微点动 ----
  for (int i = 0; i <= 100; i += 4) {
    display.clearDisplay();

    for (int y = 0; y < 64; y += 8) display.drawFastHLine(0, y, 128, SSD1306_WHITE);
    for (int x = 0; x < 128; x += 16) display.drawFastVLine(x, 0, 64, SSD1306_WHITE);

    display.drawRect(8, 22, 112, 12, SSD1306_WHITE);
    display.fillRect(10, 24, map(i, 0, 100, 0, 108), 8, SSD1306_WHITE);

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(35, 5);
    display.println("SYSTEM BOOT");
    display.setCursor(40, 48);
    display.println(i < 50 ? "[LOADING]" : i < 80 ? "[SYNCING]" : "[READY!]");
    display.display();

    if (i % 20 == 0 && i > 0) {
      motorWifi(random(1, 5), 40);  // 随机方向点动 40ms
      delay(25);
      motorWifi(0);
    }
    delay(20);
  }

  // ---- 阶段 5: 眼睛表情轮播 (默认 → 开心 → 生气 → 开心 → 默认) ----
  display.clearDisplay();
  display.display();
  delay(100);
  roboEyes.setMood(DEFAULT);
  roboEyes.update();
  delay(60);
  roboEyes.setMood(HAPPY);
  roboEyes.update();
  delay(60);
  roboEyes.setMood(ANGRY);
  roboEyes.update();
  delay(60);
  roboEyes.setMood(HAPPY);
  roboEyes.update();
  delay(60);
  roboEyes.setMood(DEFAULT);
  roboEyes.update();
  delay(60);

  // ---- 阶段 6: 四方向点动自检 (前进/后退/左转/右转各 40ms) ----
  motorWifi(1, 40);
  delay(40);
  motorWifi(0);
  delay(30);
  motorWifi(2, 40);
  delay(40);
  motorWifi(0);
  delay(30);
  motorWifi(3, 40);
  delay(40);
  motorWifi(0);
  delay(30);
  motorWifi(4, 40);
  delay(40);
  motorWifi(0);
  delay(30);

  roboEyes.setMood(DEFAULT);
  roboEyes.update();
  delay(100);

  // ---- 阶段 7: 显示当前 WiFi 模式和 IP, 停留 3 秒 ----
  bool sta = (currentWiFiMode == MODE_STA && WiFi.status() == WL_CONNECTED);
  String wifiIp   = sta ? WiFi.localIP().toString()  : WiFi.softAPIP().toString();
  String wifiSsid = sta ? WiFi.SSID()                : String(AP_SSID);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(2, 0);
  display.println(sta ? F("[STA] WiFi Connected") : F("[AP] Hotspot Mode"));

  // 分隔线
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // SSID 超过 14 字符时截断并加 "~"
  display.setCursor(2, 13);
  display.print(F("SSID: "));
  if (wifiSsid.length() > 14) { display.print(wifiSsid.substring(0, 13)); display.print(F("~")); }
  else display.print(wifiSsid);

  // IP 地址水平居中 (size 1 每字符 6px)
  int ipW = wifiIp.length() * 6;
  int ipX = (128 - ipW) / 2;
  ipX = (ipX < 0) ? 0 : ipX;
  display.setCursor(ipX, 30);
  display.println(wifiIp);

  // 底部操作提示
  display.setCursor(2, 52);
  display.println(sta ? F("Open browser -> IP above") : F("Connect WiFi -> open IP"));
  display.display();
  delay(3000);

  // 动画结束: 恢复自动眨眼/空闲动作, 释放播放标志
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);
  isBootPlaying = false;
}

/* ============================================================
 * [4] 摄像头与视频流
 * ------------------------------------------------------------
 * 视频同时供两条链路使用:
 *   - HTTP MJPEG /stream : 兼容性兜底
 *   - WebSocket /ws_cam  : 低延迟主通道
 * 低延迟三原则: 限帧率、GRAB_LATEST、浏览器 ACK 背压.
 * ============================================================ */

// 推流参数 (想更清晰/更省带宽, 优先改这里)
#define CAM_STREAM_FRAMESIZE   FRAMESIZE_SVGA  // 800x600
#define CAM_STREAM_JPEG_Q      20              // JPEG 质量, 越小越清晰/越大
#define CAM_STREAM_FB_COUNT    3               // 帧缓冲数量
#define CAM_STREAM_MAX_FPS     30              // 推流帧率上限
#define CAM_WS_ACK_TIMEOUT_MS  200             // 等浏览器 ACK 的兜底时间

// wsCam 背压状态: 上一帧是否还没被浏览器确认
volatile bool     wsCamAwaitAck  = false;
volatile uint32_t wsCamLastSendMs = 0;

/* 初始化摄像头 (OV2640):
 *   - 配置 DVP 并口、SCCB、XCLK
 *   - 输出 JPEG, 分辨率/质量/帧缓冲由上面的宏决定
 *   - 初始化成功后微调亮度、对比度、白平衡等
 * 初始化失败只打印错误, 不阻塞其它功能 (网页仍可控制电机).
 */
void initCamera() {
  camera_config_t config;

  // ---- DVP 并口 + SCCB 引脚映射 ----
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  // ---- 时钟、像素格式、帧缓冲 ----
  config.xclk_freq_hz = 20000000;          // 20MHz 主时钟
  config.pixel_format = PIXFORMAT_JPEG;     // 直接输出 JPEG, 省 CPU
  config.frame_size = CAM_STREAM_FRAMESIZE; // 分辨率
  config.jpeg_quality = CAM_STREAM_JPEG_Q;  // 质量
  config.fb_count = CAM_STREAM_FB_COUNT;    // 帧缓冲数量
  config.grab_mode = CAMERA_GRAB_LATEST;    // 总是拿最新帧
  config.fb_location = CAMERA_FB_IN_PSRAM;  // 帧放 PSRAM

  // ---- 启动摄像头驱动 ----
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    cameraInitialized = false;
    return;
  }

  // ---- 图像参数微调 (不同批次摄像头可在这里改) ----
  sensor_t *s = esp_camera_sensor_get();
  s->set_brightness(s, 0);
  s->set_contrast(s, 2);
  s->set_saturation(s, 2);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_ae_level(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_bpc(s, 0);
  s->set_wpc(s, 0);
  s->set_sharpness(s, 2);
  s->set_framesize(s, CAM_STREAM_FRAMESIZE);
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);

  // ---- 初始化完成 ----
  cameraInitialized = true;
  Serial.printf("Camera initialized: q=%d fb=%d, 推流上限 %dfps (低延迟模式)\n",
                CAM_STREAM_JPEG_Q, CAM_STREAM_FB_COUNT, CAM_STREAM_MAX_FPS);
}

/* 单帧抓拍响应:
 *   /capture 先把当前全局帧 memcpy 到 _snapBuf, 再交给这个响应对象;
 *   _fillBuffer 每次只往网络缓冲里拷一块, 避免一次性占用大内存.
 *   析构时释放 _snapBuf.
 */
class AsyncCaptureResponse : public AsyncAbstractResponse {
private:
  uint8_t *_snapBuf;   // 抓拍帧副本
  size_t _index;       // 当前发送位置
  uint32_t _bufLen;    // 总长度
public:

  AsyncCaptureResponse(uint8_t *buf, size_t len) {
    _code = 200;
    _contentType = "image/jpeg";
    _sendContentLength = true;
    _contentLength = len;
    _snapBuf = buf;
    _index = 0;
    _bufLen = len;
  }

  ~AsyncCaptureResponse() {
    if (_snapBuf) {
      free(_snapBuf);  // 发送结束/连接断开时释放抓拍副本
    }
  }
  bool _sourceValid() const override {
    return true;
  }

  // 把剩余数据拷到 buf, 返回本次拷出的字节数
  virtual size_t _fillBuffer(uint8_t *buf, size_t maxLen) override {
    size_t toSend = (_bufLen - _index) > maxLen ? maxLen : (_bufLen - _index);
    memcpy(buf, _snapBuf + _index, toSend);
    _index += toSend;
    return toSend;
  }
};

/* MJPEG 视频流响应 (/stream):
 *   浏览器用 <img src="/stream"> 时, 服务端持续输出:
 *     --frame\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n<JPEG>
 *   该对象自己持有一块 PSRAM 帧缓冲, 每次取全局最新帧.
 *   如果暂时没有新帧, 就重发上一帧; 连续失败 MAX_FAIL 次后结束响应.
 */
class AsyncCamStreamResponse : public AsyncAbstractResponse {
private:
  uint8_t *_frameBuf;      // 当前要发送的帧副本
  size_t _frameLen;        // 当前帧长度 (0 = 需要取新帧)
  size_t _prevFrameLen;    // 上一帧长度, 用于没有新帧时重发
  uint32_t _lastSentSeq;   // 上次发送的帧序号
  size_t _index;           // 当前帧发送位置
  bool _headerSent;        // 当前帧的 boundary 头是否已发
  uint16_t _failCount;     // 连续取帧失败次数
  static const uint16_t MAX_FAIL = 500;

  static const char BOUNDARY[];
  static const size_t BOUNDARY_LEN;
public:
  AsyncCamStreamResponse() {
    _code = 200;
    _contentType = "multipart/x-mixed-replace; boundary=frame";
    _sendContentLength = false;
    _chunked = true;
    _frameBuf = (uint8_t *)ps_malloc(CAM_BUF_MAX_SIZE);
    _frameLen = 0;
    _prevFrameLen = 0;
    _lastSentSeq = 0xFFFFFFFFu;
    _index = 0;
    _headerSent = false;
    _failCount = 0;
  }
  ~AsyncCamStreamResponse() {
    if (_frameBuf) {
      free(_frameBuf);
    }
  }
  // 512KB→192KB 后仍可能因 PSRAM 紧张分配失败, 交给 handleStream 拒绝请求.
  bool bufferReady() const {
    return _frameBuf != nullptr;
  }
  bool _sourceValid() const override {
    return true;
  }

  virtual size_t _fillBuffer(uint8_t *buf, size_t maxLen) override {

    // 摄像头未就绪, 或缓冲分配失败: 结束响应
    if (!cameraInitialized || !_frameBuf || !global_cam_buf) {
      return 0;
    }

    // ---- 当前没有待发帧: 从全局缓存取最新帧 ----
    if (_frameLen == 0) {
      bool gotNewFrame = false;

      if (xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {

        // 只有出现新序号才拷贝, 避免重复发送同一帧.
        // 同时检查全局缓存实际容量, 防止 DRAM 回退时按 192KB 越界拷贝.
        if (global_cam_len > 0 && global_cam_len <= global_cam_capacity
            && global_cam_len <= CAM_BUF_MAX_SIZE
            && global_cam_seq != _lastSentSeq) {
          memcpy(_frameBuf, global_cam_buf, global_cam_len);
          _frameLen = global_cam_len;
          _prevFrameLen = _frameLen;
          _lastSentSeq = global_cam_seq;
          _failCount = 0;
          gotNewFrame = true;
        }
        xSemaphoreGive(cam_mutex);
      }

      // 没有新帧: 重发上一帧; 如果连上一帧都没有, 计数并等待
      if (!gotNewFrame) {

        if (_prevFrameLen > 0) {
          _frameLen = _prevFrameLen;
        } else {

          _failCount++;
          if (_failCount >= MAX_FAIL) return 0;
          vTaskDelay(pdMS_TO_TICKS(1));
          return 0;
        }
      }
      _index = 0;
      _headerSent = false;
    }

    // ---- 先发 boundary + Content-Length 头 ----
    if (!_headerSent) {
      char lenStr[12];
      int lenStrLen = snprintf(lenStr, sizeof(lenStr), "%u", _frameLen);
      size_t headerLen = BOUNDARY_LEN + lenStrLen + 4;

      if (maxLen < headerLen) {
        buf[0] = '\r';
        return 1;
      }
      size_t pos = 0;
      memcpy(buf + pos, BOUNDARY, BOUNDARY_LEN);
      pos += BOUNDARY_LEN;
      memcpy(buf + pos, lenStr, lenStrLen);
      pos += lenStrLen;
      memcpy(buf + pos, "\r\n\r\n", 4);
      pos += 4;
      _headerSent = true;
      return pos;
    }

    // ---- 再分块发送 JPEG 数据 ----
    size_t bytesLeft = _frameLen - _index;
    if (bytesLeft > 0) {
      size_t toSend = (bytesLeft > maxLen) ? maxLen : bytesLeft;
      memcpy(buf, _frameBuf + _index, toSend);
      _index += toSend;
      if (_index >= _frameLen) {
        _frameLen = 0;  // 本帧发完, 下次取新帧
      }
      return toSend;
    }

    buf[0] = '\r';
    return 1;
  }
};
const char AsyncCamStreamResponse::BOUNDARY[] = "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ";
const size_t AsyncCamStreamResponse::BOUNDARY_LEN = sizeof(AsyncCamStreamResponse::BOUNDARY) - 1;

/* /stream: MJPEG 视频流入口.
 * 浏览器 <img> 直接请求该地址; 响应对象 AsyncCamStreamResponse 持续输出帧.
 * 这里额外关闭 Nagle 算法并加 no-cache 头, 降低延迟/避免旧画面.
 */
void handleStream(AsyncWebServerRequest *request) {
  if (!cameraInitialized || !global_cam_buf) {
    request->send(503, "text/plain", "Camera Offline");
    return;
  }
  AsyncCamStreamResponse *response = new AsyncCamStreamResponse();
  if (!response) {
    request->send(500, "text/plain", "Out of memory");
    return;
  }
  // MJPEG 客户端缓冲分配失败时不要把它交给 AsyncWebServer,
  // 否则 _fillBuffer() 会向空指针 memcpy.
  if (!response->bufferReady()) {
    delete response;
    request->send(503, "text/plain", "Camera buffer unavailable");
    return;
  }

  // ---- 防缓存 + 长连接 + 低延迟 ----
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Connection", "keep-alive");
  response->addHeader("X-Framerate", "25");

  AsyncClient *cli = request->client();
  if (cli) cli->setNoDelay(true);  // 关闭 Nagle, 视频帧立即发
  request->send(response);
}

/* /capture: 抓拍一张 JPEG.
 * 从全局帧缓存复制一份, 再交给 AsyncCaptureResponse 异步发送;
 * 复制过程持锁, 避免和 cameraTask 的写入竞争.
 */
void handleCapture(AsyncWebServerRequest *request) {
  if (!cameraInitialized) {
    request->send(503, "text/plain", "Camera Offline");
    return;
  }

  uint8_t *snap_buf = NULL;
  size_t snap_len = 0;
  if (xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // 除了长度, 还要检查全局缓冲存在且长度不超过实际容量.
    if (global_cam_buf && global_cam_len > 0 && global_cam_len <= global_cam_capacity) {
      snap_buf = (uint8_t *)malloc(global_cam_len);
      if (snap_buf) {
        memcpy(snap_buf, global_cam_buf, global_cam_len);
        snap_len = global_cam_len;
      }
    }
    xSemaphoreGive(cam_mutex);
  }

  if (!snap_buf) {
    request->send(500, "text/plain", "Camera capture failed or busy");
    return;
  }

  AsyncCaptureResponse *response = new AsyncCaptureResponse(snap_buf, snap_len);
  response->addHeader("Content-Disposition", "inline; filename=capture.jpg");
  request->send(response);
}

/* ============================================================
 * [5] 动作动画 / 跳舞序列
 * ============================================================ */

// 随机巡航动画状态:
//   running/phaseActive 控制“动-停-动”状态机
//   t1=运动时长, t2=停止时长, totalSteps=总步数
struct MotorAnim {
  bool running = false;
  byte c = 0;
  int t1 = 0, t2 = 0;
  int totalSteps = 0;
  int currentStep = 0;
  bool phaseActive = false;
  unsigned long phaseStart = 0;
};
MotorAnim anim;

// 立即停止所有电机 (带互斥锁, 失败则本次放弃)
void motorStopAll() {
  if (xSemaphoreTake(motor_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    motorPwmAllOff();
    xSemaphoreGive(motor_mutex);
  }
}

/* 动画/跳舞专用的方向输出:
 * 和 motorWifi 使用同一套命令映射和极性映射, 只是不带 Web 防抖/日志.
 */
void applyAnim(byte c) {
  if (xSemaphoreTake(motor_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

  int lf = 0, lb = 0, rf = 0, rb = 0;
  (void)decodeMotorCmdAll(c, lf, lb, rf, rb);  // 未知命令按 STOP 处理

  applyMotorPolarity(lf, lb, rf, rb);

  motorPwmApply(lf, lb, rf, rb);
  xSemaphoreGive(motor_mutex);
}

/* 启动随机巡航动画:
 *   c    = 方向命令
 *   t1   = 每次运动持续毫秒
 *   t2   = 每次停止持续毫秒
 *   time = 总步数
 */
void motorAnimStart(byte c, int t1, int t2, int time) {
  anim.running = true;
  anim.c = c;
  anim.t1 = t1;
  anim.t2 = t2;
  anim.totalSteps = time;
  anim.currentStep = 0;
  anim.phaseActive = true;
  anim.phaseStart = millis();
  applyAnim(anim.c);
}

// 停止随机巡航动画并立即停车
void motorAnimStop() {
  anim.running = false;
  motorStopAll();
}

/* 随机巡航状态机, 由 oledMotorTask 每 20ms 调一次:
 *   - 手动控制期间暂停
 *   - 运动阶段结束后停车, 进入停止阶段
 *   - 停止阶段结束后步数 +1, 未到总数则继续运动
 */
void motorAnimUpdate() {
  if (!anim.running || manualActive) return;
  unsigned long now = millis();
  unsigned long duration = anim.phaseActive ? (unsigned long)anim.t1 : (unsigned long)anim.t2;
  if ((long)(now - anim.phaseStart) < (long)duration) return;
  anim.phaseStart = now;
  if (anim.phaseActive) {
    anim.phaseActive = false;
    motorStopAll();
  } else {
    anim.currentStep++;
    if (anim.currentStep >= anim.totalSteps) {
      anim.running = false;
      motorStopAll();
      return;
    }
    anim.phaseActive = true;
    applyAnim(anim.c);
  }
}

/* 跳舞序列: 每步 = {命令, 持续毫秒}
 * 命令编号见 decodeMotorCmdAll:
 *   0 STOP, 1 FWD, 2 BACK, 3 LEFT, 4 RIGHT
 * 序列内容: 左右摇摆 3 轮 → 前后各一次 → 小幅左右 2 轮 → 停车
 */
struct DanceStep { byte cmd; uint16_t ms; };

static const DanceStep DANCE_SEQ[] PROGMEM = {
  {3,150},{0,200},{4,150},{0,200},   // 第 1 轮: 左、停、右、停
  {3,150},{0,200},{4,150},{0,200},   // 第 2 轮
  {3,150},{0,200},{4,150},{0,200},   // 第 3 轮
  {1,200},{0,250},{2,200},{0,250},   // 前进、停、后退、停
  {3,100},{0,150},{4,100},{0,150},   // 小幅左、停、右、停
  {3,100},{0,150},{4,100},{0,150},   // 再一轮小幅
  {0,1},                              // 收尾停车
};
static const uint8_t DANCE_SEQ_LEN = sizeof(DANCE_SEQ)/sizeof(DANCE_SEQ[0]);

// 当前跳舞进度
struct DanceState {
  bool active = false;
  uint8_t step = 0;
  unsigned long stepStart = 0;
} dance;

// 开始跳舞: 先停随机动画, 重置步数, 立即执行第一步
void danceStart() {
  motorAnimStop();
  dance.active = true;
  dance.step = 0;
  dance.stepStart = millis();

  DanceStep s;
  memcpy_P(&s, &DANCE_SEQ[0], sizeof(s));
  applyAnim(s.cmd);
}

/* 跳舞状态机, 由 oledMotorTask 推进:
 *   时间到就执行下一步; 全部跳完后恢复巡航模式.
 */
void danceUpdate() {
  if (!dance.active) return;
  DanceStep s;
  memcpy_P(&s, &DANCE_SEQ[dance.step], sizeof(s));
  if ((long)(millis() - dance.stepStart) < (long)s.ms) return;
  dance.step++;
  if (dance.step >= DANCE_SEQ_LEN) {
    dance.active = false;
    motorStopAll();
    randomMode = RANDOM_NORMAL;
    autoModePaused = false;
    return;
  }
  dance.stepStart = millis();
  memcpy_P(&s, &DANCE_SEQ[dance.step], sizeof(s));
  applyAnim(s.cmd);
}

/* 判断浏览器是否接受 gzip.
 * 页面 uncompressed 约 44KB, gzip 后约 14KB; 支持时优先发压缩版.
 */
static inline bool clientAcceptsGzip(AsyncWebServerRequest *request) {
  if (!request->hasHeader("Accept-Encoding")) return false;
  return request->getHeader("Accept-Encoding")->value().indexOf("gzip") >= 0;
}

/* 从 PROGMEM 直接发送 HTML:
 *   - 支持 gzip 就发预压缩数组, 省带宽/加载时间
 *   - 不支持就发原字符串
 *   - 两种情况都不把页面拷到堆
 */
static void sendProgmemHtml(AsyncWebServerRequest *request,
                            const uint8_t *plain, size_t plainLen,
                            const uint8_t *gzip, size_t gzipLen) {
  bool useGzip = clientAcceptsGzip(request);
  AsyncWebServerResponse *resp = request->beginResponse(
      200, "text/html", useGzip ? gzip : plain, useGzip ? gzipLen : plainLen);
  if (!resp) { request->send(500, "text/plain", "oom"); return; }

  if (useGzip) {
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Vary", "Accept-Encoding");
  }
  resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(resp);
}

/* 首页: 直接把 PROGMEM 里的页面作为响应体.
 * 不拷贝到堆; no-cache 确保浏览器拿到最新页面.
 */
void handleRoot(AsyncWebServerRequest *request) {
  sendProgmemHtml(request,
                  (const uint8_t *)PAGE_HTML_MAIN, strlen(PAGE_HTML_MAIN),
                  PAGE_HTML_MAIN_GZ, PAGE_HTML_MAIN_GZ_LEN);
}

/* ============================================================
 * [6] WiFi / 时间同步 / 网页协议
 * ============================================================ */

// 从 NVS 读取 WiFi 模式、SSID、密码
void loadWiFiConfig() {
  preferences.begin("mochan", true);
  int mode = preferences.getInt("wifi_mode", MODE_AP);
  sta_ssid = preferences.getString("sta_ssid", "");
  sta_password = preferences.getString("sta_password", "");
  preferences.end();
  currentWiFiMode = (WiFiMode)mode;
}

// 保存 WiFi 配置到 NVS (切换 STA 时调用)
void saveWiFiConfig(int mode, String ssid, String password) {
  preferences.begin("mochan", false);
  preferences.putInt("wifi_mode", mode);
  preferences.putString("sta_ssid", ssid);
  preferences.putString("sta_password", password);
  preferences.end();
}

// 清除 WiFi 配置并恢复 AP 模式
void clearWiFiConfig() {
  preferences.begin("mochan", false);
  preferences.clear();
  preferences.end();
  currentWiFiMode = MODE_AP;
  sta_ssid = "";
  sta_password = "";
}

/* 连接路由器 (STA 模式):
 *   关闭旧连接 → 切 STA → 最大发射功率 → 最多等 20 秒.
 * 连接成功后关闭 WiFi 省电, 降低视频延迟; 失败返回 false, 由调用方回退 AP.
 */
bool connectWiFiSTA() {
  if (sta_ssid.isEmpty()) return false;
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(sta_ssid.c_str(), sta_password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
  }
  return false;
}

/* 启动 AP 热点:
 *   创建 "MOCHAN" 热点, 并启动 DNS 强制门户 (访问任意域名都跳到本机).
 */
void startAPMode() {
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  dnsServer.start(53, "*", WiFi.softAPIP());
}

// 浏览器把当前 Unix 时间戳发过来, 用于 OLED 时钟
void handleSyncTime(AsyncWebServerRequest *request) {
  if (request->hasParam("timestamp")) {
    syncTimestamp = request->getParam("timestamp")->value().toInt();
    syncMillis = millis();
    timeSynced = true;
    request->send(200, "text/plain", "OK");
  } else request->send(400, "text/plain", "Missing timestamp");
}

// 返回当前 Unix 时间; 没同步过就用开机秒数
unsigned long getCurrentTime() {
  if (timeSynced) return syncTimestamp + (millis() - syncMillis) / 1000;
  return millis() / 1000;
}

/* HTTP 方向路由不做全局 seq 去重:
 * 多个客户端共用一个全局序号会互相丢命令; 方向命令本身幂等,
 * 直接按到达顺序入队即可. 前端仍可带 ?seq=, 服务端忽略.
 */

/* ============================================================
 * /ws_ctrl 文本控制协议
 * ------------------------------------------------------------
 * 帧格式:
 *   "f" / "b" / "l" / "r" / "s"          方向/停车
 *   "cmd,seq"                             带序号 (seq 仅为兼容旧前端, 服务端忽略)
 *   "mode_off" / "mode_soft" / "mode_normal"
 *   "spd0" / "spd1" / "spd2"              转速 慢/中/快
 *   "speed=0..2"                          转速的另一种写法
 *   "mood0".."mood3"                      OLED 表情
 *   "dance"                               跳舞
 *   "text=..."                            OLED 显示文字
 * 最后一个控制客户端断开时自动停车, 防止机器人失联后继续跑.
 * ============================================================ */
static void onWsCtrlEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                          AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      // 新连接: 只计数; 不再用全局 seq, 避免多个客户端互相丢命令
      wsCtrlConnCount++;
      Serial.printf("[WS_CTRL] client %u connected (total=%u)\n", client->id(), wsCtrlConnCount);
      break;
    }
    case WS_EVT_DISCONNECT: {
      // 断线: 计数 -1; 只有最后一个控制客户端离开时才自动停车
      if (wsCtrlConnCount > 0) wsCtrlConnCount--;
      Serial.printf("[WS_CTRL] client %u disconnected (total=%u)\n",
                    client->id(), wsCtrlConnCount);
      if (wsCtrlConnCount == 0) {
        Serial.println(F("[WS_CTRL] last client gone -> auto-stop"));
        motorAnimStop();
        motorEnqueue(0);
        manualActive = false;
        autoModePaused = false;
      }
      break;
    }
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (!info->final || info->opcode != WS_TEXT) break;  // 只处理完整文本帧
      String msg = String((char*)data, len);               // 用长度构造, 防止无 \0 越界
      msg.trim();
      if (msg.length() == 0) break;

      // ---- 拆分 "cmd,seq"; text= 内容可能含逗号, 跳过拆分 ----
      // WebSocket 基于 TCP, 本身有序, 不需要 seq 去重;
      // 逗号后的序号只为兼容旧前端, 这里直接忽略.
      int commaIdx = msg.startsWith("text=") ? -1 : msg.indexOf(',');
      String cmd   = (commaIdx >= 0) ? msg.substring(0, commaIdx) : msg;

      // ---- 方向控制: 先停动画, 标记手动, 再入队 ----
      if      (cmd == "f") { motorAnimStop(); manualActive=true; autoModePaused=true; lastManualTime=millis(); motorEnqueue(1); }
      else if (cmd == "b") { motorAnimStop(); manualActive=true; autoModePaused=true; lastManualTime=millis(); motorEnqueue(2); }
      else if (cmd == "l") { motorAnimStop(); manualActive=true; autoModePaused=true; lastManualTime=millis(); motorEnqueue(3); }
      else if (cmd == "r") { motorAnimStop(); manualActive=true; autoModePaused=true; lastManualTime=millis(); motorEnqueue(4); }
      else if (cmd == "s") { motorAnimStop(); manualActive=true; autoModePaused=true; lastManualTime=millis(); motorEnqueue(0); }

      // ---- 运行模式: 休眠 / 柔和随机 / 正常随机 ----
      else if (cmd == "mode_off") {
        randomMode = RANDOM_OFF; sleepMode = true; motorAnimStop();
      } else if (cmd == "mode_soft") {
        randomMode = RANDOM_SOFT; sleepMode = false; currentMood = 0;
        roboEyes.setMood(DEFAULT); roboEyes.setAutoblinker(ON, 3, 2); roboEyes.setIdleMode(ON, 2, 2);
      } else if (cmd == "mode_normal") {
        randomMode = RANDOM_NORMAL; sleepMode = false; currentMood = 0;
        roboEyes.setMood(DEFAULT); roboEyes.setAutoblinker(ON, 3, 2); roboEyes.setIdleMode(ON, 2, 2);
      }

      // ---- 三档转速: spd0/1/2, 或 speed=N ----
      else if (cmd == "spd0") { motorSpeedSet(0); }
      else if (cmd == "spd1") { motorSpeedSet(1); }
      else if (cmd == "spd2") { motorSpeedSet(2); }
      else if (cmd.startsWith("speed=")) { motorSpeedSet(cmd.substring(6).toInt()); }

      // ---- OLED 表情 ----
      else if (cmd == "mood0") { currentMood = 0; roboEyes.setMood(DEFAULT); }
      else if (cmd == "mood1") { currentMood = 1; roboEyes.setMood(TIRED); }
      else if (cmd == "mood2") { currentMood = 2; roboEyes.setMood(ANGRY); }
      else if (cmd == "mood3") { currentMood = 3; roboEyes.setMood(HAPPY); }

      // ---- 跳舞 ----
      else if (cmd == "dance") { autoModePaused = true; randomMode = RANDOM_OFF; danceStart(); }

      // ---- OLED 文字: text=内容, '+' 还原成空格 ----
      else if (cmd.startsWith("text=")) {
        String t = cmd.substring(5);
        t.replace('+', ' ');
        t.toCharArray(textDisplayBuf, sizeof(textDisplayBuf));
        textDisplayStartMs = millis();
        textDisplayDrawn = false;
        textDisplayActive = true;
      }
      break;
    }
    default: break;
  }
}

/* /ws_cam 视频通道:
 *   服务端推送 JPEG 二进制帧; 浏览器每渲染完一帧回一个文本帧 (ACK).
 *   wsCamAwaitAck=true 表示上一帧还没被确认, cameraTask 会主动丢新帧,
 *   保证“在途帧永远 <= 1”, 网络慢时表现为掉帧而不是延迟越积越大.
 */
static void onWsCamEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      wsCamConnCount++;
      wsCamAwaitAck = false;  // 新连接: 立即推首帧
      Serial.printf("[WS_CAM ] client %u connected (total=%u)\n", client->id(), wsCamConnCount);
      break;
    }
    case WS_EVT_DISCONNECT: {
      if (wsCamConnCount > 0) wsCamConnCount--;
      wsCamAwaitAck = false;  // 不要把等待状态留给下一个客户端
      Serial.printf("[WS_CAM ] client %u disconnected (total=%u)\n",
                    client->id(), wsCamConnCount);
      break;
    }
    case WS_EVT_DATA: {
      wsCamAwaitAck = false;  // 收到任意上行帧都当作上一帧已消费
      break;
    }
    default: break;
  }
}

/* ============================================================
 * Web 路由注册
 * ------------------------------------------------------------
 * 路由分几组:
 *   控制: /f /b /l /r /s, /speed, /spd0..2, /mode_*, /dance
 *   显示: /display_text, /mood, /status
 *   媒体: /stream, /capture
 *   网络: /wifi_scan, /wifi_save, /wifi_ap, /sync_time
 *   系统: /restart, /ota, /update
 * ============================================================ */
void setupServer() {

  // ---- 首页 ----
  server.on("/", HTTP_GET, handleRoot);

  // ---- 连通性探测: 前端每 5 秒请求 /ping, 期望纯文本 pong ----
  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "pong");
  });

  // ---- 方向控制: 非阻塞入队, 立即返回 200 ----
  server.on("/f", HTTP_GET, [](AsyncWebServerRequest *request) {
    motorAnimStop();
    manualActive = true;
    autoModePaused = true;
    lastManualTime = millis();
    motorEnqueue(1);
    request->send(200);
  });
  server.on("/b", HTTP_GET, [](AsyncWebServerRequest *request) {
    motorAnimStop();
    manualActive = true;
    autoModePaused = true;
    lastManualTime = millis();
    motorEnqueue(2);
    request->send(200);
  });
  server.on("/l", HTTP_GET, [](AsyncWebServerRequest *request) {
    motorAnimStop();
    manualActive = true;
    autoModePaused = true;
    lastManualTime = millis();
    motorEnqueue(3);
    request->send(200);
  });
  server.on("/r", HTTP_GET, [](AsyncWebServerRequest *request) {
    motorAnimStop();
    manualActive = true;
    autoModePaused = true;
    lastManualTime = millis();
    motorEnqueue(4);
    request->send(200);
  });
  server.on("/s", HTTP_GET, [](AsyncWebServerRequest *request) {
    motorAnimStop();
    manualActive = true;
    autoModePaused = true;
    lastManualTime = millis();
    motorEnqueue(0);
    request->send(200);
  });

  // ---- 三档转速 HTTP 接口 ----
  // /speed?level=0..2 : 带参数校验, 返回 JSON
  // /spd0../spd2      : WebSocket 断线时 sendCmd() 的 HTTP 兜底
  server.on("/speed", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("level")) { request->send(400, "text/plain", "Missing level"); return; }
    int lvl = request->getParam("level")->value().toInt();
    if (lvl < 0 || lvl >= MOTOR_SPEED_LEVELS) { request->send(400, "text/plain", "Bad level"); return; }
    motorSpeedSet(lvl);
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/spd0", HTTP_GET, [](AsyncWebServerRequest *request) { motorSpeedSet(0); request->send(200); });
  server.on("/spd1", HTTP_GET, [](AsyncWebServerRequest *request) { motorSpeedSet(1); request->send(200); });
  server.on("/spd2", HTTP_GET, [](AsyncWebServerRequest *request) { motorSpeedSet(2); request->send(200); });

  // ---- 网页文字发送到 OLED (约 5 秒后自动消失) ----
  server.on("/display_text", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("text")) {
      String t = request->getParam("text")->value();
      t.toCharArray(textDisplayBuf, sizeof(textDisplayBuf));
      textDisplayStartMs = millis();
      textDisplayDrawn = false;
      textDisplayActive = true;
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing text");
    }
  });

  // ---- 运行模式切换: 休眠 / 柔和随机 / 正常随机 ----
  server.on("/mode_off", HTTP_GET, [](AsyncWebServerRequest *request) {
    randomMode = RANDOM_OFF;
    sleepMode = true;
    motorAnimStop();
    request->send(200);
  });
  server.on("/mode_soft", HTTP_GET, [](AsyncWebServerRequest *request) {
    randomMode = RANDOM_SOFT;
    sleepMode = false;
    currentMood = 0;
    roboEyes.setMood(DEFAULT);
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.setIdleMode(ON, 2, 2);
    request->send(200);
  });
  server.on("/mode_normal", HTTP_GET, [](AsyncWebServerRequest *request) {
    randomMode = RANDOM_NORMAL;
    sleepMode = false;
    currentMood = 0;
    roboEyes.setMood(DEFAULT);
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.setIdleMode(ON, 2, 2);
    request->send(200);
  });

  // ---- OLED 表情: /mood?m=0..3 ----
  server.on("/mood", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("m")) {
      int m = request->getParam("m")->value().toInt();
      if (m >= 0 && m <= 3) {
        currentMood = (uint8_t)m;
        roboEyes.setMood((unsigned char)m);
        request->send(200, "text/plain", "OK");
      } else request->send(400, "text/plain", "Bad mood");
    } else request->send(400, "text/plain", "Missing m");
  });

  // ---- 设备状态 JSON: 网页每 2 秒轮询一次 ----
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool sta = (currentWiFiMode == MODE_STA);
    String ip   = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    String ssid = sta ? WiFi.SSID() : String(AP_SSID);
    int rssi    = sta ? WiFi.RSSI() : 0;

    // 命令编号 → 英文名, 供前端显示
    static const char* MOTOR_NAME[9] = {"STOP","FWD","BACK","LEFT","RIGHT","LBACK","RBACK","LFWD","RFWD"};
    byte mc = lastMotorCmd;
    const char* mname = (mc < 9) ? MOTOR_NAME[mc] : "?";
    // 随机巡航模式 → 前端可读字符串
    const char* mode = (randomMode == RANDOM_OFF) ? "off"
                     : (randomMode == RANDOM_SOFT) ? "soft" : "normal";

    float temp = temperatureRead();  // 芯片内部温度

    // 拼 JSON: 网络 / 内存 / 温度 / 复位原因 / 在线数 / 模式 / 表情 / 转速 / 电机
    String j = "{";
    j += "\"ssid\":\"" + ssid + "\",";
    j += "\"rssi\":" + String(rssi) + ",";
    j += "\"ip\":\"" + ip + "\",";
    j += "\"wifimode\":\"" + String(sta ? "STA" : "AP") + "\",";
    j += "\"uptime\":" + String(millis()) + ",";
    j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"psram\":" + String(ESP.getFreePsram()) + ",";
    j += "\"temp\":" + String(temp, 1) + ",";
    j += "\"rst\":" + String((int)esp_reset_reason()) + ",";
    j += "\"minheap\":" + String(ESP.getMinFreeHeap()) + ",";
    j += "\"wscam\":" + String((int)wsCamConnCount) + ",";
    j += "\"wsctrl\":" + String((int)wsCtrlConnCount) + ",";
    j += "\"apstas\":" + String(WiFi.softAPgetStationNum()) + ",";
    j += "\"mode\":\"" + String(mode) + "\",";
    j += "\"sleep\":" + String(sleepMode ? "true" : "false") + ",";
    j += "\"mood\":" + String((int)currentMood) + ",";
    j += "\"speed\":" + String((int)motorLevel()) + ",";
    j += "\"speedpct\":" + String((int)MOTOR_SPEED_PCT[motorLevel()]) + ",";
    j += "\"speedduty\":" + String((int)motorDuty()) + ",";
    j += "\"motor\":{\"cmd\":" + String(mc)
       + ",\"name\":\"" + String(mname) + "\""
       + ",\"active\":" + String(manualActive ? "true" : "false")
       + ",\"ago\":" + String(millis() - lastMotorCmdMs) + "}";
    j += "}";
    request->send(200, "application/json", j);
  });

  // ---- 摄像头与时间同步 ----
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/sync_time", HTTP_GET, handleSyncTime);

  // ---- 跳舞: 非阻塞状态机 ----
  server.on("/dance", HTTP_GET, [](AsyncWebServerRequest *request) {

    autoModePaused = true;
    randomMode = RANDOM_OFF;
    danceStart();
    request->send(200);
  });

  // ---- WiFi 扫描: 返回附近热点 JSON ----
  server.on("/wifi_scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    int n = WiFi.scanNetworks(false, false, false, 200);
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    request->send(200, "application/json", json);
  });

  // ---- 保存 WiFi 并重启进入 STA 模式 ----
  server.on("/wifi_save", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid")) {
      request->send(400, "text/plain", "Missing ssid");
      return;
    }
    String ssid = request->getParam("ssid")->value();
    String pwd  = request->hasParam("pwd") ? request->getParam("pwd")->value() : "";
    saveWiFiConfig(MODE_STA, ssid, pwd);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Connecting WiFi..."));
    display.println(ssid);
    display.display();
    request->send(200, "text/plain", "OK");
    delay(500);
    ESP.restart();
  });

  // ---- 清除 WiFi 配置并重启回 AP 热点 ----
  server.on("/wifi_ap", HTTP_GET, [](AsyncWebServerRequest *request) {
    clearWiFiConfig();
    request->send(200, "text/plain", "OK");
    delay(500);
    ESP.restart();
  });

  // ---- 重启: 先停电机, 再重启 ----
  server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Restarting...");
    motorAnimStop();
    motorWifi(0);
    delay(100);
    ESP.restart();
  });

  // ---- OTA 升级页面 ----
  // 直接发 PROGMEM, 支持 gzip 时发压缩版; 不再把 17KB 页面拷到 String 堆.
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendProgmemHtml(request,
                    (const uint8_t *)PAGE_HTML_OTA, strlen(PAGE_HTML_OTA),
                    PAGE_HTML_OTA_GZ, PAGE_HTML_OTA_GZ_LEN);
  });

  // ---- OTA 固件上传: 完成回调 + 数据流回调 ----
  // 完成回调: 检查 Update 是否出错, 成功则重启
  // 数据流回调: 第一块数据到达时 Update.begin(), 之后逐块写入, 最后 Update.end()
  server.on(
    "/update", HTTP_POST, [](AsyncWebServerRequest *request) {
      bool hasError = Update.hasError();
      if (hasError) {
        Serial.printf("OTA Error: %d\n", Update.getError());
        request->send(400, "text/plain", "Update failed");
      } else {
        Serial.println("OTA Success, restarting...");
        request->send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
      static size_t totalSize = 0;
      if (!index) {
        Serial.printf("OTA Update Start: %s\n", filename.c_str());
        totalSize = 0;

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Serial.printf("Update.begin failed: %d\n", Update.getError());
          return;
        }

        // 前端在 X-Firmware-MD5 头里带 32 位 MD5; 有就做完整性校验.
        // 注意: setMD5 必须在 Update.begin() 之后调用, 否则会被 begin 重置.
        const AsyncWebHeader *md5Hdr = request->getHeader("X-Firmware-MD5");
        if (md5Hdr) {
          String md5 = md5Hdr->value();
          md5.trim();
          md5.toLowerCase();
          if (Update.setMD5(md5.c_str())) {
            Serial.printf("OTA MD5: %s\n", md5.c_str());
          } else {
            Serial.println("OTA MD5 格式无效, 本次跳过 MD5 校验");
          }
        }
      }
      if (len) {
        size_t written = Update.write(data, len);
        totalSize += written;
        // 写入不完整说明 Flash 出错, 立即中止, 避免把半截固件当成成功.
        if (written != len) {
          Serial.printf("OTA write failed: %u/%u\n", (unsigned)written, (unsigned)len);
          Update.abort();
        }
      }
      if (final) {
        if (Update.end(true)) {
          Serial.printf("OTA Success: %u bytes\n", totalSize);
        } else {
          Serial.printf("Update.end failed: %d\n", Update.getError());
        }
      }
    });

  // ---- 未知路径统一回首页 (兼容前端路由/旧链接) ----
  server.onNotFound([](AsyncWebServerRequest *request) {
    handleRoot(request);
  });

  // ---- 注册两个 WebSocket ----
  wsCam.onEvent(onWsCamEvent);
  server.addHandler(&wsCam);

  wsCtrl.onEvent(onWsCtrlEvent);
  server.addHandler(&wsCtrl);

  // ---- 启动 HTTP 服务 ----
  server.begin();
}

/* ============================================================
 * OLED 文字显示
 * ------------------------------------------------------------
 * Web 发来的文字可能是纯 ASCII, 也可能是中文 UTF-8.
 *   - 纯 ASCII: 用 Adafruit 自带字体, 速度快
 *   - 含中文  : 切到 U8g2 的文泉驿 GB2312 点阵字体
 * ============================================================ */

// 返回 UTF-8 字符占用的字节数 (ASCII=1, 常见中文=3, 4 字节表情=4)
static inline int utf8CharBytes(unsigned char b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 1;
}

/* 含中文时的绘制:
 *   - 用 U8g2 的 getUTF8Width() 按像素宽度自动折行
 *   - 最多 4 行, 整体垂直居中
 *   - 外框圆角矩形
 */
void drawWebTextMessageUtf8() {
  u8g2.clearBuffer();
  u8g2.drawRFrame(0, 0, 128, 64, 6);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);

  const int padX = 5;
  const int maxW  = 128 - padX * 2;
  const int lineH = 14;
  const int maxLines = 4;

  // 先把文本按宽度拆成最多 maxLines 行
  String lines[maxLines];
  int  n = 0;
  String cur = "";
  for (int i = 0; textDisplayBuf[i] && n < maxLines; ) {
    int cb = utf8CharBytes((unsigned char)textDisplayBuf[i]);
    String ch = "";
    for (int k = 0; k < cb && textDisplayBuf[i + k]; k++) ch += textDisplayBuf[i + k];
    if (ch == "\n") {
      lines[n++] = cur; cur = ""; i += cb; continue;  // 手动换行
    }
    String test = cur + ch;
    if (cur.length() > 0 && u8g2.getUTF8Width(test.c_str()) > maxW) {
      lines[n++] = cur;
      cur = ch;
    } else {
      cur = test;
    }
    i += cb;
  }
  if (cur.length() > 0 && n < maxLines) lines[n++] = cur;

  // 逐行水平居中绘制
  int top = (64 - n * lineH) / 2;
  for (int l = 0; l < n; l++) {
    int w = u8g2.getUTF8Width(lines[l].c_str());
    int x = (128 - w) / 2;
    if (x < padX) x = padX;
    u8g2.drawUTF8(x, top + l * lineH + 11, lines[l].c_str());
  }
  u8g2.sendBuffer();
}

/* 统一入口: 检测文本里是否有非 ASCII 字节.
 * 有中文 → U8g2 通道; 纯英文/数字 → Adafruit 快速通道.
 */
void drawWebTextMessage() {

  bool hasUtf8 = false;
  for (int i = 0; textDisplayBuf[i]; i++) {
    if ((uint8_t)textDisplayBuf[i] >= 0x80) { hasUtf8 = true; break; }
  }
  if (hasUtf8) { drawWebTextMessageUtf8(); return; }

  // ---- 纯 ASCII 快速通道 ----
  display.clearDisplay();
  display.drawRoundRect(0, 0, 128, 64, 6, SSD1306_WHITE);

  // 统计字符数, 决定字号和每行容量
  int totalChars = 0;
  for (int i = 0; textDisplayBuf[i]; ) {
    i += utf8CharBytes((unsigned char)textDisplayBuf[i]);
    totalChars++;
  }
  if (totalChars == 0) { display.display(); return; }

  bool big   = (totalChars <= 10); // 短文本用大字号
  int  size  = big ? 2 : 1;
  int  charW = 6 * size;
  int  lineH = 8 * size;
  int  maxCol = big ? 9 : 20;
  int  maxRow = big ? 3 : 6;

  // 按列数折行, 记录每行在缓冲区里的起点和长度
  uint8_t lineStart[7], lineLen[7];
  int lines = 0, i = 0, col = 0, start = 0;
  while (textDisplayBuf[i] && lines < maxRow) {
    int cb = utf8CharBytes((unsigned char)textDisplayBuf[i]);
    if (col >= maxCol) {
      lineStart[lines] = start;
      lineLen[lines]   = i - start;
      lines++;
      start = i;
      col = 0;
    }
    i += cb;
    col++;
  }
  if (lines < maxRow && start < i) {
    lineStart[lines] = start;
    lineLen[lines]   = i - start;
    lines++;
  }

  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);

  // 整块文本垂直居中
  int y = 3 + (58 - lines * lineH) / 2;
  for (int l = 0; l < lines; l++) {

    int chars = 0;
    for (int k = lineStart[l]; k < lineStart[l] + lineLen[l]; ) {
      k += utf8CharBytes((unsigned char)textDisplayBuf[k]);
      chars++;
    }
    int x = (128 - chars * charW) / 2;
    if (x < 3) x = 3;
    display.setCursor(x, y + l * lineH);
    display.write((const uint8_t *)&textDisplayBuf[lineStart[l]], lineLen[l]);
  }
  display.display();
}

/* ============================================================
 * [7] FreeRTOS 双核任务
 * ============================================================ */

/* Core 0: OLED + 电机控制任务.
 *   - 执行 Web 入队的电机命令 (带 delay 的时序放这里, 不阻塞网络)
 *   - 显示 Web 文字 / 休眠时钟 / 眼睛动画
 *   - 推进随机巡航、跳舞、手动超时
 * 每 20ms 或收到 xTaskNotifyGive 时醒一次.
 */
void oledMotorTask(void *pvParameters) {
  while (1) {

    // ---- 处理 Web 入队的电机命令 ----
    if (motorCmdValid) {
      byte cmd = motorCmdPending;
      bool timed = motorCmdHasTimed;
      int ms = motorCmdTimedMs;
      motorCmdValid = false;
      lastMotorCmd = cmd;
      lastMotorCmdMs = millis();
      if (timed) motorWifi(cmd, ms);
      else       motorWifi(cmd);
    }

    // ---- 处理 Web 文字显示 (5 秒后自动消失) ----
    // 注意: 这里只冻结“屏幕画面”, 不能跳过下面的电机状态机,
    // 否则文字显示的 5 秒内, 手动超时/跳舞/巡航都会停摆.
    bool showingText = false;
    if (textDisplayActive) {
      if (millis() - textDisplayStartMs >= 5000) {
        textDisplayActive = false;
      } else {
        showingText = true;
        if (!textDisplayDrawn) {
          textDisplayDrawn = true;
          drawWebTextMessage();
        }
      }
    }

    // ---- 休眠模式显示时钟, 否则更新眼睛动画 (文字显示时暂停画面更新) ----
    if (!showingText) {
      if (sleepMode) {
        drawSleepScreen();
      } else {
        roboEyes.update();
      }
    }

    // ---- 推进随机巡航 / 跳舞状态机 ----
    motorAnimUpdate();

    danceUpdate();

    // ---- 手动控制超时: 200ms 没新命令自动停车 ----
    unsigned long now = millis();
    if (manualActive && now - lastManualTime >= MANUAL_TIMEOUT) {
      manualActive = false;
      motorWifi(0);
    }

    // ---- 手动结束 10 秒后恢复自动巡航 ----
    if (autoModePaused && now - lastManualTime >= AUTO_RESUME_DELAY) {
      autoModePaused = false;
    }

    // ---- 随机巡航生成 ----
    static unsigned long lastTick = 0;
    if (!manualActive && !anim.running && !autoModePaused && randomMode != RANDOM_OFF && now - lastTick > 40) {
      lastTick = now;
      if (randomMode == RANDOM_SOFT && random(120) == 1) {

        // 柔和模式: 触发少、动作短
        motorAnimStart(random(1, 9), random(6, 18), random(40, 90), 1);
      } else if (randomMode == RANDOM_NORMAL && random(100) == 1) {

        // 正常模式: 触发多、动作长
        motorAnimStart(random(1, 9), random(5, 50), random(10, 100), random(20));
      }
    }

    // ---- 等待新命令通知, 最多 20ms ----
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
}

/* Core 1: 摄像头采集 + 推流任务.
 *   - 按 CAM_STREAM_MAX_FPS 限速取帧
 *   - GRAB_LATEST 保证拿到最新画面
 *   - 写入 global_cam_buf 供 HTTP /stream /capture 使用
 *   - 通过 wsCam 推送二进制帧, 浏览器 ACK 后才推下一帧
 */
void cameraTask(void *pvParameters) {
  static uint32_t lastBigFrameWarn = 0;  // 大帧告警限流
  static uint32_t nextGrabMs = 0;        // 下一次取帧时间
  uint32_t sentFrames = 0, droppedFrames = 0, statStartMs = millis();

  while (1) {

    // 摄像头未初始化: 休眠等待
    if (!cameraInitialized) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // ---- 帧率限速 ----
    uint32_t now = millis();
    int32_t waitMs = (int32_t)(nextGrabMs - now);
    if (waitMs > 0) {
      vTaskDelay(pdMS_TO_TICKS(waitMs));
      continue;
    }
    nextGrabMs = now + (1000u / CAM_STREAM_MAX_FPS);

    // ---- 取一帧 ----
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      uint32_t t = millis();
      if (fb->len > CAM_BUF_MAX_SIZE) {
        // 帧太大: 丢弃并限流打印
        if (t - lastBigFrameWarn > 5000) {
          lastBigFrameWarn = t;
          Serial.printf("[CAM] WARN 帧过大 %uB > %uB, 已丢弃\n",
                        (unsigned)fb->len, (unsigned)CAM_BUF_MAX_SIZE);
        }
      } else {

        // ---- 复制到全局缓存, 供 HTTP 视频/抓拍读取 ----
        // 如果 PSRAM 分配失败或回退后的容量不够, 就跳过复制;
        // WebSocket 推流直接用 fb->buf, 不依赖这个全局缓存.
        if (global_cam_buf && fb->len <= global_cam_capacity
            && xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          memcpy(global_cam_buf, fb->buf, fb->len);
          global_cam_len = fb->len;
          global_cam_seq++;  // 发布新帧序号
          xSemaphoreGive(cam_mutex);
        }

        // ---- WebSocket 推流: 严格 ACK 背压 ----
        if (wsCamConnCount > 0 && wsCam.availableForWriteAll()) {

          // 上一帧已被 ACK, 或等待超时 → 允许发下一帧
          bool clientReady = !wsCamAwaitAck ||
                             (t - wsCamLastSendMs) >= CAM_WS_ACK_TIMEOUT_MS;
          if (clientReady) {
            if (wsCam.binaryAll(fb->buf, fb->len) != AsyncWebSocket::DISCARDED) {
              wsCamAwaitAck   = true;
              wsCamLastSendMs = t;
              sentFrames++;
            } else {
              droppedFrames++;  // 发送队列满, 丢帧
            }
          } else {
            droppedFrames++;    // 客户端还没消费完, 丢帧保延迟
          }
        }
      }
      esp_camera_fb_return(fb);  // 归还驱动帧缓冲
    }

    // ---- 每 5 秒打印一次推流统计 ----
    uint32_t tNow = millis();
    if (tNow - statStartMs >= 5000) {
      uint32_t dt = tNow - statStartMs;
      Serial.printf("[CAM] 推流 %u.%ufps (sent=%u drop=%u) q=%d\n",
                    (unsigned)(sentFrames * 1000 / dt),
                    (unsigned)((sentFrames * 10000 / dt) % 10),
                    (unsigned)sentFrames, (unsigned)droppedFrames, CAM_STREAM_JPEG_Q);
      sentFrames = droppedFrames = 0;
      statStartMs = tNow;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/* 开机最早期把 4 路电机输入锁成 OUTPUT+LOW.
 * 必须早于 WiFi 连接(可能阻塞十几秒)、开机动画、摄像头初始化,
 * 否则这些 GPIO 会悬空, H 桥输入漂移到高电平导致开机轮子自转.
 */
static inline void earlyMotorPinLockdown() {
  const int motorPins[4] = {LF, LB, RF, RB};
  for (int i = 0; i < 4; i++) {
    pinMode(motorPins[i], OUTPUT);
    digitalWrite(motorPins[i], LOW);
  }
}

/* ============================================================
 * 系统初始化 (按 Phase 1~5 顺序执行)
 *   Phase 1: 芯片信息、互斥锁、PSRAM 帧缓存、随机数
 *   Phase 2: 读取 WiFi 配置并连接, 启动 Web 服务
 *   Phase 3: OLED / U8g2 / 眼睛动画 / 开机动画
 *   Phase 4: 摄像头初始化
 *   Phase 5: 电机 PWM 初始化
 *   最后启动 Core 0 / Core 1 两个 RTOS 任务
 * ============================================================ */
void setup() {
  Serial.begin(115200);
  earlyMotorPinLockdown();  // 第一时间锁住电机脚
  motorPwmInit();           // 挂载 LEDC (通道 4~7)
  loadMotorSpeed();         // 恢复上次保存的转速档位
  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" [Phase 1/5] ESP32 CORE INIT                 "));
  Serial.println(F("============================================="));

  // ---- Phase 1.1: 芯片信息 ----
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  Serial.printf("  Chip     : ESP32-S3 rev %d, cores=%d, %s%s\n",
                chip.revision, chip.cores,
                (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
                (chip.features & CHIP_FEATURE_BLE) ? "BLE" : "");
  Serial.printf("  Free DRAM: %u bytes\n",  heap_caps_get_free_size(MALLOC_CAP_8BIT));
  Serial.printf("  Free PSRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("  CPU Freq : %u MHz\n", getCpuFrequencyMhz());

  // ---- Phase 1.2: 创建互斥锁 ----
  cam_mutex = xSemaphoreCreateMutex();
  motor_mutex = xSemaphoreCreateMutex();
  if (cam_mutex == NULL)   Serial.println(F("  [WARN] cam_mutex 创建失败!"));
  else                     Serial.println(F("  [ OK ] cam_mutex 已创建"));
  if (motor_mutex == NULL) Serial.println(F("  [WARN] motor_mutex 创建失败!"));
  else                     Serial.println(F("  [ OK ] motor_mutex 已创建"));

  // ---- Phase 1.3: 分配摄像头共享帧缓存 (优先 PSRAM) ----
  global_cam_buf = (uint8_t *)heap_caps_malloc(CAM_BUF_MAX_SIZE, MALLOC_CAP_SPIRAM);
  if (global_cam_buf != NULL) {
    global_cam_capacity = CAM_BUF_MAX_SIZE;
    Serial.printf("  [ OK ] PSRAM 图像缓冲区: %u bytes @ 0x%08X\n",
                  CAM_BUF_MAX_SIZE, (uint32_t)global_cam_buf);
  } else {
    const size_t fallbackSize = CAM_BUF_MAX_SIZE / 2;
    Serial.printf("  [WARN] PSRAM 不足, 回退 DRAM (减半: %u bytes)\n", fallbackSize);
    global_cam_buf = (uint8_t *)heap_caps_malloc(fallbackSize, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (global_cam_buf != NULL) {
      global_cam_capacity = fallbackSize;
    } else {
      global_cam_capacity = 0;
      Serial.println(F("  [ERR ] DRAM 缓冲区分配失败, HTTP 视频/抓拍不可用!"));
    }
  }

  // ---- Phase 1.4: 随机数种子 (跳舞/巡航用) ----
  randomSeed(esp_random());
  Serial.println(F("  [ OK ] RNG 已初始化"));

  delay(100);
  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" [Phase 2/5] WiFi + NETWORK                  "));
  Serial.println(F("============================================="));

  // ---- Phase 2.1: 读取 NVS 中的 WiFi 配置 ----
  loadWiFiConfig();

  // ---- Phase 2.2: 优先 STA, 失败回退 AP ----
  bool wifiConnected = (currentWiFiMode == MODE_STA && !sta_ssid.isEmpty()) ? connectWiFiSTA() : false;
  if (!wifiConnected) startAPMode();

  // ---- Phase 2.3: 启动 Web 服务器 ----
  setupServer();
  Serial.println(F("  [ OK ] AsyncWebServer 已启动 (port 80)"));

  delay(100);
  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" [Phase 3/5] OLED SCREEN + EYES              "));
  Serial.println(F("============================================="));

  // ---- Phase 3.1: I2C 总线 ----
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Serial.printf("  [ OK ] I2C 总线 (SDA=%d, SCL=%d) @ 400KHz\n", OLED_SDA, OLED_SCL);

  // ---- Phase 3.2: SSD1306 OLED ----
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.display();
  oledInitialized = true;
  Serial.println(F("  [ OK ] SSD1306 OLED 128x64 初始化完成"));

  // ---- Phase 3.3: U8g2 中文通道 ----
  // 注意: u8g2.begin() 内部会无参调用 Wire.begin(), 可能把 I2C 引脚复位,
  // 所以 begin 之后必须重新指回 OLED_SDA/OLED_SCL.
  u8g2.begin();
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Serial.println(F("  [ OK ] U8g2 中文渲染通道就绪 (wqy12 GB2312)"));

  // ---- Phase 3.4: 眼睛动画 + 开机动画 ----
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  Serial.println(F("  [ OK ] RoboEyes 已加载"));

  bootAnimation();
  Serial.println(F("  [ OK ] 开机动画播放完毕"));

  delay(100);
  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" [Phase 4/5] CAMERA MODULE                   "));
  Serial.println(F("============================================="));

  // ---- Phase 4: 摄像头 ----
  initCamera();

  delay(100);
  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" [Phase 5/5] MOTOR DRIVER MODULE             "));
  Serial.println(F("============================================="));

  // ---- Phase 5: 电机 PWM 已就绪, 这里只重申 0% ----
  motorPwmAllOff();
  Serial.printf("  [ OK ] 电机 PWM 配置完成 (LF=%d LB=%d RF=%d RB=%d, 20kHz/8bit, DRIVE=MAX)\n",
                LF, LB, RF, RB);
  delay(30);
  Serial.printf("  [ OK ] 电机驱动模块就绪 (DRV8833, 当前档位 %s %u%%)\n",
                MOTOR_SPEED_TAG[motorLevel()], MOTOR_SPEED_PCT[motorLevel()]);

  // ---- 启动双核 RTOS 任务 ----
  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" START DUAL-CORE RTOS TASKS                  "));
  Serial.println(F("============================================="));

  // Core 0: OLED + 电机控制 (优先级 1)
  xTaskCreatePinnedToCore(
    oledMotorTask,
    "OledMotorTask",
    8192,
    NULL,
    1,
    &OledMotorTaskHandle,
    0
  );
  Serial.println(F("  [ OK ] OledMotorTask  -> Core 0"));

  // Core 1: 摄像头采集/推流 (优先级 2, 略高保证取帧)
  xTaskCreatePinnedToCore(
    cameraTask,
    "CameraTask",
    8192,
    NULL,
    2,
    &CameraTaskHandle,
    1
  );
  Serial.println(F("  [ OK ] CameraTask     -> Core 1"));

  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" BOOT SEQUENCE COMPLETE - SYSTEM ONLINE      "));
  Serial.println(F("============================================="));
}

/* 主循环 (Core 1 空闲守护):
 *   - 处理 AP 模式 DNS 请求
 *   - 清理断开的 WebSocket 客户端
 *   - STA 断线超过 60 秒时自动切回 AP, 防止失联
 */
void loop() {
  dnsServer.processNextRequest();

  // 清理掉线客户端 (会触发 WS_EVT_DISCONNECT → 自动停车)
  wsCtrl.cleanupClients();
  wsCam.cleanupClients();

  // ---- WiFi 断线看门狗 ----
  // 用 WiFi.getMode() 而不是 currentWiFiMode:
  // 开机 STA 失败回退 AP 时 currentWiFiMode 仍是 STA, 会误判.
  static unsigned long wifiLostMs = 0;
  if (WiFi.getMode() == WIFI_STA) {
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiLostMs == 0) wifiLostMs = millis();
      else if (millis() - wifiLostMs >= 60000) {
        Serial.println(F("[WIFI] STA 断线超 60s, 自动切回 AP 模式"));
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println(F("WiFi Lost!"));
        display.println(F("Switching to AP..."));
        display.display();
        clearWiFiConfig();
        delay(500);
        ESP.restart();
      }
    } else {
      wifiLostMs = 0;
    }
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
