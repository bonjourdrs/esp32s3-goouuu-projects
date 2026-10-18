# 开发环境

## 工具链版本

| 组件 | 版本 / 路径 |
|---|---|
| ESP-IDF | **v5.5.6** · `D:\Espressif\esp-idf` |
| 目标芯片 | ESP32-S3（xtensa-esp32s3-elf） |
| Python | 3.10.11 · IDF 专用环境 `D:\Espressif\.espressif\python_env\idf5.5_py3.10_env` |
| 工具安装目录 | `D:\Espressif\.espressif` |
| 便携 Git | MinGit **2.56.0** · `D:\Espressif\MinGit`（不在系统 PATH） |
| 构建目录 | `D:\esp32\build\<工程名>` |

## 环境变量

三个脚本（`tools\build.ps1` / `flash.ps1` / `monitor.ps1`）都会 dot-source `tools\env.ps1`，
由它统一设置：

```powershell
$env:IDF_PATH       = 'D:\Espressif\esp-idf'
$env:IDF_TOOLS_PATH = 'D:\Espressif\.espressif'
$env:Path           = 'D:\Espressif\MinGit\cmd;' + $env:Path
. "$env:IDF_PATH\export.ps1"
```

这三项都可在运行脚本前自行设置以覆盖默认值，例如换到别的 IDF 安装位置：

```powershell
$env:IDF_PATH = 'E:\esp-idf'
.\tools\build.ps1 -Project 03_mic_level
```

## 构建 / 烧录 / 监视

```powershell
.\tools\build.ps1   -Project 01_blink_led
.\tools\flash.ps1   -Project 01_blink_led -Port COM4
.\tools\monitor.ps1 -Project 01_blink_led -Port COM4
```

`-BuildDir` 可选，用来显式指定构建目录。构建产物默认在 `D:\esp32\build\<工程名>`，
可用环境变量 `ESP32_BUILD_ROOT` 改根目录。

## 已知问题与规避

| 问题 | 规避方式 |
|---|---|
| Windows `MAX_PATH`（260 字符）导致 ESP-IDF 对象路径超限 | 源码与构建目录都放在短路径下；本仓库根为 `D:\esp32\esp32s3-goouuu-projects`，构建目录在 `D:\esp32\build\` |
| PowerShell 执行策略禁止运行脚本 | 各脚本首行用 `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass` 临时放开 |
| `git` 不在系统 PATH，`idf.py` 需要它 | 由 `tools\env.ps1` 把 `D:\Espressif\MinGit\cmd` 加进 PATH |
| `idf.py -B` 与当前工作目录不一致导致找不到工程 | 脚本先 `Push-Location` 到工程目录再执行 `idf.py` |
| 国内下载 GitHub 子模块慢 | 使用 Gitee 镜像 / `dl.espressif.com` 资源加速（仅安装期需要） |

## 目录约定

- `sdkconfig` 是构建期生成文件，不入库；每个工程只提交 `sdkconfig.defaults`。
- 构建目录（`build/`、`.build/`）与日志文件已由根目录 `.gitignore` 排除。
