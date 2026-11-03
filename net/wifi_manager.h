#pragma once
#include "../config.h"
#include "../utils/diag.h"
#include "../utils/json_escape.h"
#include "../utils/str_buf.h"

// 掩码：SSID 只显示前 3 位后加 **，密码全掩码
static inline String maskSsid(const String &s){
  if (s.length()<=3) return "***";
  return s.substring(0,3)+"***";
}
static inline String maskPwd(const String &s){ (void)s; return String("********"); }

// ---- Preferences 读写 ----
static inline void loadWiFiConfig() {
  phaseStart("②WiFi-loadCfg");
  if (!prefs.begin("wifi_cfg", true)) { phaseFAIL("②WiFi-loadCfg","prefs 打开失败"); return; }
  sta_ssid = prefs.getString("ssid","");
  sta_password = prefs.getString("pwd","");
  prefs.end();
  phaseOK("②WiFi-loadCfg", "SSID=%s PWD=%s (len=%u)",
    maskSsid(sta_ssid).c_str(), maskPwd(sta_password).c_str(), (unsigned)sta_password.length());
}
static inline void saveWiFiConfig(const String &ssid, const String &pwd) {
  prefs.begin("wifi_cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pwd", pwd);
  prefs.end();
  DIAG_PRINTF("[WIFI] 配置已保存 SSID=%s len(pwd)=%u\n", maskSsid(ssid).c_str(), (unsigned)pwd.length());
}

// ---- STA 指数退避连接 5s→60s ----
static inline bool connectWiFiSTA() {
  phaseStart("②WiFi-STA");
  if (sta_ssid.length()==0 || sta_password.length()<WIFI_PWD_MIN_LEN) {
    phaseFAIL("②WiFi-STA", "SSID空或密码<%u位，跳过", WIFI_PWD_MIN_LEN); return false;
  }
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  uint8_t backoff = STA_RECONNECT_START_S;
  for (int attempt=1; attempt<=6; attempt++){
    DIAG_PRINTF("[WIFI] STA 第 %d 次尝试 SSID=%s (超时 %us)\n", attempt, maskSsid(sta_ssid).c_str(), backoff);
    WiFi.begin(sta_ssid, sta_password);
    wl_status_t st = (wl_status_t)WiFi.waitForConnectResult(backoff*1000);
    if (st == WL_CONNECTED) {
      phaseOK("②WiFi-STA", "IP=%s RSSI=%ddBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      currentWiFiMode = MODE_STA; return true;
    }
    DIAG_PRINTF("[WIFI] STA 失败 status=%u，退避 %us 后重试\n", (unsigned)st, backoff);
    if (backoff < STA_RECONNECT_MAX_S) backoff = (backoff*2>STA_RECONNECT_MAX_S)?STA_RECONNECT_MAX_S:backoff*2;
  }
  phaseFAIL("②WiFi-STA", "6 次全部失败，切 AP 模式"); return false;
}

// ---- AP 静态 IP + 信道 6 ----
static inline void startAPMode() {
  phaseStart("②WiFi-AP");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_STATIC_IP, AP_GATEWAY, AP_SUBNET);
  bool ok = WiFi.softAP(MANUAL_WIFI_SSID, MANUAL_WIFI_PASSWORD, AP_CHANNEL, 0, 4);
  if (!ok) { phaseFAIL("②WiFi-AP", "softAP() 失败"); return; }
  dnsServer.start(53, "*", AP_STATIC_IP);
  phaseOK("②WiFi-AP", "SSID=%s PW=%s CH=%d IP=%s (CaptivePortal DNS *→AP)",
    MANUAL_WIFI_SSID, MANUAL_WIFI_PASSWORD, AP_CHANNEL,
    WiFi.softAPIP().toString().c_str());
  currentWiFiMode = MODE_AP;
}

// ---- C5: Basic Auth 失败计数 + 60s 封禁 ----
static inline bool checkAuthBlock(AsyncWebServerRequest *req) {
  if (authFailCount >= AUTH_FAIL_MAX && millis() < authBlockUntilMs) {
    String ip = req->client()->remoteIP().toString();
    DIAG_PRINTF("[SEC] Auth brute-force blocked %u s remaining (IP=%s)\n",
      (unsigned)((authBlockUntilMs-millis())/1000), ip.c_str());
    req->send(403, "text/plain", "Auth blocked, try later.");
    return false;
  }
  if (millis() >= authBlockUntilMs) { authFailCount = 0; }
  return true;
}
static inline void onAuthFailInc() {
  if (++authFailCount >= AUTH_FAIL_MAX) authBlockUntilMs = millis() + AUTH_BLOCK_MS;
}
static inline void onAuthSuccessClear() { authFailCount = 0; }

static inline bool requestBasicAuth(AsyncWebServerRequest *req) {
  if (!checkAuthBlock(req)) return false;
  if (req->authenticate(AUTH_USERNAME, MANUAL_WIFI_PASSWORD)) { onAuthSuccessClear(); return true; }
  onAuthFailInc();
  return req->requestAuthentication();  // 401 + WWW-Authenticate
}

// ---- WiFi 路由处理（/scan /save /set_mode /get_mode）----
static inline void handleScanWifi(AsyncWebServerRequest *r) {
  if (!requestBasicAuth(r)) return;
  int n = WiFi.scanComplete();
  if (n==WIFI_SCAN_RUNNING || n==-1) {
    WiFi.scanNetworks(true);
    return r->send(202, "application/json", "{\"scanning\":true}");
  }
  WiFi.scanDelete(); n = WiFi.scanNetworks();
  StrBuf j(n*90+64); j.concat("{\"ssids\":[");
  for (int i=0;i<n;i++){
    if (i) j.concat(",");
    j.catf("{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d,\"ch\":%d}",
      jsonEscape(WiFi.SSID(i)).c_str(), WiFi.RSSI(i),
      (int)WiFi.encryptionType(i), WiFi.channel(i));
  }
  j.concat("]}"); WiFi.scanDelete(); WiFi.scanNetworks(true);
  r->send(200, "application/json", j);
}

static inline void handleSaveWifi(AsyncWebServerRequest *r) {
  if (!requestBasicAuth(r)) return;
  String s = r->hasArg("ssid")?r->arg("ssid"):"";
  String p = r->hasArg("pwd") ?r->arg("pwd") :"";
  if (s.length()==0 || s.length()>WIFI_SSID_MAX_LEN
      || p.length()<WIFI_PWD_MIN_LEN || p.length()>WIFI_PWD_MAX_LEN) {
    return r->send(400,"application/json","{\"ok\":false,\"msg\":\"密码长度8-63 / SSID非空≤32\"}");
  }
  saveWiFiConfig(s,p);
  r->send(200,"application/json","{\"ok\":true,\"msg\":\"已保存，重启后生效\"}");
}

static inline void handleSetMode(AsyncWebServerRequest *r) {
  if (!requestBasicAuth(r)) return;
  int m = r->arg("mode").toInt();
  if (m!=MODE_AP && m!=MODE_STA) return r->send(400,"application/json","{\"ok\":false}");
  if (m==MODE_AP) { WiFi.disconnect(true); startAPMode(); }
  else {
    if (!connectWiFiSTA()) startAPMode();
  }
  r->send(200,"application/json","{\"ok\":true}");
}
static inline void handleGetMode(AsyncWebServerRequest *r) {
  StrBuf j(128);
  j.catf("{\"mode\":%d,\"ap_ssid\":\"%s\",\"sta_ssid\":\"%s\",\"ip\":\"%s\"}",
    currentWiFiMode, MANUAL_WIFI_SSID, jsonEscape(sta_ssid).c_str(),
    (currentWiFiMode==MODE_STA?WiFi.localIP():WiFi.softAPIP()).toString().c_str());
  r->send(200, "application/json", j);
}

// ---- beginWifi (setup Phase 2) ----
static inline void beginWifi() {
  loadWiFiConfig();
  bool staOK = connectWiFiSTA();
  if (!staOK) startAPMode();
}
