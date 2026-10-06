# 一次上车全量验证手册（ONETRIP）

目标：一趟车把 **Phase 0/1/3** 需要的全部真车数据采完，之后到 Phase 4 集成前都不用再上车。

## U 盘准备

FAT32 格式 U 盘，根目录放 `dist\usb\` 里的 4 个文件：

```
probe.exe          系统环境探针（弹窗即成功）
usbwatch.exe       USB 设备监测（常驻 4 分钟或点击退出）
bench.exe          性能/音频/触控基准（自动跑约 1-2 分钟）
bench_frame.jpg    bench 的测试图（必须与 bench.exe 同目录）
```

备用：`third_party\Ford-WINCE-Autokit\TC\cecmd.exe`（文件管理器，万一要现场翻文件系统）。
另带：iPhone + Lightning/数据线一根（必须支持数据传输，不能是纯充电线）。

## 执行顺序（全程约 15 分钟）

### ① probe.exe（2 分钟）
- 启动方式：设置→导航路径指向 U 盘里的 probe.exe，点导航启动
- 弹窗"probe done" = 成功；失败换 cecmd.exe 交叉验证
- **顺手拍照**：车机 设置→关于/系统信息 页面（若有版本号显示）

### ② usbwatch.exe（5 分钟，采集 Phase 1 生死数据）
启动后屏幕显示 WATCHING，然后按顺序操作（间隔看屏幕提示）：

| 步骤 | 操作 | 等待 | 观察/拍照 |
|---|---|---|---|
| 1 | 启动 usbwatch | 10s | 基线 dump 完成 |
| 2 | **插 iPhone（USB口1）** | 15s | iPhone 是否充电⚡；是否弹"信任"/"不支持此配件" |
| 3 | 拔 iPhone | 5s | — |
| 4 | **插 iPhone（USB口2，若有）** | 15s | 同上 |
| 5 | 拔 iPhone | 5s | — |
| 6 | 插一个 U 盘（对照） | 10s | 车机是否弹出音乐扫描（说明 host 正常） |
| 7 | 拔 U 盘 | 5s | — |
| 8 | 点屏幕退出 usbwatch | — | — |

### ③ bench.exe（5 分钟，采集 Phase 3 性能数据）
自动依次跑 S1-S6，屏幕逐行显示结果：
- S1-S4 自动（约 60-90 秒，画面会闪渐变色块 = 正常）
- **S5 播放 440Hz 音 3 秒 → 记录听没听到**
- S6 要求**点屏幕 5 下：左上、右上、左下、右下、正中** → 看红圈是否跟手（镜像/旋转会暴露）
- 显示 DONE 后点一下退出

### ④ 收尾
- U 盘插回电脑，取回 3 个日志：`probe_log.txt`、`usbwatch_log.txt`、`bench_log.txt`
- 交给 Claude 分析

## 回来后怎么判读（我会做，这里供参考）

| 数据 | 通过线 | 不通过的后果 |
|---|---|---|
| probe: OS | CE 6.x | 5.x 也可做，API 重查 |
| probe: CPU | ARM | 非 ARM 终止 |
| probe: USB VERDICT | ≥1 个 USB 驱动 | Phase 1 加驱动工作 |
| usbwatch: 插 iPhone 有 +++ ADDED | 有枚举事件 | 无 → 换线/换口重试，仍无则查 USB 口硬件 |
| bench: S2 BitBlt | ≥25 fps | <15 → 渲染方案改 DirectDraw/局部刷新 |
| bench: S4 MJPEG | ≥15 fps | H.264 软解压力更大，需降分辨率兜底 |
| bench: S5 | 听到音 | 无声 → 查音频焦点/路由 |
| bench: S6 | 红圈跟手 | 镜像/旋转 → 触控坐标变换 |

## iPhone 弹窗对照

- 只充电、无弹窗：正常（没 host 通信就不会弹信任框）
- 弹"信任此电脑？"：说明有枚举+通信（超预期的好消息）
- 弹"此配件不支持"：也说明枚举成功，只是未认证
