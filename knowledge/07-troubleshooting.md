# 排错

按现象查。每条给出可能原因和处理方式，涉及参数调整的细节在 [06-tuning.md](06-tuning.md)。

## 编译与构建

### CMake 报路径过长，提到 CMAKE_OBJECT_PATH_MAX

源码或构建目录的路径超过 Windows 260 字符上限。把构建目录移到短路径，`tools/` 的脚本默认已用 `D:\esp32\build\<工程名>`。自己新增深层目录时也要一并考虑。

### 提示找不到 esp_driver_i2s.h 之类的头文件

`main/CMakeLists.txt` 少声明了组件依赖。ESP-IDF 5.x 把 I2S、RMT 拆成了独立组件，用哪个就得在 `PRIV_REQUIRES` 里写上，对照 [04-architecture.md](04-architecture.md) 的依赖表。

### 提示 project name 重复

同一个构建目录里改过 `project()` 的名字。删掉那个构建目录重新构建。

### 禁止运行脚本

PowerShell 执行策略限制。脚本内部已用 `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass` 放开当前进程；手动 dot-source `export.ps1` 遇到这个报错时，先手动执行同一条命令。

### 提示找不到 git

系统 PATH 里没有 git。用 `tools/env.ps1`（它会把 `D:\Espressif\MinGit\cmd` 加进去），或者手动：

```powershell
$env:Path = 'D:\Espressif\MinGit\cmd;' + $env:Path
```

### idf.py 命令不存在，或报找不到 CMakeLists.txt

没先加载 IDF 环境，或者工作目录不对。脚本已经处理这两件事；手动操作时先 `. D:\Espressif\esp-idf\export.ps1`，再进工程目录执行。

## 烧录

### 端口被占用，打开串口失败

上一个 monitor 进程没退干净。在 monitor 里按 `Ctrl+]` 正常退出；已经卡住的，关掉残留的 `python.exe` 或拔插一次 USB。

### 找不到 COM 口

先确认 USB 线能传数据，很多线只供电。再看设备管理器里有没有出现串口设备；没有就按住 BOOT 键再插一次。端口号在不同 USB 口之间会变，脚本默认 COM4，实际不同要传 `-Port`。

### 烧录中途失败，提示校验错误

供电不稳或线材质量差。换 USB 口（尤其别接外接 hub），或换线重试。

## 运行与外设

### 串口反复重启，打印 rst:0x...

看复位原因。`RTCWDT_RTC_RESET`、`TG0WDT_SYS_RST` 这类多半是任务死循环没让出 CPU 或看门狗超时，检查任务里有没有漏掉 `vTaskDelay`。

### 灯完全不亮

先确认 GPIO 号：底板是 GPIO48，旧版 DevKitC-1 是 GPIO38。再用 menuconfig 确认 LED 类型选的是 WS2812 而不是普通 GPIO。最后看串口有没有打印 `WS2812: ready on GPIO48`。

### 灯颜色不对，红绿颠倒

WS2812 按 GRB 顺序接收，代码里已做过转换。如果改过 `ws2812.c` 的发送部分，检查是不是把顺序弄回了 RGB。

### 麦克风读数一直是 0

三种可能：`L/R` 引脚没接 GND，悬空时可能选到右声道而配置读的是左声道；SD/SCK/WS 接错脚；I2S 的槽模式与数据位宽不匹配。先量一下 `L/R` 是否确实接地。

### 麦克风读数一直偏高，安静时灯也亮着

环境噪声底高于归一化下限。按 [06-tuning.md](06-tuning.md) 把 `MIC_FLOOR_DB` 调大。

### 扬声器没声音

功放的 `SD` 脚要拉高才工作（底板接 3V3），`GAIN` 决定增益（接 GND 是 9 dB）。再看串口有没有 `AUDIO: MAX98357 ready`。都正常就检查 DIN/BCLK/LRC 是否接到 7/15/16。

### 扬声器有持续底噪

I2S 通道没开 `auto_clear` 时会反复输出最后一帧，听起来像卡住，代码里已经开了。此外功放输入端走线长容易拾取噪声，属硬件层面问题。

## 与 GitHub 同步

### git clone 卡住不动

本机 git 配置把 `github.com` 重写成了 `gh-proxy.com` 镜像，镜像不通就会卡住。先看当前规则：

```powershell
git config --get-regexp url
```

镜像临时不可用时，可以先把这条规则摘掉，用完再补回：

```powershell
git config --global --unset url."https://gh-proxy.com/https://github.com/".insteadOf
git config --global url."https://gh-proxy.com/https://github.com/".insteadOf "https://github.com/"
```

### 推送成功但 GitHub 上看不到改动

确认推的是哪个分支、有没有真的产生提交。`git status -sb` 能看到本地分支与远端的关系，`git log --oneline -3` 确认提交在不在。
