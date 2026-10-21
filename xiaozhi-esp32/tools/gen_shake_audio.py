import os
import subprocess
import sys

FFMPEG = r"C:\Users\19736\AppData\Roaming\TRAE SOLO CN\ModularData\ai-agent\vm\tools\python\lib\site-packages\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe"
OUT_DIR = r"D:\esp32\xiaozhi-esp32\main\assets\common"
TMP_DIR = r"D:\esp32\xiaozhi-esp32\build\tts_tmp"
VOICE = "zh-CN-XiaoxiaoNeural"

LINES = {
    "shake_light": "嗯？",
    "shake_complain_1": "哎哟，别摇了，我头都晕了！",
    "shake_complain_2": "你轻点儿，我里面的零件都要散架了。",
    "shake_complain_3": "再摇我可要罢工了啊。",
    "shake_complain_4": "哎哟喂，我又不是摇奖机，摇不出奖的。",
    "shake_complain_5": "别晃了别晃了，我眼前全是小星星。",
    "shake_complain_6": "你是不是想把我摇成脑震荡啊。",
    "shake_complain_7": "手下留情，我只是个小小的语音助手。",
    "shake_complain_8": "哎哟，这么大力气，你是在摇奶茶吗？",
    "shake_complain_9": "冷静冷静，再摇我就记仇了。",
    "shake_complain_10": "我头晕，让我缓一缓好不好。",
}

os.makedirs(TMP_DIR, exist_ok=True)
os.makedirs(OUT_DIR, exist_ok=True)

for name, text in LINES.items():
    mp3 = os.path.join(TMP_DIR, name + ".mp3")
    ogg = os.path.join(OUT_DIR, name + ".ogg")
    subprocess.run(
        [sys.executable, "-m", "edge_tts", "--text", text, "--voice", VOICE,
         "--write-media", mp3],
        check=True,
    )
    subprocess.run(
        [FFMPEG, "-y", "-hide_banner", "-loglevel", "error", "-i", mp3,
         "-c:a", "libopus", "-b:a", "24k", "-ar", "16000", "-ac", "1", ogg],
        check=True,
    )
    print(f"{name} -> {os.path.getsize(ogg)} bytes")
