#pragma once
#include "../config.h"
#include "motor_control.h"

// ============ U8g2 原生 SSD1306 + RobotEye 全局对象 (定义在 reboot.ino) ============
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern RobotEye roboEyes;

static bool isBootPlaying = false;

// 原 reboot.ino 内联 getCurrentTime() (与 oled_ui.h 解耦时使用)
static inline uint32_t getCurrentTime() { return millis(); }

// ==================== bootAnimation (简化占位 —— 完整版位于 reboot.ino) ====================
// 若某些模块单独 include oled_ui.h 并想快速播放启动标识, 调用此函数
static inline void bootAnimation() {
  isBootPlaying = true;
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_9x15B_tf);
  u8g2.setCursor(8, 20 + 15);  u8g2.print("MOCHAN");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(32, 45 + 10); u8g2.print("ROBOT v1.0");
  u8g2.sendBuffer();
  delay(300);
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  isBootPlaying = false;
}

// ==================== drawSleepScreen (休眠) ====================
// 直接使用 RobotEye 内置 EXPR_SLEEPY 表情 (眼皮下垂 + 顶部斜坡)
// 若 reboot.ino 已实现完整版则覆盖此 inline 版本
static inline void drawSleepScreen() {
  roboEyes.setExpression(RobotEye::EXPR_SLEEPY);
  roboEyes.update();
}

// ==================== Phase 3 OLED 初始化 (U8g2 + RobotEye) ====================
// (若 setup() 已自行完成初始化, 请勿重复调用; 此函数用于备用初始化路径)
static inline void beginOled() {
  phaseStart("③OLED");
  Wire.begin(SDA, SCL);
  if (!u8g2.begin(OLED_RESET, SCL, SDA)) {
    phaseFAIL("③OLED", "U8g2 SSD1306 @0x3C 没找到 (SDA=%d SCL=%d)", SDA, SCL);
    oledInitialized = false;
    return;
  }
  oledInitialized = true;
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  roboEyes.begin(0);
  bootAnimation();
  phaseOK("③OLED", "U8g2 SSD1306 %dx%d @I2C 0x3C, RobotEye OK", SCREEN_WIDTH, SCREEN_HEIGHT);
}

// ==================== Core 0: OLED + Motor 组合任务 (C2 TWDT 登记) ====================
// U8g2 + RobotEye 版本:
//   • roboEyes.update() 内部推进状态机 + 绘制 + 调用 u8g2.sendBuffer() → 无需额外 display.display()
//   • 文字 / 休眠模式由外层 oledMotorTask() (reboot.ino) 分支优先处理, 这里只做普通眼球回退
static inline void oledMotorTaskBody(void *) {
  esp_task_wdt_add(NULL);
  for (;;) {
    // OLED 渲染 (仅在非开机动画期间)
    if (oledInitialized && !isBootPlaying) {
      roboEyes.update();
      esp_task_wdt_reset();
      esp_task_wdt_reset();
    }
    // 手动连发 MANUAL_TIMEOUT 到期自动停电机
    if (manualActive && (millis() - manualTimeoutMs >= MANUAL_TIMEOUT)) {
      motorStopAll();
      manualActive = false;
    }
    // 跳舞巡航帧
    if (anim.running) motorAnimUpdate();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(33));  // 30 FPS
    esp_task_wdt_reset();
  }
}
