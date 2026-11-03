# MOCHAN Robot — Web 端 YOLO 目标检测设计

日期：2026-11-03
状态：已与用户确认，进入实现

## 目标

在机器人现有 Web 控制台的摄像头画面上叠加 YOLO 风格的实时检测框（类别 + 置信度），
并附带彩蛋：检测到「人」时机器人 OLED 眼睛切换为开心表情。

## 约束与前提

- 硬件：ESP32-S3-CAM（OV2640，HQVGA 240×176 JPEG，~20 FPS 经 WebSocket `/ws_cam` 推流）
- Flash 16MB（用户确认），Arduino IDE 烧录
- 机器人常工作在 AP 热点模式，浏览器端**不能依赖外网**，所有 AI 资产必须由机器人自己提供
- 现有摄像头 / 电机 / 推流逻辑零改动

## 架构：浏览器端推理

```
摄像头 → ESP32 共享缓冲 → /ws_cam → 浏览器 <img>
                                       ├─ 离屏 canvas letterbox 320×320
                                       ├─ onnxruntime-web (WASM SIMD) 推理 YOLOv8n-int8
                                       ├─ JS NMS → 叠加 canvas 画框
                                       └─ person 检出 → /ws_ctrl "ai:person" → ESP32 → OLED HAPPY
```

模型与运行时（~6.6MB gzip）存放于 ESP32 LittleFS，经 `/ai/*` 路由提供给浏览器，
浏览器用 Cache Storage 缓存，二次打开秒载。

## 组件

### AI 资产（离线准备，经网页上传到机器人）

| 文件 | gzip 后 | 来源 |
|---|---|---|
| `ort.wasm.min.js` | ~0.05MB | onnxruntime-web 1.17.3 dist（最后一代带非线程版 wasm，免 COOP/COEP 隔离头） |
| `ort-wasm-simd.wasm` | ~2.9MB | 同上（单线程 SIMD） |
| `yolov8n.onnx`（320×320 int8） | ~2.7MB | ultralytics 导出 320 → **手工逐通道权重量化 + DequantizeLinear**（不用 quantize_dynamic：新版会转成 ConvInteger，ORT WASM 后端无此内核） |

> 实测（桌面浏览器）：320 int8 推理 ~115ms/帧，zidane 图 2×person ✓，bus 图 bus+3×person ✓

### 固件（1.ino，net/ 目录为未使用的重构版本，不改）

- `LittleFS.begin(true)`（首次自动格式化），启动打印容量与文件清单诊断
- 分区方案：Arduino IDE 选「16M with spiffs (3MB APP/9MB SPIFFS)」
- 新路由：
  - `GET /ai/<file>`：从 LittleFS 流式输出，`.gz` 存储 + `Content-Encoding: gzip`；
    `.wasm` → `application/wasm`，`.js` → `text/javascript`，`.onnx` → `application/octet-stream`；
    文件不存在返回 404 JSON
  - `POST /ai_upload?path=<name>`：multipart 上传写入 LittleFS（固件同款上传处理器模式）
  - `GET /ai_status`：`{"ready":true/false,"files":{...}}`，网页据此决定是否允许开启检测
- `/ws_ctrl` 新消息 `ai:person`：置 `personSeenUntilMs = now + 5000`
- OLED 任务：`personSeenUntilMs` 有效期内 `roboEyes.setMood(HAPPY)`，过期恢复 DEFAULT

### 网页（web_content.h，改后跑 tools/embed_web.py 重新生成 gz 头）

- 视频 `<img>` 上叠透明 `<canvas>`，CSS 尺寸跟随视频，框坐标按 240×176 → 显示尺寸缩放
- 「AI 检测」开关：开启时先拉取/初始化模型，显示进度；`/ai_status` 未就绪则提示先上传
- 检测循环：`requestAnimationFrame` 驱动，串行 await 推理；实测耗时 >250ms 自动降为每 3 帧一次（手机）
- 预处理：letterbox（灰边 114）→ float32 NCHW /255；输出 [1,84,2100] 解码 + score 0.45 阈值 + 类内 NMS IoU 0.45
- person 且置信度 > 0.6：wsCtrl 发 `ai:person`，本地节流 1 次/秒

## 部署流程

1. Arduino IDE：Flash Size 16MB + 上述分区方案，刷新固件
2. 打开控制台 →「上传 AI 文件」→ 选择准备好的 3 个 `.gz` 文件
3. 点「AI 检测」开关即用

## 错误处理

- LittleFS 挂载失败 / 模型缺失：网页显示「AI 未就绪」并禁用开关，其余功能不受影响
- 浏览器不支持 WASM SIMD：onnxruntime-web 自动回退纯 WASM（慢但可用）；文件缺失回退提示
- 上传中断：写入临时文件，完整接收后才 rename 生效，避免半个模型文件

## 测试

- 本地测试页（test_ai.html）：用静态图片验证 预处理→推理→NMS→画框 全链路，再嵌入固件网页
- 固件编译验证；串口确认 LittleFS 容量与 /ai_status 返回
