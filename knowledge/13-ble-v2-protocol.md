# BLE V2 接口（App ↔ ESP32，冻结协议）

`xiaozhi-esp32` 的 `goouuu-s3-dual-app-demo` 变体与 Android V2 / V2.1 之间的接口契约。
协议已冻结：字段名、枚举值、单位都不能改；App 端按本文解析，固件端不要另设计一套。

代码位置：`xiaozhi-esp32/main/app_demo/`

| 文件 | 职责 |
|---|---|
| `app_ble_manager.cc/.h` | NimBLE GATT 外设、收发队列、MTU 处理 |
| `app_demo.cc/.h` | 启动流程、命令解析与 ACK |
| `app_network_manager.cc/.h` | `selected_route` 持久状态、Wi-Fi / 4G 状态机 |
| `app_sensor_reporter.cc/.h` | 周期上报与 `get_status` 全量发送 |
| `app_device_status.cc/.h` | uptime / heap / psram / fw |

## 1. GATT 固定参数

| 项目 | 值 |
|---|---|
| Device Name | `ESP32S3-AI-ASSISTANT` |
| Service | `8f1d0001-9b4a-4c2e-9d3f-7a6b5c4d3e2f` |
| RX（App → ESP32） | `8f1d0002-9b4a-4c2e-9d3f-7a6b5c4d3e2f`，`WRITE` + `WRITE WITHOUT RESPONSE` |
| TX（ESP32 → App） | `8f1d0003-9b4a-4c2e-9d3f-7a6b5c4d3e2f`，`READ` + `NOTIFY` |
| CCCD | `00002902-0000-1000-8000-00805f9b34fb` |
| MTU | App 请求 247，固件 `ble_att_set_preferred_mtu(247)` |

只注册一个自定义 Service，不注册 GATT（0x1801），避免 Android 把 Service Changed 误当成 NOTIFY 通道。

## 2. 消息边界

- 所有应用消息都是 UTF-8 JSON Object，不加二进制头，不要求 `\n`。
- 推荐「一个 Notify = 一个完整 JSON」；超过 MTU 时 App 支持跨 Notify 重组，但固件会先按 MTU 丢弃超长帧。
- 普通消息与高频 IMU 分开：ACK/状态走普通队列，IMU 走深度 1 的「最新帧」队列，拥塞时丢旧帧，不补发历史样本。
- BLE TX 里不混入串口日志。

## 3. 命令格式

```json
{"type":"cmd","id":101,"cmd":"get_status"}
```

- `type` 固定 `cmd`，`id` 由 App 生成，ACK 原样带回。
- 仅四个命令：`get_capabilities`、`get_status`、`set_route`、`reinit_route`，全小写。

### set_route

```json
{"type":"cmd","id":102,"cmd":"set_route","route":"wifi"}
{"type":"cmd","id":103,"cmd":"set_route","route":"4g"}
```

`route` 只能是 `wifi` / `4g`。执行顺序是硬约束：

```
立即改 selected_route → 写 NVS → ACK →
Notify route(initializing) → 真实重初始化目标链路 →
route(connecting/checking_internet) → route(ready|failed)
```

即使初始化失败，`selected_route` 也**不回滚**，`route.selected` 仍是用户选择的链路。

### reinit_route

```json
{"type":"cmd","id":106,"cmd":"reinit_route"}
```

只重新初始化当前 `selected_route`，**不得修改** `selected_route`。

## 4. ACK

```json
{"type":"ack","id":102,"ok":true}
{"type":"ack","id":102,"ok":false,"err":"INVALID_ROUTE"}
```

错误码：`AP_NOT_FOUND`、`AUTH_FAILED`、`DHCP_FAILED`、`NO_INTERNET`、`MODEM_NOT_RESPONDING`、
`SIM_NOT_INSERTED`、`REGISTRATION_FAILED`、`REGISTRATION_DENIED`、`DATA_SESSION_FAILED`、
`TIMEOUT`、`INTERNAL_ERROR`、`INVALID_ROUTE`、`UNKNOWN_CMD`、`INVALID_JSON`。

## 5. route（最终权威状态）

```json
{"type":"route","selected":"wifi","phase":"ready"}
{"type":"route","selected":"wifi","phase":"failed","err":"AP_NOT_FOUND"}
```

- `selected`：`wifi` / `4g`
- `phase`：`idle` / `initializing` / `connecting` / `checking_internet` / `ready` / `failed`
- 每次 `set_route`、`reinit_route`、设备启动、BLE 重连后的 `get_status`，都必须能拿到当前 route 状态。

## 6. Wi-Fi 状态

```json
{"type":"wifi","state":"online","ap":1,"ip_ready":1,"internet":1,
 "ssid":"Home_5G","ip":"192.168.1.52","rssi":-55}
```

`state`：`disabled` / `initializing` / `connecting` / `ap_connected` / `got_ip` /
`checking_internet` / `online` / `offline` / `error`。
布尔字段统一用 `0/1`；`ssid`、`ip`、`err` 只在有值时出现。

## 7. 4G 状态

```json
{"type":"cell","state":"online","module":1,"sim":1,"registered":1,"data":1,"internet":1,
 "operator":"CMCC","ip":"10.x.x.x","rssi":-81,"csq":18}
```

`state`：`disabled` / `initializing` / `checking_sim` / `no_sim` / `sim_ready` / `registering` /
`registered` / `starting_data` / `data_connected` / `checking_internet` / `online` / `offline` / `error`。
`type` 必须是 `cell`（不是 `4g` / `lte` / `cellular` / `ml307`）。

## 8. 传感器上报

| 消息 | 周期 | 字段 |
|---|---|---|
| `env` | 2 s | `temp`（°C）、`hum`（%）；缺 AHT20 时 `err="AHT20_NOT_FOUND"` |
| `baro` | 2 s | `pressure`（hPa）、`chip_temp`（°C）；缺 BMP280 时 `err="BMP280_NOT_FOUND"` |
| `imu` | **10 Hz（100 ms）** | `ax/ay/az`（g）、`gx/gy/gz`（°/s）、`qw/qx/qy/qz`、`motion`、`pose` |
| `gps` | 1 s | `fix`、`lat`、`lon`、`alt`、`speed`（km/h）、`course`、`sat`、`hdop` |
| `device` | 5 s | `uptime`（秒）、`heap`、`psram`（bytes）、`fw="0.2-demo"` |
| `wifi` | 2 s | 同第 6 节 |
| `cell` | 3 s | 同第 7 节 |

状态变化（连接、注册、定位获得）会立即补发一条，不等周期。

### IMU 姿态

```json
{"type":"imu","ax":0.01,"ay":-0.02,"az":0.99,"gx":0.3,"gy":-0.1,"gz":0.2,
 "qw":0.965,"qx":0.102,"qy":0.214,"qz":0.083,"motion":"still","pose":"face_up"}
```

- 单位：加速度 g，角速度 °/s；四元数无量纲。
- `motion`：`still` / `moving` / `shake`
- `pose`：`face_up` / `face_down` / `portrait` / `landscape`
- 融合：MPU6050 内部 50 Hz 采样 → Mahony 互补滤波（加速度计校正重力）→ 四元数。
  无磁力计，Roll/Pitch 稳定，**Yaw 允许缓慢漂移**，不算故障。
- BLE 每 100 ms 只发最新一帧；数值按 3～4 位小数收敛，单条报文约 165 字节，能放进 MTU 247。

### GPS

```json
{"type":"gps","fix":1,"lat":31.123456,"lon":121.123456,"alt":25.2,
 "speed":0.0,"course":126.0,"sat":12,"hdop":0.9}
```

无定位只发 `{"type":"gps","fix":0}`；GPS 未启用时加 `err="GPS_DISABLED"`。
单位：lat/lon 十进制度、alt 米、speed km/h、course 度、sat 颗、hdop 无量纲。
NMEA 原始速度若为节，必须换算成 km/h 再发。地图、瓦片、逆地理编码全部由 App 负责。

## 9. cap

```json
{"type":"cap","wifi":1,"4g":1,"aht20":1,"bmp280":1,"mpu6050":1,"gps":1}
```

后续可新增 `bmm350`、`ltr390`、`tof`、`sgp41`、`scd41`、`battery`、`camera`，已有 key 不改名。

## 10. get_status 顺序

收到 `get_status` 先回 ACK，再依次 Notify：`route`、`wifi`、`cell`、`env`、`baro`、`imu`、`gps`、`device`。
不要合并成一个超大 `all_status`。

## 11. 构建与联调

```powershell
cd xiaozhi-esp32
python scripts/build.py goouuu-s3-dual --name goouuu-s3-dual-app-demo
```

该变体打开 `CONFIG_APP_DEMO_MODE`，不初始化音频/云端，只跑 BLE + 选路 + 传感器；
控制台是 USB-Serial-JTAG，GPS 占用 UART2（GPIO43/44，9600）。

联调要点：

- App 连上后先发 `get_capabilities`，再发 `get_status` 建立全量状态。
- 点 Wi-Fi/4G 后必须收到 `ACK → route(initializing) → … → route(ready|failed)`。
- 拔掉 SIM 卡切 4G：应得到 `cell.state="no_sim"` 与 `route.phase="failed"`，`selected` 仍是 `4g`。
- 断网时 BLE 仍可用；BLE 与 Internet 完全解耦。
