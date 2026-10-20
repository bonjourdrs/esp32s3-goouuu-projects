# 09 · ML307R 4G 模组探测

验证底板上的 ML307R（Cat.1 4G 模组）能不能被识别和初始化。**这个工程不需要 SIM 卡**——
AT 通道通不通，靠模组的型号、IMEI 和 AT 错误码就能确认；没有卡只会让"入网"这一步失败，
而那正好是预期结果。

## 硬件

| 信号 | ESP32-S3 | 说明 |
|---|---|---|
| ESP TX | GPIO12 | 接模组 RXD |
| ESP RX | GPIO11 | 接模组 TXD |
| EN | 5VIN | 模组常使能 |
| VCC | 5VIN | 模组需要 5 V |

串口是**交叉**连接的：ESP 的 TX 接模组的 RX。底板的网络名是按**模组侧**命名的
（G11_TXD / G12_RXD），所以 ESP 侧正好相反。这一对引脚已在 2026-10-21 实测确认，
实测过程见下方「实测结果」。

UART 用 UART1，走 UHCI DMA（组件内部实现），不是普通的 `uart_driver_install`。

## 依赖

`main/idf_component.yml` 声明了两个托管组件，首次编译会自动下载：

```yaml
dependencies:
  idf: ">=5.5.2"
  78/esp-ml307: "^3.8.1"
  78/uart-uhci: "^0.4.1"
```

第二条不是可选项：`at_uart.h` 是 esp-ml307 的公开头文件，却 `#include "uart_uhci.h"`，
而上游只把 `uart-uhci` 列为私有依赖。不显式声明就会在编译时报找不到头文件。

`78/esp-ml307` 是[小智项目](https://github.com/78/esp-ml307)的 Cat.1 AT 组件，支持
ML307A / ML307R / EC801E / NT26K，自动识别型号。它要求 **C++23**（用了 `std::expected`）
和 **ESP-IDF ≥ 5.5.2**；本仓库是 5.5.6，满足。

在 ESP32-S3 上，组件会把 `78/uart-uhci` 一起拉下来（`idf_component.yml` 里的
`target not in [esp32]` 规则）。所以 `managed_components/` 下会有两个目录，都不入库。

## 启动流程

```
按实测顺序探测：TX=GPIO12 RX=GPIO11  →  失败再试 TX=GPIO11 RX=GPIO12
   ↓  AtModem::Detect(TX, RX, DTR=NC, 115200, 6000)
   ↓  自动扫波特率：115200 → 921600 → 460800 → 230400 → 57600 → 38400 → 19200 → 9600
   ↓  每轮发 "AT"，收到 OK 即锁定
   ↓  发 AT+CGMR 读型号，按返回值选 ML307 / EC801E 后端
   ↓
打印 revision / 实际波特率 / IMEI
   ↓
逐条查询 CPIN? / MCCID / CSQ / CEREG? / COPS?
   ↓
WaitForNetworkReady(10 s)
```

超时必须给一个有限值。组件的 `DetectBaudRate` 里是 `while (true)`，超时参数为 `-1`
时会**永远重试**，没有模组就卡死在这里。这里给 6 s，刚好够扫完 8 档波特率；
两路引脚各 6 s，最坏情况约 12 s 出结论。

## 实测结果（2026-10-21，未插 SIM）

```
I (761) ML307: probing with measured   TX=GPIO12 RX=GPIO11
I (7911) AtUart: Detected baud rate: 115200
I (7921) AtModem: Detected modem: ML307R-DL-MBRH0S01
I (7941) ML307: answered on measured   TX=GPIO12 RX=GPIO11
I (7941) ML307: module revision : ML307R-DL-MBRH0S01
I (7951) ML307: active baud rate: 115200
I (7951) ML307: IMEI            : 864269079780892
W (7971) ML307: AT+CPIN?  FAIL AT CME error (cme=10)
W (7981) ML307: AT+MCCID  FAIL AT CME error (cme=10)
I (7981) ML307: AT+CSQ    OK
I (7991) ML307: AT+CEREG? OK
W (8001) ML307: AT+COPS?  FAIL AT CME error (cme=30)
W (8001) ML307: network attach  : SIM missing or PIN error (expected to fail with no SIM)
I (23001) ML307: alive  pin_ready=0  network_ready=0  CSQ=99
```

结论：模组识别成功，AT 通道双向可用。第一次探测用反过来的引脚顺序（TX=GPIO11）
时，8 档波特率全部无应答，说明那一对确实不通，不是波特率问题。

`AT+CSQ` / `AT+CEREG?` 返回 `OK` 但正文为空，是因为组件把 `+CSQ:` / `+CEREG:`
这些信息行吞掉了，只留 `OK`；`GetCsq()` 读到 99 也能印证无卡无信号。

判断标准很直接：

| 现象 | 含义 |
|---|---|
| 打印出 `Detected modem: ML307R-...` | AT 通道已通，模组型号识别成功 |
| `IMEI` 有值 | 模组固件正常，且不依赖 SIM |
| `AT+CPIN?` 返回 `cme=10` | 确认"无卡"，**不是**通信故障 |
| `CSQ=99` | 无卡时无信号，正常 |
| `AT+COPS?` 返回 `cme=30` | 没有网络服务，无卡时正常 |
| `network attach: SIM missing or PIN error` | 完全符合无卡预期 |

**关键区分**：`cme=10` / `cme=30` 是模组正常应答后给出的业务错误，说明数据已经在来回跑；
而 `detect failed: timeout` 才是物理层没通。

## 排错

- **`detect failed: timeout`**：先量模组 VCC 有没有 5 V，再确认 EN 是否接 5VIN。
  这些都正常却仍超时，说明是模组本身没起来（或排线断了），不是引脚顺序问题——
  两个顺序都已自动试过。
- **`Detected modem` 但型号乱码**：波特率扫到了但电平不稳，通常是杜邦线太长或地没共好。
- **`UART_FIFO_OVF`**：已通过 `CONFIG_UART_ISR_IN_IRAM=y` 规避；如果仍出现，把其它重负载
  任务绑到 CPU1。
- **编译报 `std::expected` 找不到**：确认 ESP-IDF ≥ 5.5.2，且组件版本 ≥ 3.7。

## 编译烧录

```powershell
.\tools\build.ps1 -Project 09_ml307_probe
.\tools\flash.ps1 -Project 09_ml307_probe -Port COM4
.\tools\monitor.ps1 -Project 09_ml307_probe -Port COM4
```

首次编译要下载组件，比其它工程慢；之后走缓存。

## 插卡之后

装好 SIM 卡再跑一次，`WaitForNetworkReady` 应该返回 `ready`，`AT+CSQ` 会给出
实际的信号质量（0–31，越大越好），`AT+COPS?` 能读到运营商名。到那一步就可以接
`CreateHttp()` / `CreateMqtt()` 做真正的联网测试了。
