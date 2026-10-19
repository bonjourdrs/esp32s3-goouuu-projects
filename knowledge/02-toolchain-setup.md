# 环境搭建

本仓库的代码在 ESP-IDF v5.5.6、xtensa-esp32s3 工具链、Python 3.10.11 上验证。下面两条路线选一条走。

## 路线一 机器上已有 ESP-IDF

只要让 `tools/env.ps1` 能找到它。脚本默认读这两个路径：

```powershell
$env:IDF_PATH       = 'D:\Espressif\esp-idf'
$env:IDF_TOOLS_PATH = 'D:\Espressif\.espressif'
```

装在别处就在运行脚本前覆盖掉。这也是把 IDF 装在 D 盘而非默认用户目录的原因：源码路径短，能避开后面会提到的 MAX_PATH 问题。

## 路线二 从零安装

### 拿到 IDF 源码

国内直连 GitHub 慢且容易断，走 Gitee 镜像，并且只取需要的那个版本：

```powershell
git clone --depth 1 -b v5.5.6 https://gitee.com/EspressifSystems/esp-idf.git D:\Espressif\esp-idf
```

子模块是 ESP-IDF 的大头。Gitee 镜像的子模块地址是相对路径，用官方的 `git submodule update` 会因为解析不到主机而失败，需要先修正远端：

```powershell
cd D:\Espressif\esp-idf
git config --global url."https://gitee.com/EspressifSystems/".insteadOf "https://github.com/espressif/"
git submodule update --init --recursive --depth 1
```

### 安装工具链

工具包（编译器、OpenOCD、Python 环境）默认从 GitHub 下载，把源换成乐鑫 CDN 会快很多：

```powershell
$env:IDF_TOOLS_PATH    = 'D:\Espressif\.espressif'
$env:IDF_GITHUB_ASSETS = 'dl.espressif.com/github_assets'
$env:PIP_INDEX_URL     = 'https://mirrors.aliyun.com/pypi/simple'
cd D:\Espressif\esp-idf
.\install.ps1 esp32s3
```

`esp32s3` 参数只装 ESP32-S3 相关工具，比 `all` 省一半时间和空间。

### 解决 git 缺失

`idf.py` 在配置阶段会调用 git 取版本号。系统里没装 Git、又不方便走安装包（会弹 UAC 提权窗口）时，用免安装的便携版 MinGit：解压到 `D:\Espressif\MinGit`，不需要管理员权限，也不需要写进系统 PATH，由 `tools/env.ps1` 在运行时临时加进 PATH。

### 验证

```powershell
. D:\Espressif\esp-idf\export.ps1
idf.py --version        # 应输出 ESP-IDF v5.5.6
```

再把板子插上，确认能识别芯片：

```powershell
esptool.py -p COM4 flash_id
```

输出里能看到 `Chip is ESP32-S3`、`Detecting Flash Size`、以及 8 MB PSRAM 相关字样，就说明下载通路也通了。

## 这份环境的实测记录

| 组件 | 版本 / 路径 |
|---|---|
| ESP-IDF | v5.5.6 · `D:\Espressif\esp-idf` |
| Python | 3.10.11 · `D:\Espressif\.espressif\python_env\idf5.5_py3.10_env` |
| 便携 Git | MinGit 2.56.0 · `D:\Espressif\MinGit` |
| 构建目录 | `D:\esp32\build\<工程名>` |

更细的环境变量与已知问题见 [docs/environment.md](../docs/environment.md)。
