#pragma once
// ============================================================
//  MOCHAN Robot — Global Config & Include Gatekeeper
//  所有模块必须先 include 本文件，任何第三方库只在这里 include 一次
//  切换到 U8g2 原生 SSD1306 驱动 + fonzu/RobotEye 矢量眼睛动画库
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "web_content.h"           // OneUI PROGMEM 原版
#include "web_content_gz.h"        // B1 预 gzip
#include "esp_camera.h"
#include "fb_gfx.h"
#include "esp_wifi.h"
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
// ---- U8g2 原生 SSD1306 驱动 (替换 Adafruit_SSD1306 + Adafruit_GFX) ----
#include <U8g2lib.h>
// ---- RobotEye 矢量眼睛动画库 (替换 FluxGarage_RoboEyes) ----
//   库来源: 文档/Arduino/libraries/RobotEye (Arduino 库管理器安装, 含 robot_eye.cpp 实现)
//   注意: 已在库头 robot_eye.hpp 禁用 RE_ENABLE_BITMAP_EXPR, 无需 Irisoled 位图资源
#include <RobotEye.h>
#include <functional>
#include <cstdint>
#include <cstring>

// ============================================================
//  [CONFIG] 用户可改的唯一入口
// ============================================================

// --- 电机引脚 ---
#define LF 47
#define LB 21
#define RF 42
#define RB 45
#define MOTOR_DRIVE_STRENGTH GPIO_DRIVE_CAP_3

// --- 方向极性开关 ---
#define INVERT_LEFT_MOTOR   false
#define INVERT_RIGHT_MOTOR  false
#define SWAP_LEFT_RIGHT_MOTOR false

// --- OLED SSD1306 (I2C) ---
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    U8X8_PIN_NONE  // U8g2 风格: 无硬件复位脚
#define SDA           40
#define SCL           41

// --- 摄像头 DVP (ESP32-S3 CAM) ---
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM        8
#define Y3_GPIO_NUM        9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13
#define CAM_BUF_MAX_SIZE (64*1024)

// --- WiFi ---
#define MANUAL_WIFI_SSID      "MOCHAN"
#define MANUAL_WIFI_PASSWORD  "88888888"
#define AP_CHANNEL            6
#define AP_STATIC_IP          IPAddress(192,168,4,1)
#define AP_GATEWAY            IPAddress(192,168,4,1)
#define AP_SUBNET             IPAddress(255,255,255,0)
#define AUTH_USERNAME         "admin"
#define WIFI_SSID_MAX_LEN     32
#define WIFI_PWD_MIN_LEN      8
#define WIFI_PWD_MAX_LEN      63
#define STA_RECONNECT_START_S 5
#define STA_RECONNECT_MAX_S   60

// --- OTA ---
#define OTA_MD5_HEADER        "X-Firmware-MD5"

// --- 诊断打印 ---
#define DIAG_SILENT           false
#if DIAG_SILENT
#  define DIAG_PRINTF(...)   ((void)0)
#else
#  define DIAG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#endif

// --- 电机 ---
#define MOTOR_DEBOUNCE_MS     30
#define MOTOR_DEADBAND_MS     2UL
#define MANUAL_TIMEOUT        240

// --- FreeRTOS ---
#define TASK_STACK_4K         (4*1024)
#define CORE_MOTOR_OLED       0
#define CORE_CAMERA           1
#define TWDT_TIMEOUT_MOTOR_MS 500
#define TWDT_TIMEOUT_CAM_MS   2000

// --- 安全 C5 ---
#define AUTH_FAIL_MAX         5
#define AUTH_BLOCK_MS         60000UL

// ============================================================
//  Enums
// ============================================================
enum : int { MODE_AP = 0, MODE_STA = 1 };
enum : byte { CMD_STOP=0, CMD_FWD=1, CMD_BACK=2, CMD_LEFT=3, CMD_RIGHT=4,
              CMD_LBACK=5, CMD_RBACK=6, CMD_LFWD=7, CMD_RFWD=8 };

// ============================================================
//  共享结构
// ============================================================
struct MotorAnim {
  bool     running;
  byte     frame;
  uint32_t lastTickMs;
  byte     seq[32];
  byte     seqLen;
  uint16_t frameMs;
};

// ============================================================
//  全局对象 extern 声明
// ============================================================
extern AsyncWebServer   server;
extern DNSServer        dnsServer;
extern Preferences      prefs;
extern SemaphoreHandle_t motor_mutex;
extern SemaphoreHandle_t cam_mutex;
extern MotorAnim        anim;
extern bool             cameraInitialized;
extern bool             oledInitialized;
extern bool             manualActive;
extern uint32_t         manualTimeoutMs;
extern String           sta_ssid;
extern String           sta_password;
extern int              currentWiFiMode;
extern volatile int     authFailCount;
extern volatile uint32_t authBlockUntilMs;
extern uint8_t         *globalCamBuf;
extern size_t           globalCamLen;
