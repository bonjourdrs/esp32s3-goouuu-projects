# 10_ble_demo · BLE 双向通信最小验证

用 NimBLE 搭一个最小 GATT 外设，验证「Android 扫描 → 连接 → 写 RX → ESP32 处理 → Notify TX → Android」
这条链路。它只验证 BLE 本身，不接 Wi-Fi、4G、音频或传感器，用来把手机端和固件端的连接问题隔离出来。

`xiaozhi-esp32` 里的 `goouuu-s3-dual-app-demo` 变体复用同一套 UUID 与通道设计，本工程可以看作是它的最小可运行参考。

## 硬件与构建

- 芯片：ESP32-S3（N16R8）
- 框架：ESP-IDF v5.5.6 + NimBLE
- 控制台：USB-Serial-JTAG（`sdkconfig.defaults` 已配置），与 BLE 互不影响

```powershell
# 在仓库根目录
.\tools\build.ps1 -Project 10_ble_demo
.\tools\flash.ps1 -Project 10_ble_demo -Port COM4
.\tools\monitor.ps1 -Project 10_ble_demo -Port COM4
```

构建产物在 `D:\esp32\build\10_ble_demo`。

## GATT 固定参数

| 项目 | 值 |
|---|---|
| Device Name | `ESP32S3-BLE-DEMO` |
| Service | `8f1d0001-9b4a-4c2e-9d3f-7a6b5c4d3e2f` |
| RX（App → ESP32） | `8f1d0002-9b4a-4c2e-9d3f-7a6b5c4d3e2f`，`WRITE` + `WRITE WITHOUT RESPONSE` |
| TX（ESP32 → App） | `8f1d0003-9b4a-4c2e-9d3f-7a6b5c4d3e2f`，`READ` + `NOTIFY` |
| CCCD | `00002902-0000-1000-8000-00805f9b34fb`（NimBLE 自动生成） |
| 建议 MTU | 247 |

注意：本工程不注册 GATT 服务（0x1801）。GATT 的 Service Changed 带 INDICATE 属性，Android 端如果
「取第一个 NOTIFY/INDICATE 特征」会把它误当成接收通道，所以这里只保留只读的 GAP 服务，
保证 App 枚举到的第一个 WRITE / NOTIFY 就是 RX / TX。

## 应用层协议（纯文本，无二进制头）

| App 写入 | ESP32 回复 |
|---|---|
| `PING` | `PONG` |
| `STATUS` | `{"device":"ESP32-S3","ble":true,"uptime":<毫秒>}` |
| 其他可打印文本 | `ECHO:<原文>` |
| 二进制数据 | 原样回显 |

- 收到数据会先去掉首尾空白，因此 App 自动补 `\r\n` 也按文本处理。
- 收到/发出都会打印长度与十六进制（最多 64 字节），排查「App 到底发了什么」很方便。
- 订阅 TX 后，固件每 5 秒主动 Notify 一条心跳；未订阅或未连接时不发送。
- 发送队列深度 4，单条最长 512 字节；超过 `MTU-3` 会被截断并打印警告。

## 实测结论

2026-10-23 用 Android 实测通过 7 项：扫描发现、连接、MTU 247、写 RX、Notify TX、
断开重连、跨 Notify 数据完整性。日志关键字：

```text
[BLE] client connected (conn_handle=...)
[BLE] mtu updated to 247
[BLE] TX notify enabled
[BLE RX] len=4 text="PING"
[BLE TX] len=4 text="PONG"
```

## 常见问题

- **App 找不到设备**：确认广播已启动（`BLE advertising started`），并且没有先连上旧连接。
- **写 RX 无反应**：确认写的是 `8f1d0002`；`WRITE` 与 `WRITE WITHOUT RESPONSE` 都支持。
- **收不到 Notify**：必须写 CCCD 使能通知，日志出现 `TX notify enabled` 才算订阅成功。
- **Android 拿到 Service Changed**：不要在 App 侧按「第一个 NOTIFY 特征」选通道，按 UUID 精确匹配。
