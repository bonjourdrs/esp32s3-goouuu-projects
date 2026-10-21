# 代码审查遗留项（Backlog）

这份清单来自 `xiaozhi-esp32` 的完整代码审查（review 报告），记录**本轮没有处理、留待后续排期**的问题。已修复项见文末「已完成」一节，不在这里重复展开。

排期原则：先保语音主链路和硬件功能，再谈安全加固、产品化和性能优化。

---

## 一、安全与产品化（需要协议或量产配置配合）

| 编号 | 问题 | 为什么现在不做 | 建议方案 |
|---|---|---|---|
| P0-07 | 远程固件 URL + Secure Boot 关闭 | 需要量产密钥、签名私钥和烧录流程，属产品决策 | 开启 Secure Boot v2 + Flash Encryption，固件 URL 走签名校验；开发板保持现状 |
| P1-19 / 8.3 | MQTT 音频 AES-CTR 只有加密没有认证，可被篡改/重放 | 需服务端同步升级协议，单改设备端会直接连不上 | 服务端与设备端一起切 AES-GCM 或 AES-CCM，消息带 nonce/序号 |
| 8.2 | MCP URL 类工具构成设备侧 SSRF 能力 | 已限制只允许 http/https，进一步限制需产品定义白名单 | 增加域名白名单 / 禁止内网网段 / 用户确认弹窗 |
| 8.4 / P2-39 | 激活 payload 完整打印 serial/challenge/HMAC；IMEI/ICCID 已脱敏 | 日志治理影响排查效率，需配套日志级别规范 | INFO 只打印前后各 4 位，完整值仅 DEBUG 且默认关闭 |
| P1-28 | Assets 更新不是 A/B 原子切换 | 需要改分区表和升级流程，工作量大 | 资源分区 A/B + 校验通过后切 boot 分区 |
| P1-29 | Assets 只有 16-bit additive checksum，无签名 | 同上，且需与资源打包工具一起改 | SHA-256 + 签名，与 A/B 方案一起落地 |

## 二、OTA / 版本管理

| 编号 | 问题 | 为什么现在不做 | 建议方案 |
|---|---|---|---|
| P2-32 | 版本检查指数退避总等待过长 | 影响首次开机体验但不影响功能 | 前 3 次短间隔，之后封顶 10 分钟 |
| P2-33 | 服务端 activation timeout 字段未真正用于 deadline | 需要确认服务端语义 | 解析后用 esp_timer 控制激活总时长 |
| P2-34 | OTA `server_time` 疑似把 Unix epoch 直接加时区偏移 | 需要和服务端对齐时间字段定义 | 明确字段是 UTC epoch 还是本地时间，加单测 |
| P2-35 | OTA image_header 累积逻辑会重复 append 已读 buffer | 触发条件少，需构造特定分包 | 按 offset 写入而非 append，补短读单测 |
| P2-36 | 下载前未校验 Content-Length 是否放得下 update 分区 | 正常固件不会超，异常包会浪费一次升级 | 预检查 `content_length <= partition size` |
| P2-37 | OTA 不验证 `total_read == content_length` | 断流时靠 ESP-IDF 校验兜底 | 写完前显式比对长度 |
| P2-38 | OTA early-return 路径没有统一 RAII Close | 只是资源清理风格问题 | 用 RAII 封装 HTTP 连接与 update handle |

## 三、并发与架构（改动面大，需专门回归）

| 编号 | 问题 | 为什么现在不做 | 建议方案 |
|---|---|---|---|
| P2-21 | `DualNetworkBoard::GetBoardType()` 返回当前子板类型 | 已改 `GetBoardJson` 返回板名，语义影响面待评估 | 明确「板型」与「当前网络」两个字段，调用方逐个迁移 |
| P2-22 | AUTO 模式在没有保存 Wi-Fi 时不会自动尝试 4G | 当前设计是先进配网，避免用户不知情走流量 | 产品确认后加「无 SSID 时先试 4G、超时再配网」开关 |
| P2-23 | `retired_board_` 是回调生命周期债务 | 已用保留实例避免悬空，但仍是长期隐患 | 网络子板改为组合而非继承，回调统一注册到 DualNetworkBoard |
| P2-24 | 多处 main/callback 路径直接 `vTaskDelay` | 网络模式切换已挪到独立任务，剩余多为一次性启动延时 | 统一改成 esp_timer 延时 + 事件回调 |
| P2-47 | `AudioService::IsIdle()` 不含 send queue | 影响省电判定精度，不影响功能 | 把 send queue 计入 idle 条件 |
| P3-05 | MCP 注册的裸指针 ownership 不清晰 | 当前有重复注册 delete 保护 | 改 `std::unique_ptr` 容器 |

## 四、传感器 / GPS（硬件确认后再动）

| 编号 | 问题 | 为什么现在不做 | 建议方案 |
|---|---|---|---|
| P2-04 | 气压 history 没有 timestamp | 已按样本数修正 1 小时语义，够用 | 环形缓冲存 `{us, hpa}`，趋势按时间窗口算 |
| P2-05 | AHT20 init 未强制校验 calibration bit | 当前仅日志提示，实测工作正常 | 校准位为 0 时重试一次初始化，仍失败则标记不可用 |
| P2-06 | BMP/MPU 多处寄存器写返回值被忽略 | 失败概率低，逐条加检查会拉长初始化代码 | 统一 `ESP_RETURN_ON_ERROR` 风格，失败即标记该传感器不可用 |
| P2-07 | MPU6050 开机 1 秒 gyro bias 标定默认设备静止 | 用户开机时晃动会引入零偏，影响姿态角 | 标定期间检测加速度方差，晃动则延后重标 |
| P2-09 | 姿态角假设 MPU 安装坐标系固定 | 需要实际装配方向确认 | 增加安装方向配置项（旋转矩阵/轴向映射） |
| P2-11 | SensorHub 没有 runtime re-probe / reset | 上电后热插拔概率低 | 环境任务里定期探测失败设备并重初始化 |
| P2-12 | I2C 400kHz 依赖内部上拉 | 需确认底板是否有外部上拉电阻 | 硬件确认后决定是否删除内部上拉配置 |
| P2-14 | 无 checksum 的 NMEA 语句被直接接受 | 部分模组默认关闭 checksum，禁用会收不到定位 | 增加 Kconfig 开关，默认接受，产品版可强制校验 |
| P2-20 | 城市距离用 equirectangular，仅适合局部距离 | 已加 80km 最大距离限制，误匹配已可控 | 需要跨省精度时换 Haversine |
| P3-10 | GPS 城市表硬编码在固件 | 表小、离线可用，符合当前产品定位 | 后续可放 Assets 分区或改为服务端反查 |

## 五、性能与代码质量（不影响当前功能）

| 编号 | 问题 | 为什么现在不做 | 建议方案 |
|---|---|---|---|
| P2-29 | MCP tool list 8KB 硬上限 | 当前工具数量远未触顶 | 超限时分页返回，或改为按需查询 |
| P2-30 | MCP JSON-RPC 只接受 numeric id | 官方客户端都用数字 id | 兼容 string id |
| P2-40 | Assets 初始化 mmap 整个 8MB 分区 | 有 PSRAM，当前无内存压力 | 按需 mmap 或改流式读取 |
| P2-41 | NVS 分区只有 16KB | 当前配置项少，够用 | 改分区表扩容，注意与 OTA 布局一起评估 |
| P2-42 | Settings 写失败大量使用 fatal 风格 | 写失败极少发生 | 改为返回错误码 + 上层提示 |
| P2-43 / P2-44 | NoAudioCodec 每帧创建 vector、每帧 `pow()` | 该路径非当前主力（走 ES8311 codec） | 预分配缓冲 + 音量查表 |
| P2-45 | Runtime I2S path 使用 `ESP_ERROR_CHECK` 过于激进 | 初始化阶段失败即重启符合预期 | 运行期改为记录错误并重建通道 |
| P2-46 | Resampler 调用返回值检查不完整 | 当前采样率组合固定 | 补齐返回值检查 |
| P2-48 | AFE input_buffer 从头部 erase，实时性能不理想 | 当前帧率下未观察到卡顿 | 改环形缓冲或 offset 游标 |
| P2-49 | Wake-word encode static task 创建结果/内存策略 | 当前工作正常 | 检查创建结果，失败降级为动态任务 |
| P2-50 | LCD panel reset/init 返回值被忽略 | 屏已实测可用 | 补错误日志，失败时标记显示不可用 |
| P2-51 | GOOUUU info/gesture timer create/start 返回值未检查 | 定时器创建失败概率极低 | 补检查并降级（不启用手势） |
| P2-54 | 设备 Busy 时无条件 `RestoreScreen` | 属省电策略取舍 | 按产品定义：Busy 时是否保持熄屏 |
| P3-01 | Root CMake 全局关闭 `-Wmissing-field-initializers` | 上游默认配置，改动会引入大量告警 | 局部打开，逐个修 |
| P3-02 | board source 使用 `file(GLOB ...)` | 上游机制，新增板目录自动生效很方便 | 改为显式列表（需同步改脚本） |
| P3-06 | 多个 boolean/status 用字符串表示 | 影响可读性和性能，改动面广 | 内部改 enum，对外 JSON 再转字符串 |
| P3-07 | BuildInfoText 每 5 秒构造大量临时 string | 量小，无明显影响 | 改为变化时才刷新 |
| P3-08 | 传感器实现全部堆在单一 `sensor_hub.cc` | 已稳定运行，拆分风险大于收益 | 按芯片拆成 aht20/bmp280/mpu6050 驱动 |
| P3-09 | 中英文和产品文案硬编码在 board C++ | 与 i18n 体系不一致 | 收敛到 lang_config / 资源包 |

## 六、测试体系

| 编号 | 内容 | 建议 |
|---|---|---|
| 13.1 | 4 个 Python 测试在 Windows 上因临时目录清理 `RecursionError` 失败 | 测试 teardown 改用 `ignore_errors=True` 或 `shutil.rmtree(..., onexc=...)` |
| 13.2 | 缺少运行时单测：协议解析、状态机、音频队列、传感器判定 | 抽成纯函数/纯类，用 host 单测覆盖 |
| 13.3 | 最该 fuzz 的入口：WebSocket 帧、MQTT JSON、Assets 索引、NMEA 行 | 建 libFuzzer/自建随机输入测试 |

---

## 已完成（本轮 review 修复）

**P0**：P0-01 WebSocket 帧边界、P0-02 user_only 执行权限、P0-03 Assets 索引边界、P0-04 状态机迁移加锁、P0-05 DisplayLockGuard、P0-06 图片预览上限。

**P1**：P1-01～P1-05、P1-06（网络边沿事件改有序队列）、P1-07～P1-18、P1-20～P1-27、P1-30～P1-35。

**P2**：P2-01/02（valid 时效 + 分传感器时间戳）、P2-03、P2-08、P2-10、P2-13、P2-15、P2-16、P2-17、P2-18、P2-19、P2-26、P2-27、P2-28、P2-31、P2-52、P2-53。

**P3**：P3-03、P3-04。

**验证状态**：ESP-IDF v5.5.6 编译通过；`scripts/tests` 62 个用例 58 通过，4 个失败为 Windows 临时目录清理问题（见 13.1）。
