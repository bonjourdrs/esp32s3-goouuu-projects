# MPU6050 摇晃变声

接线：MPU6050 SDA→GPIO8，SCL→GPIO9，VCC→3V3，GND→GND。自动识别地址 0x68/0x69。MAX98357 使用 GPIO15 BCLK / GPIO16 LRC / GPIO7 DIN。

启动时短暂保持静止。摇晃越强，音量越大、音高越高（220–1760 Hz）；停止后声音淡出。低通估计重力后计算三轴动态加速度，带噪声门和平滑。转动板子也可能产生短暂声音。采样失败立即静音；未识别到 MPU6050 时输出接线提示。

在仓库根目录运行 tools/build.ps1 -Project 04_mpu6050_sound，随后 tools/flash.ps1 -Project 04_mpu6050_sound -Port COM4。

调参在 main/main.c：NOISE_GATE_G 是静止噪声门；FULL_SHAKE_G 越小越灵敏；MAX_AMPLITUDE 控制最大音量。

寄存器依据：https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf

实板识别：I2C 地址 0x68、WHO_AM_I=0x70，对应 MPU6500。程序兼容 MPU6050（0x68）和 MPU6500（0x70），并单独配置 MPU6500 加速度低通滤波器。
官方参考：https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6500-Register-Map2.pdf
