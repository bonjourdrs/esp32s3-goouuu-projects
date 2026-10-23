# goouuu-s3-dual

GOOUUU ESP32-S3 底板上的 AI 随身助手板型：WiFi + ML307R 4G 双网络，板载 AHT20 / BMP280 /
MPU6050 与可选 ATGM336H GPS，240×240 ST7789 屏幕。

## 构建

```bash
python scripts/build.py goouuu-s3-dual                       # 默认固件（UART0 控制台，GPS 关闭）
python scripts/build.py goouuu-s3-dual --name goouuu-s3-dual-gps       # 启用 GPS（控制台改为 USB-Serial-JTAG）
python scripts/build.py goouuu-s3-dual --name goouuu-s3-dual-app-demo  # Android App 对接 Demo
```

## App 对接 Demo 变体

`goouuu-s3-dual-app-demo` 通过 `CONFIG_APP_DEMO_MODE` 走独立入口，只跑
「BLE + Wi-Fi/4G 显式选路 + 传感器上报」，不初始化音频、屏幕 UI 与云端连接，
供 Android V2.x 直接管理设备。

- 设备名 `ESP32S3-AI-ASSISTANT`，Service `8f1d0001-…`，RX `8f1d0002-…`，TX `8f1d0003-…`，MTU 247。
- 命令：`get_capabilities` / `get_status` / `set_route` / `reinit_route`。
- `selected_route` 独立存 NVS（`app_demo/route`，1=wifi，2=4g），只由 App 命令或启动恢复修改；
  网络成功/失败都不改它，也**不走 AUTO 自动兜底**。
- 周期上报：env/baro 2 s、imu 10 Hz、gps 1 s、device 5 s、wifi 2 s、cell 3 s，状态变化立即补发。
- IMU 用 Mahony 互补滤波输出四元数，BLE 只发最新一帧，拥塞时丢旧帧。
- GPS 占用 UART2（GPIO43/44，9600），控制台固定 USB-Serial-JTAG。

完整字段、枚举与消息时序见
[knowledge/13-ble-v2-protocol.md](../../../../knowledge/13-ble-v2-protocol.md)。

## 网络模式

| 模式 | 行为 |
| --- | --- |
| `auto`（默认） | 优先连 WiFi，20 秒内连不上自动切 4G；没有配置过 WiFi 时直接走 4G |
| `wifi` | 仅 WiFi，连接失败进入配网模式 |
| `4g` | 仅 4G |

模式保存在 NVS 的 `network/type`（0=wifi，1=4g，2=auto），重启后生效。

交互：

- 单击 BOOT：开始/结束对话；启动阶段单击进入 WiFi 配网
- 双击 BOOT：`auto` → `wifi` → `4g` → `auto` 循环切换（会重启）
- 长按 BOOT：进入 WiFi 配网（当前不是 WiFi 链路时会提示先切换模式）
- 摇一摇：分轻摇和重摇两级，见下方「摇一摇反馈」
- 手势：拿起（由静止转为移动）自动点亮屏幕；空闲静止 60 秒降到 20% 亮度，静止 5 分钟熄屏；对话/配网期间不做亮度调节

## 对 AI 开放的工具

| 工具 | 说明 |
| --- | --- |
| `self.sensor.environment` | 温度、湿度、气压，含 1 小时气压趋势与数据新鲜度 |
| `self.sensor.motion` | 六轴姿态、运动状态 |
| `self.location.get_position` | 经纬度 + 离线城市匹配（先报坐标再报城市） |
| `self.network.get_status` | 网络模式、当前链路、是否联网 |
| `self.network.set_mode` | 切换 `auto` / `wifi` / `4g`，重启生效 |
| `self.device.get_status` | 网络、各传感器在线状态、音量与亮度 |
| `self.device.shake_status` | 累计摇晃次数、重摇次数、上次摇晃距今秒数，供 AI 在下一轮对话中吐槽 |

## 屏幕状态条

顶栏每 5 秒刷新一行滚动信息，例如：

```
自动·WiFi在线 ·24.3°C 46% ·1012hPa ·静止 ·北京
```

## 摇一摇反馈

判定在 `SensorHub` 里完成：50 Hz 采样，0.9 秒内出现 3 次方向反转算一次摇晃，再按反转次数和峰值加速度分级。

| 当前状态 | 轻摇（3–4 次反转，峰值 0.45–1.5 g） | 重摇（≥5 次反转或峰值 ≥1.5 g） |
| --- | --- | --- |
| 待机 | 播“嗯？”，表情变困惑 | 随机播一句本地抱怨，表情变烦 |
| 聆听 | 仅表情变困惑（不出声，避免被麦克风采集上传） | 结束本轮聆听、丢弃残留录音后播抱怨 |
| 说话 | 仅表情变困惑（不出声，避免排在 TTS 后面） | 立即打断 TTS、回待机后播抱怨 |

重摇走 `Application::PlaySoundInterrupting()`：说话时先 `abort` 并清空解码队列，聆听时先 `listen/stop` 并清空发送队列，
保证本地音效立刻响，且不会把自己播放的声音当成用户输入。`connecting / activating / upgrading` 阶段不响应摇一摇。

## 本地语音包

抱怨语音在 `main/assets/common/shake_*.ogg`（16 kHz、24 kbps Opus），由 `tools/gen_shake_audio.py` 用 edge-tts 生成，
文案在脚本顶部，改完重新执行即可：

```powershell
.\tools\gen_shake_audio.ps1
```

新增或删除 OGG 后需要重新跑一次 CMake configure（或 `idf.py reconfigure`），否则 `EMBED_FILES` 不会更新。
`lang_config.h` 由构建脚本自动生成，不要手改。

## 已知限制

- `auto` 模式下 WiFi 掉线会先给 30 秒重连窗口，仍未恢复才切 4G；切到 4G 后不会自动切回 WiFi，需重启或手动切模式
- GPS 与 UART0 控制台共用 GPIO43/44，只能用 `goouuu-s3-dual-gps` 固件并走 USB 串口看日志
- 城市名来自内置离线城市表（地级市最近匹配），不做行政区级精确逆地理编码
- 激活流程（检查资源、检查新版本）期间摇一摇和唤醒词都不响应，进入待机后恢复
- 本地抱怨语音与云端 TTS 音色不同，前者不依赖网络、响应更快
