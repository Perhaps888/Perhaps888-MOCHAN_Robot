#pragma once
#include "../config.h"
#include "../utils/diag.h"

static inline void initCamera() {
  phaseStart("④CAM");
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_d7=Y9_GPIO_NUM; c.pin_d6=Y8_GPIO_NUM; c.pin_d5=Y7_GPIO_NUM; c.pin_d4=Y6_GPIO_NUM;
  c.pin_d3=Y5_GPIO_NUM; c.pin_d2=Y4_GPIO_NUM; c.pin_d1=Y3_GPIO_NUM; c.pin_d0=Y2_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.xclk_freq_hz = 20000000; c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_HQVGA; c.jpeg_quality = 12; c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM; c.grab_mode = CAMERA_GRAB_LATEST;
  // PSRAM 优先
  if(psramFound()){ c.fb_count=2; c.jpeg_quality=12; }
  esp_err_t e = esp_camera_init(&c);
  if (e!=ESP_OK) {
    phaseFAIL("④CAM", "esp_camera_init() 失败: %s (每 10s 重试)", esp_err_to_name(e));
    cameraInitialized=false;
    return;
  }
  // 共享缓冲（PSRAM → DRAM 回退）
  globalCamBuf = (uint8_t*)heap_caps_malloc(CAM_BUF_MAX_SIZE, MALLOC_CAP_SPIRAM);
  if (!globalCamBuf) {
    globalCamBuf = (uint8_t*)heap_caps_malloc(CAM_BUF_MAX_SIZE/2, MALLOC_CAP_8BIT);
    phaseOK("④CAM", "就绪 (共享缓冲=%uB DRAM, psramFound=%d)", globalCamBuf?CAM_BUF_MAX_SIZE/2:0, psramFound());
  } else {
    phaseOK("④CAM", "就绪 (共享缓冲=%uKB PSRAM)", CAM_BUF_MAX_SIZE/1024);
  }
  globalCamLen = 0;
  cameraInitialized = (globalCamBuf!=nullptr);
}

static uint32_t camMutFailLastTs=0;
// Camera Task Core 1：登记 TWDT + 抓帧到共享缓冲 + C6 Mutex 超时限流打印
static inline void cameraTaskBody(void *) {
  esp_task_wdt_add(NULL);
  for(;;){
    if (!cameraInitialized || !globalCamBuf) { vTaskDelay(pdMS_TO_TICKS(500)); esp_task_wdt_reset(); continue; }
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb && fb->len>0) {
      size_t n = (fb->len<CAM_BUF_MAX_SIZE)?fb->len:CAM_BUF_MAX_SIZE;
      if (xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(globalCamBuf, fb->buf, n);
        globalCamLen = n;
        xSemaphoreGive(cam_mutex);
      } else {
        // C6 限流：5 分钟一次
        if (diagRateLimit(camMutFailLastTs)) {
          DIAG_PRINTF("[CAM] Mutex 拿不到 10ms 超时 (task=CameraTask @%lu)\n", millis());
        }
      }
    }
    if (fb) esp_camera_fb_return(fb);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));  // ~20 FPS 足够
    esp_task_wdt_reset();
  }
}
