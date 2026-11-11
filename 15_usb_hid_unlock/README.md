# 15_usb_hid_unlock · BOOT 键触发 USB / 蓝牙 HID 解锁电脑 + 手机网页配置

把 ESP32-S3 当成一把「键盘」：插在 Windows 电脑上，**短按**板载 **BOOT** 键，
固件自动输入当前解锁凭据（电脑 PIN 或电脑密码）并回车，完成锁屏解锁。
**长按 BOOT 3 秒**临时打开配置热点，手机连上后自动弹出配置网页；
网页登录密码出厂默认 `0000`，**第一次登录必须修改**，之后才能设置解锁方式 —— 不用重新编译烧录。
**长按 BOOT 10 秒**恢复出厂设置。

键盘有两条通路，**USB 优先、蓝牙兜底**：

- USB 已挂载（线插着）→ 走 USB HID，最稳；
- USB 没插、但蓝牙已配对连接 → 走 BLE HID 蓝牙键盘；
- 两者都没有 → 记一条警告日志，不发送。

底座是 ESP-IDF v5.5.6 官方 `examples/peripherals/usb/device/tusb_hid`（Unlicense OR CC0-1.0），
去掉了鼠标部分，只保留键盘，并按「唤醒 → 等界面就绪 → 逐位输入 → 回车」的锁屏时序改写；
蓝牙部分来自 `examples/bluetooth/esp_hid_device` 的 NimBLE 键盘分支。

## 三种操作

| 操作 | 行为 |
|---|---|
| 短按 BOOT（< 3 秒松开） | 先发 Win+L 强制锁屏 → 等 0.9 秒 → Space 唤醒 → 等 1.2 秒 → 按当前解锁方式逐键输入 → 回车 |
| 长按 BOOT ≥ 3 秒 | 开 / 关配置热点（不会输入凭据） |
| 长按 BOOT ≥ 10 秒 | 恢复出厂：网页密码回 `0000`、清空电脑凭据、USB/蓝牙通路都开启、清除全部蓝牙配对，然后重启 |

长按判定在按满 3 秒立即开热点、按满 10 秒立即恢复出厂，都不需要松手；松手后也不会补发按键。
还没设置解锁方式（mode = 未设置）时，短按只打日志不动作。

## 蓝牙配对（一次性）

设备默认一直在广播，名字是 `ESP32-S3 Unlock Key`：

1. Windows：设置 → 蓝牙和其他设备 → 添加设备 → 蓝牙 → 选 `ESP32-S3 Unlock Key`。
2. 采用 Just Works 配对，**不需要在电脑上输配对码**，点一下就连上；配对信息（bond）存在 NVS，重启后自动重连。
3. 日志出现 `BTKBD: 电脑已连接，蓝牙键盘可用` 即可。拔掉 USB 线，短按 BOOT 就会走蓝牙输 PIN。

注意：

- **BIOS / 开机密码界面用不了蓝牙**，那里没有蓝牙协议栈，只能插 USB。
- USB 线插着时永远走 USB，蓝牙只作兜底，不会两边同时输入。
- 电量固定上报 **100%**（设备是 USB 供电，esp_hid 默认带的电池服务如果不上报，Windows 会一直提示「电量不足 0%」）。
- 换电脑配对：**先在 Windows「删除设备」，再在网页点「清除所有蓝牙配对」**，然后在新电脑上重新添加。只清一边会在配对时反复加密失败。

## 手机网页配置

| 项 | 值 |
|---|---|
| 热点名 | `ESP32-Unlock-xxxx`（xxxx 为 MAC 后 4 位） |
| 热点密码 | 无（开放热点，直接连） |
| 配置页地址 | 连上后自动弹出；没弹就手动打开 `http://192.168.4.1/` |
| 网页登录密码 | 出厂默认 **`0000`**（4–16 位纯数字），**首次登录强制修改** |
| 可做的事 | 设置电脑 PIN / 电脑密码并二选一解锁；清空电脑凭据；开关 USB / 蓝牙通路；查看蓝牙状态和本机地址；清除所有蓝牙配对；修改网页密码 |

### 解锁方式规则

| 项目 | 规则 |
|---|---|
| 电脑 PIN | 4–16 位纯数字 |
| 电脑密码 | 1–32 位 ASCII 可打印字符（英文、数字、符号；不支持中文） |
| 两者关系 | 可以都填，也可以只填一个；**但同一时刻只能选中一种作为解锁方式** |
| 初始状态 | 两者都为空、解锁方式未设置，短按 BOOT 不动作（旧固件升级上来时，NVS 里遗留的 PIN 会在首次启动自动清空） |
| 保存条件 | 必须选中「用电脑 PIN」或「用电脑密码」，且对应那一栏不能为空 |

- **密码模式的前提**：Windows 锁屏默认显示 PIN 框时，字母和符号会被忽略。用电脑密码解锁前，
  要先在锁屏界面点「登录选项 → 密码」切到密码框，再短按 BOOT。设备只能单向发键，无法感知电脑当前是哪个输入框。
- 键盘按**美式布局**发送，`@ # ! $` 等符号是 Shift 组合键；锁屏前请勿停留在中文输入法。
- 强制门户：热点内所有 DNS 查询都指向 `192.168.4.1`，iOS / Android / Windows 会自己弹出登录页。
- 登录后服务端下发一个随机 Session Cookie（`sid`，1 小时有效），未登录访问接口一律 401；
  已登录但还没改过默认网页密码时，除改密接口外一律 403。
- 凭据与解锁方式存 NVS，**改完立即生效、重启不丢**，网页保存不会关闭热点，可以连着改多项。
- 热点只在长按期间存在，平时不耗电也不占 2.4G 信道。
- **热点会自己关**：开启后 1 分钟内没有任何设备连上 → 自动关闭；只要有设备连上过，最晚从连上那一刻起 5 分钟关闭（不管中途是否断开）。关网页 / 锁屏都不会立刻关热点，靠这两条超时兜底。参数在 `wifi_portal.c` 顶部的 `PORTAL_IDLE_CLOSE_MS` / `PORTAL_JOIN_CLOSE_MS`。
- 通路开关存在 NVS 的 `unlock_cfg` 命名空间，重启保持；两条都关掉也允许（短按 BOOT 只打日志不动作），长按开热点不受影响。
- 「关闭 USB」只是固件不再通过 USB 发键，电脑里仍能看到这个键盘设备；不想让它有任何输入就关掉这条通路即可。
- **忘记网页密码**：长按 BOOT 10 秒恢复出厂（网页密码回 `0000`），代价是电脑凭据和蓝牙配对一起清空，需要重新设置。

## 硬件与接线

| 项 | 说明 |
|---|---|
| 核心板 | ESP32-S3-WROOM-1 N16R8（果云版） |
| 触发按键 | 板载 BOOT，**GPIO0**，按下为低，内部上拉已开 |
| 电脑侧接口 | 核心板标 **USB** 的 Type-C 口（USB-OTG，GPIO19/20） |
| 蓝牙 | 芯片自带 BLE（ESP32-S3 只有 BLE，没有经典蓝牙），配好对即可当蓝牙键盘 |
| 调试/烧录接口 | 核心板标 **UART** 的 Type-C 口（GPIO43/44，UART0） |
| 配置热点 | 用芯片自带 WiFi，不需要接任何外设 |

**注意：ESP32-S3 的 USB-OTG 与 USB-Serial-JTAG 共用 GPIO19/20，同一时刻只能用一个。**
本工程已经把控制台固定在 UART0，所以日志和烧录必须走 UART 口；USB 口只负责给电脑当键盘。

## 构建与烧录

```powershell
# 在仓库根目录
.\tools\build.ps1 -Project 15_usb_hid_unlock
.\tools\flash.ps1 -Project 15_usb_hid_unlock -Port COM4
.\tools\monitor.ps1 -Project 15_usb_hid_unlock -Port COM4
```

改完代码只想增量编译时（不重新 set-target）：

```powershell
. D:\esp32\esp32s3-goouuu-projects\tools\env.ps1
idf.py -B D:\esp32\build\15_usb_hid_unlock build
```

构建产物在 `D:\esp32\build\15_usb_hid_unlock`。
COM4 换成核心板 UART 口对应的串口号。

## 首次配置步骤

1. 给板子上电（插 USB 即可），**正常上电之后再按 BOOT** —— 上电瞬间按住会进下载模式。
2. 长按 BOOT 3 秒，日志出现 `配置热点已开启：WiFi = ESP32-Unlock-xxxx ...`。
3. 手机 WiFi 连上 `ESP32-Unlock-xxxx`（无密码），系统一般会自动弹出配置页；没弹就打开 `http://192.168.4.1/`。
4. 输入网页密码 `0000` → 登录 → 进入强制改密页，设置 4–16 位纯数字的新网页密码。
5. 在配置页填写电脑 PIN（4–16 位纯数字）和/或电脑密码（1–32 位 ASCII），并**选中一种**解锁方式 → 保存。
6. 之后锁屏时短按 BOOT，设备就按选中的方式输入并回车。

密码模式（用电脑密码解锁）每次锁屏前记得先点「登录选项 → 密码」把输入框切到密码；
用 PIN 模式则保持在 PIN 框即可。

## 代码结构

| 文件 | 作用 |
|---|---|
| [main/usb_hid_unlock.c](main/usb_hid_unlock.c) | USB HID 描述符、按键时序、字符→键码映射、BOOT 短按/长按/10 秒恢复出厂、USB/蓝牙通路选择 |
| [main/bt_hid_kbd.c](main/bt_hid_kbd.c) / [.h](main/bt_hid_kbd.h) | BLE HID 键盘：NimBLE 初始化、HID 报告描述符、广播与配对、发键 |
| [main/pin_store.c](main/pin_store.c) / [.h](main/pin_store.h) | 电脑 PIN / 电脑密码 / 解锁方式的 NVS 读写与合法性校验 |
| [main/web_auth.c](main/web_auth.c) / [.h](main/web_auth.h) | 网页登录密码（默认 0000、首次必须修改）的 NVS 读写 |
| [main/wifi_portal.c](main/wifi_portal.c) / [.h](main/wifi_portal.h) | SoftAP、HTTP 服务、Session 登录、凭据接口、强制门户 DNS |
| [main/dev_cfg.c](main/dev_cfg.c) / [.h](main/dev_cfg.h) | USB / 蓝牙通路开关的 NVS 读写 |
| [main/www/index.html](main/www/index.html) | 配置网页（编译时用 `EMBED_FILES` 打进固件，无外部 CDN） |

接口一览（都返回 JSON；`ready` = 已登录且已完成首次改密）：

| 方法与路径 | 说明 |
|---|---|
| `POST /api/login`，body `password=0000` | 校验网页密码，成功返回 `Set-Cookie: sid=...` 和 `{"ok":true,"must_change":true/false}` |
| `POST /api/webpass`，body `password=1234` | 需登录；设置新网页密码（4–16 位纯数字），首次登录用它解除强制改密 |
| `GET /api/cred` | 需 `ready`，返回 `{"ok":true,"mode":1,"pin":"...","password":"..."}` |
| `POST /api/cred`，body `pin=1234&password=abc&mode=1` | 需 `ready`，保存两组凭据并指定解锁方式（1=PIN，2=密码）；空串表示清空该项 |
| `POST /api/cred/clear` | 需 `ready`，清空电脑 PIN、电脑密码和解锁方式，回到初始空状态 |
| `GET /api/status` | 需 `ready`，返回 mode、has_pin、has_pwd、USB/蓝牙开关、蓝牙连接/广播状态、本机蓝牙地址 |
| `POST /api/route`，body `usb=1&ble=0` | 需 `ready`，保存通路开关（NVS）并立即生效 |
| `POST /api/bt/forget` | 需 `ready`，清除所有蓝牙配对并重新广播 |
| `GET 其它任意路径` | 一律返回配置页（用于强制门户探测） |

未登录访问返回 401；已登录但还没改默认网页密码时，除 `/api/webpass` 外一律 403。

## 发送时序

| 步骤 | 内容 | 默认时长 |
|---|---|---|
| 1 | 按 **Win+L** 强制锁屏，保证凭据只进锁屏，不会打进前台窗口 | 按下 15 ms |
| 2 | 等锁屏动画结束 | 900 ms |
| 3 | 按一次 **Space** 唤醒屏幕 / 唤出输入框 | 按下 15 ms |
| 4 | 等锁屏界面就绪 | 1200 ms |
| 5 | 按当前解锁方式逐字符输入（PIN 为纯数字键；密码含 Shift 组合键） | 每键 15 ms + 间隔 40 ms |
| 6 | 回车提交 | 50 ms 后按 Enter |

这几个时间都可以在 [main/usb_hid_unlock.c](main/usb_hid_unlock.c) 顶部调：
`LONG_PRESS_MS`、`FACTORY_RESET_MS`、`LOCK_WAIT_MS`、`WAKE_DELAY_MS`、`KEY_PRESS_MS`、`KEY_GAP_MS`。
如果出现「只输入了前几位」，先把 `WAKE_DELAY_MS` 加到 2000–3000 再试。
网页登录密码的默认值和长度限制在 [main/web_auth.h](main/web_auth.h) 里（`WEB_PWD_DEFAULT`）。

## 预期日志

```text
I (xxx) CRED: 解锁方式：电脑 PIN（4 位）
W (xxx) WEBAUTH: 网页密码仍是出厂默认 0000，首次登录后必须修改
I (xxx) UNLOCK: USB HID 初始化
I (xxx) UNLOCK: USB HID 就绪
I (xxx) UNLOCK: 蓝牙 HID 初始化
I (xxx) BTKBD: NimBLE 主机任务启动
I (xxx) BTKBD: 蓝牙已开始广播：ESP32-S3 Unlock Key
I (xxx) BTKBD: 电脑已连接，蓝牙键盘可用
I (xxx) UNLOCK: 等待 BOOT 按键
I (xxx) UNLOCK: BOOT 按下
I (xxx) UNLOCK: 使用 USB HID 通路          # 插着线时
I (xxx) UNLOCK: USB 未连接，使用蓝牙 HID 通路  # 拔线且蓝牙已连接时
I (xxx) UNLOCK: 发送 Win+L 锁屏键
I (xxx) UNLOCK: 发送唤醒键
I (xxx) UNLOCK: 开始输入电脑 PIN（4 位）
I (xxx) UNLOCK: PIN 发送完成
I (xxx) UNLOCK: BOOT 长按 3 秒，切换配置热点
I (xxx) PORTAL: 配置热点已开启：WiFi = ESP32-Unlock-xxxx（开放），网页 = http://192.168.4.1/ ，网页密码为出厂默认 0000（首次登录必须修改）
I (xxx) PORTAL: 强制门户 DNS 已启动
I (xxx) PORTAL: 网页登录成功，需要先修改默认密码
I (xxx) WEBAUTH: 网页登录密码已更新
I (xxx) PORTAL: 解锁方式已设为电脑 PIN（4 位）
I (xxx) PORTAL: 配置热点已关闭
I (xxx) UNLOCK: BOOT 长按 10 秒，恢复出厂设置
```

## 注意事项

1. **BOOT 是 strapping 脚**。上电或复位瞬间按住会进入下载模式，程序不会运行。正常使用是「先上电，再按键」。
2. **凭据在 NVS 里仍是明文**。能读 Flash 就能拿到；网页密码也只是简单哈希无关的明文存储，只适合自己用的 demo。建议电脑 PIN 与网页密码不要设成同一个。
3. **误触不会泄露凭据**。发送前固件会先按 `Win+L` 强制锁屏，内容只输进锁屏的安全桌面（其他程序、录屏都抓不到）。代价是多约 0.9 秒，且正在编辑的内容可能被打断。
4. **锁屏停在人脸识别时输不进去**。Windows 在人脸扫描期间独占键盘输入，`Esc` / `Enter` / `Space` 都无效，也没有官方快捷键（微软明确这是设计限制）。此时先手动点一下鼠标 / 触摸板唤出输入框，再短按 BOOT。指纹登录没有这个问题。
5. **密码模式需要手动切输入框**。Windows 锁屏默认给的是 PIN 框，此时字母/符号会被忽略；必须先点「登录选项 → 密码」。设备无法感知电脑当前焦点在哪个框。
6. **热点是开放网络**。同一时间只允许 2 台设备接入，登录一次只有一个有效会话；建议改完长按关掉热点，或等它自己超时关闭。
7. **睡眠时设备会掉电**。板子从电脑 USB 取电，电脑一睡 USB 断电，按键就不响应了。
8. **锁屏后才插入可能不认**。部分机器开了「锁屏时禁用新 DMA 设备」策略，新 HID 设备要等登录后才生效。
9. **WiFi 与 HID 互不影响**。热点开着的时候短按 BOOT 依然会输凭据，可以边配网页边测解锁。
10. **键盘布局按美式发送**。PIN 是纯数字，任何布局都能正确输入；密码模式只保证美式布局（`@` = Shift+2 等）。
11. **BIOS 阶段蓝牙不可用**。BLE 键盘只在 Windows 起来之后有效；开机密码 / BIOS 设置界面请插 USB 线。
12. **WiFi 和蓝牙共用 2.4G 射频**。已开启共存调度，但配置热点开着的瞬间蓝牙延迟会略增，属正常现象。

## 参考

- 官方例程：`D:\Espressif\esp-idf\examples\peripherals\usb\device\tusb_hid`
- ESP-IDF `esp_http_server`：`components/esp_http_server`（URI 通配符 + Session 都是自己用 Cookie 手写的）
- [ToothPaste](https://github.com/Brisk4t/ToothPaste)（ESP-IDF + TinyUSB 的 HID 发送实现，AGPL-3.0，仅思路参考）
- [SecureKey](https://github.com/Shubhjaiswal408/SecureKey)（Windows 下 HID 枚举等待与逐字符发送，MIT）
