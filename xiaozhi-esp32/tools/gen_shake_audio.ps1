$ErrorActionPreference = 'Stop'
$ff = "C:\Users\19736\AppData\Roaming\TRAE SOLO CN\ModularData\ai-agent\vm\tools\python\lib\site-packages\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe"
$outDir = "D:\esp32\xiaozhi-esp32\main\assets\common"
$tmpDir = "D:\esp32\xiaozhi-esp32\build\tts_tmp"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

$lines = @{
    "shake_light"     = "嗯？"
    "shake_complain_1" = "哎哟，别摇了，我头都晕了！"
    "shake_complain_2" = "你轻点儿，我里面的零件都要散架了。"
    "shake_complain_3" = "再摇我可要罢工了啊。"
    "shake_complain_4" = "哎哟喂，我又不是摇奖机，摇不出奖的。"
    "shake_complain_5" = "别晃了别晃了，我眼前全是小星星。"
    "shake_complain_6" = "你是不是想把我摇成脑震荡啊。"
    "shake_complain_7" = "手下留情，我只是个小小的语音助手。"
    "shake_complain_8" = "哎哟，这么大力气，你是在摇奶茶吗？"
    "shake_complain_9" = "冷静冷静，再摇我就记仇了。"
    "shake_complain_10" = "我头晕，让我缓一缓好不好。"
}

foreach ($name in $lines.Keys) {
    $mp3 = Join-Path $tmpDir "$name.mp3"
    $ogg = Join-Path $outDir "$name.ogg"
    python -m edge_tts --text $lines[$name] --voice zh-CN-XiaoxiaoNeural --write-media $mp3 | Out-Null
    & $ff -y -hide_banner -loglevel error -i $mp3 -c:a libopus -b:a 24k -ar 16000 -ac 1 $ogg
    $size = (Get-Item $ogg).Length
    Write-Output "$name -> $size bytes"
}
