#pragma once
#include "../config.h"
#include "../utils/diag.h"

static inline bool decodeMotorCmd(byte c, int &lf, int &lb, int &rf, int &rb) {
  lf=lb=rf=rb=0;
  switch(c){
    case CMD_STOP:  return true;
    case CMD_FWD:   lf=1;            rf=1; return true;  // 左前 + 右前
    case CMD_BACK:           lb=1;            rb=1; return true;  // 左后 + 右后
    case CMD_LEFT:           lb=1; rf=1; return true;   // 左后 + 右前
    case CMD_RIGHT: lf=1;            rb=1; return true;  // 左前 + 右后
    case CMD_LBACK:          lb=1;             return true;
    case CMD_RBACK:                   rb=1;    return true;
    case CMD_LFWD:  lf=1;                      return true;
    case CMD_RFWD:                    rf=1;    return true;
    default: return false;
  }
}

// 极性 / 左右对调（C3：配合 decodeMotorCmd 一处映射完）
static inline void applyMotorPolarity(int &lf, int &lb, int &rf, int &rb) {
#if INVERT_LEFT_MOTOR
  { int t=lf; lf=lb; lb=t; }
#endif
#if INVERT_RIGHT_MOTOR
  { int t=rf; rf=rb; rb=t; }
#endif
#if SWAP_LEFT_RIGHT_MOTOR
  { int t=lf; lf=rf; rf=t; } { int t=lb; lb=rb; rb=t; }
#endif
}

// 一次性写所有 LOW → 再顺序写 HIGH 每写一个 delay(1)（软启动，浪涌砍半）
static inline void applyLevel(int lf, int lb, int rf, int rb) {
  // Phase A: 所有 LOW 引脚先写（保证桥臂下管先断开，不会直通）
  if (!lf) digitalWrite(LF, LOW);
  if (!lb) digitalWrite(LB, LOW);
  if (!rf) digitalWrite(RF, LOW);
  if (!rb) digitalWrite(RB, LOW);
  delay(MOTOR_DEADBAND_MS);  // 2ms 死区
  // Phase B: HIGH 顺序写，每个之间 1ms（双电机起动浪涌不叠加）
  if (lf) { digitalWrite(LF, HIGH); delay(1); }
  if (lb) { digitalWrite(LB, HIGH); delay(1); }
  if (rf) { digitalWrite(RF, HIGH); delay(1); }
  if (rb) { digitalWrite(RB, HIGH); delay(1); }
}

static inline void motorStopAll() {
  if (xSemaphoreTake(motor_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  digitalWrite(LF,LOW); digitalWrite(LB,LOW); digitalWrite(RF,LOW); digitalWrite(RB,LOW);
  xSemaphoreGive(motor_mutex);
  diagMotor(CMD_STOP, 0,0,0,0);
}

// motorWifi（Web 路径）
static inline void motorWifi(byte c, int speed=255) {
  (void)speed; // L298N 无 PWM，保留签名兼容未来升级
  if (c>=9) return;
  static uint32_t lastCallMs=0;
  uint32_t now=millis();
  if (now-lastCallMs < MOTOR_DEBOUNCE_MS) return;   // C: 30ms 防抖软锁
  lastCallMs=now;
  int lf,lb,rf,rb;
  if (!decodeMotorCmd(c,lf,lb,rf,rb)) return;
  applyMotorPolarity(lf,lb,rf,rb);
  if (xSemaphoreTake(motor_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  applyLevel(lf,lb,rf,rb);
  manualActive = (c!=CMD_STOP);
  manualTimeoutMs = now;
  diagMotor(c,lf,lb,rf,rb);
  xSemaphoreGive(motor_mutex);
}

// applyAnim（跳舞路径）—— 跟 motorWifi 行为完全一致，共用同一套映射
static inline void applyAnim(byte c) { motorWifi(c); }

// Phase 5: 电机 GPIO 上电初始化
static inline void beginMotor() {
  phaseStart("⑤MOTOR");
  pinMode(LF, OUTPUT); pinMode(LB, OUTPUT); pinMode(RF, OUTPUT); pinMode(RB, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)LF, MOTOR_DRIVE_STRENGTH);
  gpio_set_drive_capability((gpio_num_t)LB, MOTOR_DRIVE_STRENGTH);
  gpio_set_drive_capability((gpio_num_t)RF, MOTOR_DRIVE_STRENGTH);
  gpio_set_drive_capability((gpio_num_t)RB, MOTOR_DRIVE_STRENGTH);
  motorStopAll();
  delay(30);   // L298N 上电稳定必须
  phaseOK("⑤MOTOR", "GPIO OUTPUT + LOW (LF=%d LB=%d RF=%d RB=%d, DRIVE=MAX)", LF,LB,RF,RB);
}

// ---- MotorAnim 跳舞调度 ----
static inline void motorAnimStart(const byte *seq, byte seqLen, uint16_t frameMs) {
  if (seqLen>sizeof(anim.seq)) seqLen=sizeof(anim.seq);
  memcpy(anim.seq, seq, seqLen);
  anim.seqLen=seqLen; anim.frameMs=frameMs;
  anim.frame=0; anim.lastTickMs=millis(); anim.running=true;
  manualActive=false;
  DIAG_PRINTF("[DANCE] start %u frames, %ums/frame\n", seqLen, frameMs);
}
static inline void motorAnimStop() {
  if (!anim.running) return;
  anim.running=false; motorStopAll();
  DIAG_PRINTF("[DANCE] stop\n");
}
static inline void motorAnimUpdate() {
  if (!anim.running) return;
  uint32_t now=millis();
  if (now-anim.lastTickMs < anim.frameMs) return;
  anim.lastTickMs=now;
  if (anim.frame>=anim.seqLen) { motorAnimStop(); return; }
  byte c = anim.seq[anim.frame++];
  if (c>=9) return;
  applyAnim(c);
}
