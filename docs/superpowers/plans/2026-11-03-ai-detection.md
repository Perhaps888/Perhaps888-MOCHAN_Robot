# Web 端 YOLO 检测 实现计划

> 依据：docs/superpowers/specs/2026-11-03-ai-detection-design.md（已获用户确认）
> 注：本目录不是 git 仓库，跳过 commit 步骤；嵌入式项目无单测框架，验证 = 编译 + 本地浏览器测试页 + 串口诊断。

**Goal:** 浏览器端 YOLOv8n 实时框选 + 检测到人时 OLED 开心表情，全离线。

**Architecture:** YOLOv8n-int8 + onnxruntime-web 资产存 ESP32 LittleFS → `/ai/*` 路由供给浏览器 → 网页 canvas 叠加画框 → `ai:person` 经 `/ws_ctrl` 通知固件。

---

### Task 1: 准备 AI 资产（离线，PC 上完成）

**Files:**
- Create: `ai_assets/ort.wasm.min.js.gz`、`ai_assets/ort-wasm-simd.wasm.gz`、`ai_assets/yolov8n-320-int8.onnx.gz`
- Create: `tools/prepare_ai_assets.py`（下载 + int8 动态量化 + gzip，一键复现）

- [ ] Step 1: `pip install onnxruntime onnx`（如缺）
- [ ] Step 2: 下载 `ort.wasm.min.js`、`ort-wasm-simd.wasm`（jsdelivr，onnxruntime-web 1.20.x dist）
- [ ] Step 3: 下载 yolov8n.onnx（ultralytics assets）；若 404 则 `pip install ultralytics && yolo export model=yolov8n.pt format=onnx imgsz=320`
- [ ] Step 4: `onnxruntime.quantization.quant_dynamic(..., weight_type=QuantType.QInt8)`，若输入尺寸非 320 则用 onnx 工具改 shape
- [ ] Step 5: 三个文件 gzip（mtime=0），验证总大小 < 8MB
- [ ] 验证: 文件存在且 gzip -t 通过

### Task 2: 本地浏览器测试页先行验证 JS 全链路

**Files:**
- Create: `test_ai.html`（独立测试页，从本地 http 服务加载 ai_assets，对静态图检测画框）

- [ ] Step 1: 写检测核心 JS（本文件即将来嵌入 web_content.h 的同一份逻辑）：
  - `ort.env.wasm.wasmPaths='/ai/'`（测试页用相对路径 `./ai_assets_raw/`）
  - letterbox 240×176→320×320（填充 114）→ Float32 NCHW /255
  - session 输出 [1,84,2100]：cx,cy,w,h + 80 类分数；score>0.45；类内 NMS IoU 0.45
  - 框按类别哈希配色，画标签 + 置信度
- [ ] Step 2: `python -m http.server` 起本地服务，用含人物/物体的测试图验证
- [ ] 验证: browser_use 打开测试页，截图确认框选正确

### Task 3: 固件 — LittleFS + AI 路由

**Files:**
- Create: `net/ai_store.h`（LittleFS 挂载、/ai/*、/ai_upload、/ai_status 实现）
- Modify: `net/web_routes.h`（ROUTES 表加 3 行）
- Modify: `1.ino`（setup 里 initAIStore()）
- Modify: `config.h`（AI_FS 路径常量、AI_PERSON_HOLD_MS=5000）

- [ ] Step 1: `ai_store.h`：`LittleFS.begin(true)`；诊断打印 totalBytes/usedBytes
- [ ] Step 2: `GET /ai/<file>`：LittleFS 读 `<name>.gz` 流式输出 + `Content-Encoding:gzip`；MIME：wasm/js/octet-stream；404 JSON
- [ ] Step 3: `POST /ai_upload?path=<name>`：multipart 写临时文件 `.tmp`，完成后 rename；复用现有 OTA 上传回调模式
- [ ] Step 4: `GET /ai_status`：检查 3 个文件是否齐全，返回 JSON
- [ ] 验证: arduino-cli/IDE 编译通过（无 arduino-cli 则人工核对 + 用户 IDE 编译）

### Task 4: 固件 — person 彩蛋

**Files:**
- Modify: `1.ino`（onWsCtrlEvent 解析 `ai:person`；OLED 循环里 mood 切换）

- [ ] Step 1: 全局 `volatile uint32_t personSeenUntilMs=0;`（config.h extern）
- [ ] Step 2: wsCtrl 文本消息 == `ai:person` → `personSeenUntilMs = millis()+AI_PERSON_HOLD_MS`
- [ ] Step 3: OLED 任务每 ~500ms 检查：期内 `roboEyes.setMood(HAPPY)`，过期恢复 DEFAULT（仅当当前 mood 需要变时才调用，避免每帧重置动画）
- [ ] 验证: 编译通过

### Task 5: 网页嵌入 web_content.h

**Files:**
- Modify: `web_content.h`（视频区加 overlay canvas、「AI 检测」开关、上传 UI、检测循环——复用 Task 2 已验证代码）
- Regenerate: `web_content_gz.h`（`python tools/embed_web.py`）

- [ ] Step 1: 视频容器 `position:relative`，overlay canvas `position:absolute;inset:0;pointer-events:none`
- [ ] Step 2: 「AI 检测」toggle：先 GET /ai_status；未就绪弹上传 UI（多文件 input → 逐个 POST /ai_upload，显示进度）
- [ ] Step 3: 模型加载：Cache Storage 命中则跳过下载；显示下载/初始化进度
- [ ] Step 4: 检测循环 rAF 驱动、串行 await；耗时 >250ms 自动每 3 帧一次；框坐标按 img 显示尺寸缩放
- [ ] Step 5: person>0.6 → wsCtrl `ai:person`（1 次/秒节流）
- [ ] Step 6: 跑 embed_web.py 重新生成 gz 头
- [ ] 验证: embed 脚本输出无报错；本地测试页同源代码已通过 Task 2

### Task 6: 收尾 — 编译与烧录指引

- [ ] Step 1: 有 arduino-cli 则 `arduino-cli compile` 验证；没有则逐项人工核对 include/签名
- [ ] Step 2: 输出烧录指引（分区方案选择、上传模型步骤）写入回复，不新建文档

---

## Self-Review

- Spec 覆盖：LittleFS/路由/上传/ai_status(T3)✓ person 彩蛋(T4)✓ overlay/开关/加载/NMS/节流/通知(T5)✓ 资产(T1)✓ 离线测试(T2)✓ 部署指引(T6)✓
- 无 TBD/占位符；类型命名一致：`initAIStore()`、`personSeenUntilMs`、`AI_PERSON_HOLD_MS` 全文统一
- Task 2 先于 Task 5：同一份 JS 先在本地验证再嵌入，降低调试难度
