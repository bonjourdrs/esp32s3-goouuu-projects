# 知识库

接手这套工程需要知道的内容都集中在这里。代码分布在 01–10 十个独立示例、`xiaozhi-esp32` 应用工程和 `esp-ml307-src` 组件副本中，本目录只放给人读的材料。

## 阅读顺序

第一次接触这块板子，按下面顺序走一遍，大约半天就能独立改代码。

| 顺序 | 文档 | 读完能做什么 |
|---|---|---|
| 1 | [01-quickstart.md](01-quickstart.md) | 接上板子，编译、烧录、看到串口输出 |
| 2 | [02-toolchain-setup.md](02-toolchain-setup.md) | 在另一台电脑上从零装出一样的工具链 |
| 3 | [03-hardware.md](03-hardware.md) | 看懂底板引脚，知道模块接在哪、哪些脚不能碰 |
| 4 | [04-architecture.md](04-architecture.md) | 说清示例工程的模块划分、音频数据流和仓库分层 |
| 5 | [05-workflow.md](05-workflow.md) | 按仓库约定新增工程并提交 |
| 6 | [06-tuning.md](06-tuning.md) | 改麦克风灵敏度、换音阶、调灯的颜色 |
| 7 | [07-troubleshooting.md](07-troubleshooting.md) | 独立定位编译、烧录、串口、外设的常见故障 |

按需查阅：

| 文档 | 内容 |
|---|---|
| [08-glossary.md](08-glossary.md) | I2S、RMT、dBFS 等术语解释 |
| [09-roadmap.md](09-roadmap.md) | 还没做的扩展方向 |
| [10-xiaozhi-assistant.md](10-xiaozhi-assistant.md) | AI 随身助手：双网络、传感器工具、摇一摇、构建与已知问题 |
| [11-ml307-component.md](11-ml307-component.md) | esp-ml307 组件结构、AT 通道与本地副本说明 |
| [12-code-review-backlog.md](12-code-review-backlog.md) | 代码审查遗留项：已修清单与暂缓项及后续方案 |
| [13-ble-v2-protocol.md](13-ble-v2-protocol.md) | App 对接 Demo 的 BLE V2 冻结协议：UUID、命令、JSON 字段与状态机 |

## 与 docs/ 的分工

`docs/` 记录这块板子本身的资料（原理图、板卡引脚手册），跟着硬件走，换工程也不变。本目录记录这套工程的知识（环境、架构、工作流、调参、排错），跟着代码走。硬件引脚细节只在 `docs/board.md` 维护一份，本目录的硬件章节只做提炼和引用，避免两处各写一遍后对不上。
