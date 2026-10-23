# AI 随身助手（xiaozhi-esp32）

`xiaozhi-esp32/` 是在小智开源工程 v2.4.2 基础上扩展出的完整应用，板型名为 `goouuu-s3-dual`。
它把这块底板上的屏幕、麦克风、功放、环境传感器、六轴、WiFi 和 ML307R 4G 模组整合成一个可对话的随身助手。

## 板型文件

| 文件 | 作用 |
|---|---|
| `main/boards/goouuu-s3-dual/config.h` | 引脚定义，与底板原理图一致 |
| `main/boards/goouuu-s3-dual/config.json` | 三个构建变体：默认、`goouuu-s3-dual-gps`、`goouuu-s3-dual-app-demo` |
| `main/boards/goouuu-s3-dual/goouuu_s3_dual.cc` | 板级主类：网络、按键、手势、摇一摇、MCP 工具、状态条 |
| `main/boards/goouuu-s3-dual/sensor_hub.cc/.h` | AHT20 / BMP280 / MPU6050 统一入口，含 50 Hz 采样、Mahony 四元数与摇一摇判定 |
| `main/boards/goouuu-s3-dual/gps_receiver.cc/.h` | ATGM336H NMEA 解析与离线城市匹配 |
| `main/boards/common/dual_network_board.cc/.h` | WiFi 优先、4G 自动兜底的状态机（App Demo 变体不启用） |
| `main/app_demo/` | App 对接 Demo：BLE V2 服务、显式选路状态机、传感器上报 |

## 构建与烧录

```powershell
cd xiaozhi-esp32
python scripts/build.py goouuu-s3-dual                        # 默认：UART0 控制台，GPS 关闭
python scripts/build.py goouuu-s3-dual --name goouuu-s3-dual-gps  # GPS 开启，控制台改 USB-Serial-JTAG
python scripts/build.py goouuu-s3-dual --name goouuu-s3-dual-app-demo  # Android App 对接 Demo
idf.py -p COM4 flash monitor
```

`build/` 是本地构建目录，搬迁仓库后已清理，重新构建会自动生成。历史固件归档在
`xiaozhi-esp32/releases/goouuu-s3-dual_v2.4.2_20261021/`，含整包、分文件、`sdkconfig` 快照和烧录命令。

## 双网络

| 模式 | 行为 |
|---|---|
| `auto`（默认） | 优先连已保存的 WiFi，20 秒内连不上自动切 4G |
| `wifi` | 仅 WiFi，失败进入配网 |
| `4g` | 仅 4G；60 秒仍不可用时自动切回 WiFi 配网 |

- 模式保存在 NVS 的 `network/type`（0=wifi，1=4g，2=auto）。
- WiFi 掉线先给 30 秒重连窗口，仍未恢复才切 4G；切到 4G 后不会自动切回 WiFi。
- 配网期间禁止降级，否则会把状态机卡在 `wifi_configuring`；降级发生在配网状态时会把状态机拉回 `starting`。
- 降级时保留退役的 WiFi 板卡实例并清空回调，避免 `WifiManager` 回调悬空。

## 交互

| 操作 | 行为 |
|---|---|
| 单击 BOOT | 开始/结束对话；启动阶段单击进入 WiFi 配网 |
| 双击 BOOT | `auto` → `wifi` → `4g` → `auto` 循环切换（会重启） |
| 长按 BOOT | 进入 WiFi 配网（当前不是 WiFi 链路时提示先切模式） |
| 拿起 | 由静止转为移动时点亮屏幕 |
| 静止 60 秒 | 亮度降到 20% |
| 静止 5 分钟 | 熄屏 |
| 轻摇 | 待机时播“嗯？”；聆听/说话时只变表情 |
| 重摇 | 打断当前 TTS 或聆听，立即播本地抱怨语音 |

对话、配网、激活期间不做亮度调节。`connecting / activating / upgrading` 阶段摇一摇和唤醒词都不响应，进入待机后恢复。

## 摇一摇实现

- `SensorHub` 以 50 Hz 采样 MPU6050，0.9 秒内出现 3 次方向反转算一次摇晃；开机 3 秒后才开始检测。
- 动态加速度阈值 0.45 g，冷却时间轻摇 2 秒、重摇 5 秒。
- 轻摇：3–4 次反转且峰值 0.45–1.5 g；重摇：≥5 次反转或峰值 ≥1.5 g。
- 重摇走 `Application::PlaySoundInterrupting()`：说话时先 `abort` 并清空解码队列，聆听时先 `listen/stop` 并清空发送队列，然后播本地音效。
- 本地语音包在 `main/assets/common/shake_*.ogg`，由 `tools/gen_shake_audio.py` 生成，改文案后执行 `tools/gen_shake_audio.ps1`，并重新 configure 让 `EMBED_FILES` 更新。

## 对 AI 开放的工具

| 工具 | 内容 |
|---|---|
| `self.sensor.environment` | 温度、湿度、气压、1 小时气压趋势、数据新鲜度 |
| `self.sensor.motion` | 六轴姿态、运动状态 |
| `self.location.get_position` | 经纬度 + 离线城市匹配（先报坐标再报城市） |
| `self.network.get_status` | 网络模式、当前链路、是否联网 |
| `self.network.set_mode` | 切换 `auto` / `wifi` / `4g`，重启生效 |
| `self.device.get_status` | 网络、传感器在线状态、音量与亮度 |
| `self.device.shake_status` | 摇晃次数、重摇次数、上次摇晃距今秒数 |

## 屏幕

`LcdDisplay` 顶栏每 5 秒刷新一行滚动信息，例如：

```
自动·WiFi在线 ·24.3°C 46% ·1012hPa ·静止 ·北京
```

`SetInfoText` 只继承父对象样式，不设置局部字体指针，避免激活阶段资源重建后字体悬空导致 `InstrFetchProhibited`。

## App 对接 Demo（BLE V2）

`goouuu-s3-dual-app-demo` 变体面向 Android App，不走语音对话：

- BLE 与 Internet 完全解耦，断网也能连上设备、切选路、看状态。
- 只暴露 `wifi` / `4g` 两条链路，`selected_route` 存 NVS，失败不回滚，**不使用 AUTO/fallback**。
- `set_route` 必须依次回 `ACK → route(initializing) → … → route(ready|failed)`。
- 传感器周期上报：env/baro 2 s、IMU 10 Hz、GPS 1 s、device 5 s、wifi 2 s、cell 3 s；
  IMU 用 Mahony 互补滤波输出四元数，BLE 只保留最新一帧，避免 App 卡顿。
- 字段、枚举、错误码见 [13-ble-v2-protocol.md](13-ble-v2-protocol.md)。

## 已知问题与限制

- 缺少 `robot_2` 表情资源，启动时打印 `Emoji not found: robot_2`，不影响功能。
- 首包音频序号偶发乱序，不影响对话。
- GPS 变体代码就绪，但硬件尚未接线验证；G43/G44 与 UART0 控制台冲突，必须用 `-gps` 变体并走 USB 串口看日志。
- 本地抱怨语音与云端 TTS 音色不同；前者不依赖网络、响应更快。
- 4G 需要插入手机 SIM 卡；无卡时按预期报错。
- GPIO33–GPIO37 被 8 MB Octal PSRAM 占用，不可用作普通 IO。

## 排错顺序

1. 串口看启动日志：WiFi 是否连上、激活是否完成、唤醒词模型是否加载。
2. 配网后停在“检测模组/不能唤醒”：先确认没有进入配网模式后又触发 4G 降级。
3. 激活阶段崩溃：优先怀疑 LVGL 字体或资源指针在资源重建后悬空。
4. 摇一摇没反应：确认当前状态不是 `connecting/activating/upgrading`，再看串口有没有 `Light shake` / `Heavy shake` 日志。
5. 重摇后不能唤醒：检查是否还停留在 `connecting`，以及 `abort` 后状态机是否回到 `idle`。
