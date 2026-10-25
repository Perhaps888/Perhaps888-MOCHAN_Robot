#pragma once
#include "../config.h"
#include "wifi_manager.h"     // requestBasicAuth / WiFi 路由 + onAuthFailInc 等
#include "../hw/motor_control.h"
#include "../hw/camera_manager.h"
#include "../utils/diag.h"

// ---- B2: Chunked 1024B 分片从 PROGMEM 直出（0 堆分配）----
static void sendProgmemChunked(AsyncWebServerRequest *req,
                               const char *contentType,
                               const uint8_t *data, size_t len,
                               bool gzipped=false) {
  AsyncWebServerResponse *resp = req->beginResponse(200, contentType, NULL, len);
  if (!resp) { req->send(500,"text/plain","oom"); return; }
  if (gzipped) { resp->addHeader("Content-Encoding","gzip"); resp->addHeader("Vary","Accept-Encoding"); }
  resp->addHeader("Cache-Control","public, max-age=3600");
  size_t *pOffset = new size_t(0);
  resp->onBody([data,len,pOffset](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
    (void)index;
    size_t &offset = *pOffset;
    if (offset>=len) { delete pOffset; return 0; }
    size_t n = (len-offset<maxLen)?(len-offset):maxLen;
    memcpy_P(buf, data+offset, n);
    offset += n;
    return n;
  });
  req->send(resp);
}

// 辅助：检测 Accept-Encoding gzip
static inline bool acceptsGzip(AsyncWebServerRequest *r) {
  if (!r->hasHeader("Accept-Encoding")) return false;
  return r->getHeader("Accept-Encoding")->value().indexOf("gzip") >= 0;
}

// ---- C5: wrap 路由函数为带 Auth 的版本 ----
template<typename Fn>
static auto wrapAuth(Fn fn) {
  return [fn](AsyncWebServerRequest *r){
    if (!requestBasicAuth(r)) return;
    fn(r);
  };
}

// ============================================================
//  路由回调
// ============================================================
static void handleRoot(AsyncWebServerRequest *r) {
  bool gz = acceptsGzip(r);
  if (gz) sendProgmemChunked(r,"text/html", PAGE_HTML_MAIN_GZ, PAGE_HTML_MAIN_GZ_LEN, true);
  else    sendProgmemChunked(r,"text/html", (const uint8_t*)PAGE_HTML_MAIN, strlen(PAGE_HTML_MAIN), false);
}

static void handlePing(AsyncWebServerRequest *r) {
  String s; s.reserve(160);
  s = F("MOCHAN-ROBOT-ESP32-S3\r\nv1.0\r\nPONG: ");
  s += String(millis());
  s += "\r\nRAM free: "; s += String(ESP.getFreeHeap());
  r->send(200, "text/plain", s);
}

static void handleDir(AsyncWebServerRequest *r, byte cmd) {
  if (!requestBasicAuth(r)) return;
  motorWifi(cmd);
  r->send(204);  // No Content（fast-path, 响应体 0 B）
}
static void h_f(AsyncWebServerRequest *r){ handleDir(r,CMD_FWD); }
static void h_b(AsyncWebServerRequest *r){ handleDir(r,CMD_BACK); }
static void h_l(AsyncWebServerRequest *r){ handleDir(r,CMD_LEFT); }
static void h_r(AsyncWebServerRequest *r){ handleDir(r,CMD_RIGHT); }
static void h_s(AsyncWebServerRequest *r){ if (!requestBasicAuth(r)) return; motorStopAll(); r->send(204); }

static void handleDance(AsyncWebServerRequest *r) {
  if (!requestBasicAuth(r)) return;
  static const byte seq[] PROGMEM = {
    CMD_FWD, CMD_FWD, CMD_RIGHT, CMD_LEFT, CMD_BACK, CMD_BACK,
    CMD_RFWD, CMD_LFWD, CMD_RBACK, CMD_LBACK, CMD_STOP
  };
  if (anim.running) motorAnimStop();
  else motorAnimStart(seq, sizeof(seq), 450);
  r->send(200,"application/json", anim.running?"{\"dance\":1}":"{\"dance\":0}");
}

static void handleModeSet(AsyncWebServerRequest *r, int m) {
  if (!requestBasicAuth(r)) return;
  DIAG_PRINTF("[MODE] set %d\n", m);
  r->send(200,"application/json","{\"ok\":true}");
}
static void h_m0(AsyncWebServerRequest *r){ handleModeSet(r,0); }
static void h_m1(AsyncWebServerRequest *r){ handleModeSet(r,1); }
static void h_m2(AsyncWebServerRequest *r){ handleModeSet(r,2); }
static void h_m3(AsyncWebServerRequest *r){ handleModeSet(r,3); }

// C6: /capture Mutex 超时限流打印
static uint32_t capMutFailLastTs=0;
static void handleCapture(AsyncWebServerRequest *r) {
  if (!requestBasicAuth(r)) return;
  if (!cameraInitialized) return r->send(415,"text/plain","camera init failed");
  if (xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    if (diagRateLimit(capMutFailLastTs)) DIAG_PRINTF("[CAM] /capture Mutex 超时 @%lu\n", millis());
    return r->send(503,"text/plain","camera buffer locked, retry");
  }
  if (globalCamLen == 0 || !globalCamBuf) {
    xSemaphoreGive(cam_mutex);
    return r->send(503,"text/plain","no frame yet");
  }
  size_t *pOff = new size_t(0);
  AsyncWebServerResponse *resp = r->beginResponse(200,"image/jpeg",NULL,globalCamLen);
  if (!resp) { delete pOff; xSemaphoreGive(cam_mutex); return r->send(500); }
  resp->onBody([pOff](uint8_t *b,size_t max,size_t idx)->size_t{
    (void)idx;
    size_t &off = *pOff;
    if (off>=globalCamLen) {
      delete pOff;
      if (cam_mutex) xSemaphoreGive(cam_mutex);
      return 0;
    }
    size_t n = (globalCamLen-off<max)?(globalCamLen-off):max;
    memcpy(b, globalCamBuf+off, n); off+=n; return n;
  });
  r->send(resp);
}

// 404: 按 Accept 头分 JSON/HTML
static void handleNotFound(AsyncWebServerRequest *r) {
  bool wantJson = r->hasHeader("Accept") && r->getHeader("Accept")->value().indexOf("application/json")>=0;
  if (wantJson) r->send(404,"application/json","{\"ok\":false,\"error\":\"not found\"}");
  else {
    r->send(404,"text/html",
      "<!doctype html><meta charset=utf-8><title>404</title>"
      "<body style='font:14px system-ui;background:#0B1020;color:#fff;"
      "display:grid;place-items:center;height:100vh;margin:0'>"
      "<div><h1>404 · 你找的页面不存在</h1></div>");
  }
}

// ============================================================
//  C4 路由表（新增路由只改这里一行）
// ============================================================
struct RouteDef {
  const char *path;
  WebRequestMethodComposite method;
  void (*fn)(AsyncWebServerRequest*);
  bool needAuth;  // true = 套 wrapAuth
};
static const RouteDef ROUTES[] = {
  {"/",              HTTP_GET,  handleRoot,      false},
  {"/ping",          HTTP_GET,  handlePing,      false},
  {"/f",             HTTP_GET,  h_f,             true },
  {"/b",             HTTP_GET,  h_b,             true },
  {"/l",             HTTP_GET,  h_l,             true },
  {"/r",             HTTP_GET,  h_r,             true },
  {"/s",             HTTP_GET,  h_s,             true },
  {"/dance",         HTTP_GET,  handleDance,     true },
  {"/mode_0",        HTTP_GET,  h_m0,            true },
  {"/mode_1",        HTTP_GET,  h_m1,            true },
  {"/mode_2",        HTTP_GET,  h_m2,            true },
  {"/mode_3",        HTTP_GET,  h_m3,            true },
  {"/capture",       HTTP_GET,  handleCapture,   true },
  {"/scan",          HTTP_GET,  handleScanWifi,  true },
  {"/save_wifi",     HTTP_POST, handleSaveWifi,  true },
  {"/set_wifi_mode", HTTP_POST, handleSetMode,   true },
  {"/get_wifi_mode", HTTP_GET,  handleGetMode,   false},
};

static inline void setupRoutes(AsyncWebServer &srv) {
  for (unsigned i=0;i<sizeof(ROUTES)/sizeof(ROUTES[0]);i++){
    const RouteDef &def = ROUTES[i];
    auto wrapped = def.needAuth ? wrapAuth(def.fn) :
      (std::function<void(AsyncWebServerRequest*)>)def.fn;
    srv.on(def.path, def.method, wrapped);
  }
  srv.onNotFound(handleNotFound);
}
