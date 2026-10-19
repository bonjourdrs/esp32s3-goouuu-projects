# 开发工作流

## 三个脚本

`tools/` 下四个 PowerShell 脚本，职责单一：

| 脚本 | 作用 | 参数 |
|---|---|---|
| `env.ps1` | 设置 `IDF_PATH`、`IDF_TOOLS_PATH`，把 MinGit 加进 PATH，再加载 `export.ps1` | 无，被其他脚本 dot-source |
| `build.ps1` | 编译指定工程 | `-Project`，可选 `-BuildDir` |
| `flash.ps1` | 烧录到指定端口 | `-Project`、`-Port`（默认 COM4） |
| `monitor.ps1` | 打开串口监视 | `-Project`、`-Port`（默认 COM4） |

三个脚本都先 `Push-Location` 到工程目录再执行 `idf.py`。这一步不能省：`idf.py -B` 指定的是构建目录，工作目录不对它找不到工程。

## 构建目录为什么放在外面

源码路径里带空格、层级又深时，ESP-IDF 的对象文件路径会超过 Windows 的 260 字符上限，CMake 直接报错。所以构建目录固定放短路径 `D:\esp32\build\<工程名>`。想换位置，先设环境变量：

```powershell
$env:ESP32_BUILD_ROOT = 'E:\build'
.\tools\build.ps1 -Project 03_mic_level
```

## 新增一个工程

复制一个现成工程最快。以新增 `04_xxx` 为例：

1. 拷 `03_mic_level` 目录，改名 `04_xxx`
2. 改 `CMakeLists.txt` 里的 `project(...)`，名字不能和已有的重复
3. 改 `main/CMakeLists.txt` 的 SRCS，只留这个工程真正用到的源文件
4. 改 `main/main.c`
5. 用 `build.ps1 -Project 04_xxx` 编一次，确认过

第 3 步别偷懒。SRCS 里留着没用到的文件会白编译一遍，也可能因为缺少对应组件依赖而报错。

## 命名与约定

工程目录用两位数字前缀加英文短名，数字只表示阅读顺序，不代表依赖关系。源文件一个模块一对 `.c/.h`，模块名即文件名。日志 TAG 用大写短词（`BLINK`、`SPEAKER`、`MIC`、`WS2812`），串口上一眼能看出是哪一路在输出。

## 提交与推送

```powershell
$env:Path = 'D:\Espressif\MinGit\cmd;' + $env:Path
git add <改动>
git commit -m '说明'
git push
```

这台机器的 git 配置里有两条 URL 重写规则：拉取时把 `github.com` 换成 `gh-proxy.com` 镜像，推送时强制直连 `github.com`。所以 `git remote -v` 里 fetch 和 push 显示的是不同地址，这是正常的。克隆别的仓库时如果卡在拉取阶段，可以先用 `git config --get-regexp url` 看看是不是被镜像规则影响了。

## 什么不入库

`build/`、`.build/`、`sdkconfig`、`*.log` 都由根目录 `.gitignore` 排除。`sdkconfig` 是生成物，改配置要改 `sdkconfig.defaults` 才有意义——只改 `sdkconfig` 又不提交，别人拉下来重新构建就丢了。构建日志同理，留在本地 `D:\Espressif\` 下即可。
