#pragma once
#include "../config.h"
#include "esp_task_wdt.h"

// ---- ASCII Banner ----
static inline void printBootBanner() {
  Serial.println();
  Serial.println(F("  __  __  ___  ____ _   _    _    _   _ "));
  Serial.println(F(" |  \\/  |/ _ \\/ ___| | | |  / \\  | \\ | |"));
  Serial.println(F(" | |\\/| | | | \\___ \\ |_| | / _ \\ |  \\| |"));
  Serial.println(F(" | |  | | |_| |___) |  _  |/ ___ \\| |\\  |"));
  Serial.println(F(" |_|  |_|\\___/|____/|_| |_/_/   \\_\\_| \\_|"));
  Serial.println(F("   MOCHAN ROBOT · ESP32-S3 CAM + OLED"));
  Serial.println(F("==========================================="));
}

// ---- Phase 横幅通用模板 ----
static inline uint32_t _phaseStart = 0;
static inline void phaseStart(const char *name) {
  DIAG_PRINTF("[ .... ] Phase %s 开始...\n", name);
  _phaseStart = millis();
}
template<typename ...Args>
static inline void phaseOK(const char *name, const char *fmt="完成", Args&&... args) {
  uint32_t dt = millis()-_phaseStart;
  Serial.printf("[ OK ] Phase %s: ", name);
  Serial.printf(fmt, std::forward<Args>(args)...);
  Serial.printf(" (%lu ms)\n", dt);
}
template<typename ...Args>
static inline void phaseFAIL(const char *name, const char *fmt, Args&&... args) {
  Serial.printf("[FAIL] Phase %s: ", name);
  Serial.printf(fmt, std::forward<Args>(args)...);
  Serial.println();
}

// ---- 芯片 / 内存信息 ----
static inline void printChipInfo() {
  esp_chip_info_t info; esp_chip_info(&info);
  const char *model = "ESP32-?";
  switch(info.model){ case CHIP_ESP32:   model="ESP32";   break;
    case CHIP_ESP32S2: model="ESP32-S2"; break; case CHIP_ESP32S3: model="ESP32-S3"; break;
    case CHIP_ESP32C3: model="ESP32-C3"; break; case CHIP_ESP32H2: model="ESP32-H2"; break; }
  DIAG_PRINTF("[CHIP] %s rev%u cores=%u WiFi%s%s%s PSRAM=%uKB DRAM-free=%uKB\n",
    model, info.revision, info.cores,
    (info.features&CHIP_FEATURE_WIFI_BGN)?" BGN":"",
    (info.features&CHIP_FEATURE_BLE)?" BLE":"",
    (info.features&CHIP_FEATURE_EMB_FLASH)?" eFlash":"",
    heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024,
    heap_caps_get_free_size(MALLOC_CAP_8BIT)/1024);
}

// ---- [MOTOR] 诊断（带命令名）----
static inline void diagMotor(byte cmd, int lf, int lb, int rf, int rb) {
#if DIAG_SILENT
  (void)cmd;(void)lf;(void)lb;(void)rf;(void)rb; return;
#else
  const char *N[]={"STOP","FWD","BACK","LEFT","RIGHT","L-BACK","R-BACK","L-FWD","R-FWD","?"};
  int n = (cmd<9)?cmd:9;
  DIAG_PRINTF("[MOTOR] t=%7lu  cmd=%-5s(%d)  LF(GPIO%d)=%d LB(GPIO%d)=%d  RF(GPIO%d)=%d RB(GPIO%d)=%d\n",
    millis(), N[n], cmd, LF,lf, LB,lb, RF,rf, RB,rb);
#endif
}

// ---- 限流工具：同 msgId 5 分钟只打一次（防刷串口）----
static inline bool diagRateLimit(uint32_t &lastTs, uint32_t intervalMs=300000UL) {
  uint32_t now = millis();
  if (now - lastTs < intervalMs) return false;
  lastTs = now; return true;
}
