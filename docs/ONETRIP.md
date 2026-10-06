# 一次上车全量验证手册（ONETRIP v2，含真 USB 栈）

目标：一趟车把 Phase 0/1/3 需要的全部真车数据采完。Phase 1 这次是真的 USB 栈实战——复用 AutoKit 发行包里跑通过的内核驱动。

## U 盘准备

FAT32 格式 U 盘，根目录放齐：

    probe.exe            系统环境探针
    usbtest.exe          USB 栈实战（需 CEUSBBOX.dll + ceusbkwrapper.dll 一起放）
    CEUSBBOX.dll         UKW 内核驱动（从 AutoKit_WinCE\BIN 拷出，你车上验证过）
    ceusbkwrapper.dll    UKW 用户库（同上）
    bench.exe            性能/音频/触控基准
    bench_frame.jpg      bench 的测试图
    usbwatch.exe         备用：纯注册表差分（旧版备选，usbtest 起不来时用）

外加：iPhone + 一根真数据传输线（不是纯充电线），备用 Lightning 转接头（如 USB-C iPhone）。

## 执行顺序（约 18 分钟）

### ① probe.exe（1 分钟）
- 设置→导航路径指向 probe.exe，点导航图标启动
- 弹窗即成功；不通换 usbwatch.exe / Total Commander 交叉验证

### ② usbtest.exe ★ 重点（6 分钟，真 USB 栈）
启动后屏幕显示 "PLUG IPHONE NOW!"（说明驱动已写好注册表、内核驱动就位），然后：

    步骤   操作                                          等      看什么
    1     启动 usbtest                                   10s    屏幕"E: PLUG IPHONE NOW!" 出现（=注册表+驱动 DLL 已部署）
    2     插 iPhone（USB口1）                              15-20s 屏幕第3-5 行变成 ENUM:YES XFER:YES CLAIM:YES 或 NO
    3     拔 iPhone                                       5s    —
    4     （若有 USB口2）插口2                              15s   同上
    5     拔                                               —     —
    6     屏幕显示 DONE - tap to exit 后点一下退出          —     —

拍照：iPhone 是否充电、是否弹"信任"/"不支持"提示。

### ③ bench.exe（5 分钟）
- 自动跑 S1-S6（屏幕闪渐变色块 = 正常）
- S5 听 440Hz 3 秒 → 记有没有声音
- S6 点屏幕 5 下（4 角 + 正中）→ 看红圈是否跟手
- 出现 DONE 后点一下退出

### ④ 收尾
- 取回 U 盘带回 4 个日志：probe_log.txt、usbtest_log.txt、usbwatch_log.txt（仅旧版用）、bench_log.txt

## 判读（回家）

    日志                                              期望                            否则
    probe: OS                                          CE 6.x                          5.x 重查
    usbtest: UKW_STACK VERDICT: UkwOpenDriver OK        是                              内核驱动未加载 → 注册表失败或 \Windows 只读（已尝试 SD 全路径回退）
    usbtest: DEVICE_ENUM: Apple 设备已枚举              是                              换线/换口重试
    usbtest: CTRL_XFER VERDICT: 控制传输成功            是（这是真数据流！不只枚举）     进入枚举但 iPhone 未认证/未解锁，需先在 iPhone 上"信任"或换根线
    usbtest: CLAIM VERDICT: 接口抢占成功                是                              iAP2 接口被车机系统自带驱动占住，需要更强的 InterfaceFilter
    bench: S2 BitBlt                                   ≥25 fps                         <15 → 改 DirectDraw
    bench: S4 MJPEG                                    ≥15 fps                         H.264 更重，需降分辨率
    bench: S5                                          听到音                           改音频路由
    bench: S6                                          红圈跟手                        加触控坐标变换

## iPhone 弹窗

- 只充电、没弹窗 = 正常（没通信就不会弹信任）
- 弹"信任此电脑" = 有通信（超预期）
- 弹"配件不支持" = 也说明枚举成功（信号已到）

## Phase 1 的真正门槛

UKW_STACK OK + DEVICE_ENUM YES = 车机 USB host 能枚举 iPhone，Phase 1 上半场过了。
加上 CTRL_XFER YES = 数据传输双向通，Phase 1 实质完成，iAP2 / CarPlay 协议层可以直接在 PC 上开发，下次上车就是联调。
