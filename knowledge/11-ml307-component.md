# esp-ml307 组件与本地源码

`esp-ml307-src/` 是 [`78/esp-ml307`](https://github.com/78/esp-ml307) 3.8.1 的本地源码副本，
用来阅读 AT 通道实现、对照日志和做临时实验。它是独立 Git 仓库，父仓库通过 `.gitignore` 排除。

## 组件身份

| 项 | 值 |
|---|---|
| 组件名 | `78/esp-ml307`，构建时目录名 `78__esp-ml307` |
| 版本 | 3.8.1（commit `39638f2`） |
| 仓库 | https://github.com/78/esp-ml307 |
| 许可证 | Apache-2.0 |
| 依赖 | `idf >= 5.5.2`；非 ESP32 目标额外依赖 `78/uart-uhci`；IDF ≥ 6.0 时依赖 `espressif/mqtt` |
| 支持模组 | ML307、EC801E、NT26K 等 Cat.1 模组 |

## 目录结构

```
esp-ml307-src/
├─ include/
│   ├─ at_modem.h         AT 模组抽象基类
│   ├─ at_uart.h          AT 串口传输（含 UHCI DMA）
│   ├─ at_error.h         AT 错误码
│   ├─ esp_network.h      网络接口封装
│   ├─ http_client.h      HTTP 客户端
│   ├─ mqtt.h             MQTT 客户端
│   ├─ tcp.h / udp.h      TCP / UDP
│   └─ web_socket.h       WebSocket
├─ src/
│   ├─ at_modem.cc        AT 命令收发、URC 分发
│   ├─ ml307_at_modem.cc  ML307 初始化、网络注册、信号质量
│   ├─ ml307_http.cc      ML307 HTTP 实现
│   ├─ ml307_mqtt.cc      ML307 MQTT 实现
│   ├─ ml307_tcp.cc / udp.cc / ssl.cc
│   ├─ ec801e_*.cc        EC801E 系列实现
│   └─ esp_*.cc           ESP32 原生 WiFi 网络实现
└─ tests/                 组件自测
```

## 在本仓库里的使用方式

- **09_ml307_probe** 通过 `main/idf_component.yml` 声明 `78/esp-ml307: "^3.8.1"`，构建时由组件管理器下载到 `managed_components/`。
- **xiaozhi-esp32** 的 `managed_components/78__esp-ml307/` 是同一组件的实际构建副本。
- `esp-ml307-src/` 本身**不参与构建**。直接改这里的源码不会影响 09 或 xiaozhi，除非把工程改为引用本地组件。

如果确实要用本地源码替换组件管理器版本，二选一：

1. 把 `esp-ml307-src` 复制到工程的 `components/` 目录，并删除 `idf_component.yml` 里的对应依赖；
2. 在工程 `CMakeLists.txt` 里用 `EXTRA_COMPONENT_DIRS` 指向本地目录，同时确保没有同名组件被组件管理器拉取。

## 硬件与引脚

| 项 | 值 |
|---|---|
| 模组 | ML307R（底板板载） |
| ESP TX | GPIO12 |
| ESP RX | GPIO11 |
| 波特率 | 115200（出厂默认） |
| SIM | 手机 Nano SIM，插入后可用 |

串口按**模组侧**命名：底板 `G12_RXD` 是模组 RX、`G11_TXD` 是模组 TX，所以 ESP 侧必须 **TX=12、RX=11**，接反一个字节都收不到。

## 调试要点

- 无 SIM 卡时初始化仍能识别模组，网络注册会按预期报 `cme=10`。
- AT 通道走 UHCI DMA，`sdkconfig.defaults` 里把 UART ISR 放在 IRAM，避免 Flash 操作期间丢中断。
- 日志里先确认模组型号、IMEI，再看 `WaitForNetworkReady`；信号质量可用来判断天线和卡状态。
- HTTP / MQTT / WebSocket 都封装在同一套网络接口上，换模组时业务代码不需要改。
