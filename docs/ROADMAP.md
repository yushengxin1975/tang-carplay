# 唐 2015 WinCE 车机直连 CarPlay 项目

在比亚迪唐 2015 旗舰版（WinCE 6.0 ARM 车机）上**不借助任何外置盒子**，直接实现有线 CarPlay 接收器。

## 硬件/软件前提（已调研确认）

- 车机 OS：Windows CE（内核 6.0，ARM，10 寸屏）— 待 probe.exe 上车最终确认
- USB 口为 host 模式且可与手机做数据通信（亿连 2015 款唐 USB 投屏成功佐证）
- 可通过 U 盘 + 导航路径运行第三方 CE 程序

## 阶段计划

### Phase 0 — 上车环境探测（probe.exe）
- [x] 编写 probe.c（本仓库 probe/）
- [ ] U 盘 FAT32 放 probe.exe，车机上以导航路径/U 盘入口运行
- [ ] 确认：CE 版本 = 6.0、CPU = ARMv4I/ARMv6、分辨率、内存、USB host 驱动、存储卡挂载点
- **判定：任一项不符 → 记录并评估影响；USB host 不可用 → 项目终止，转盒子/换屏方案**

### Phase 1 — WinCE USB host ↔ iPhone 通信（已大幅降级）
- [x] 找到 VNC Automotive CEUSBKWrapper（LGPL, GitHub 已克隆）—— AutoKit 发行包里的 `CEUSBBOX.dll` = `ceusbkwrapperdrv.dll`（USBD 客户端内核驱动），`ceusbkwrapper.dll` = Ukw 用户库，**已在你车上验证过**
- [x] 注册表机制：`HKLM\Drivers\USB\ClientDrivers\Usb_Kernel_Wrapper` + `LoadClients\<VID>\Default\Default\Usb_Kernel_Wrapper`
- [ ] 上车实测（`usbtest.exe`）：装两条 VID 绑定（Apple 0x05AC=1452 + Magic 1314），跑 UkwOpenDriver → 枚举 → 控制传输 → claim
- [ ] iAP2 / CarPlay 协议栈移植（**仅在 Phase 1 通过后开展**）

> 参考：第三方 CEUSBKWrapper（用户态 libusb-win32 风格）+ maaiika/Carplay 泄露 SDK + xcertplay 协议层

### Phase 2 — iAP2 / CarPlay 协议栈移植（C++）
- [ ] PC 上先用 xcertplay + Python(wiomoc/iap2) 抓完整握手流程存档
- [ ] C++ 实现 iAP2 link/session 层（对照 maaiika/Carplay 泄露 SDK 的 Transport 层）
- [ ] 软件认证：加载 .p7b 证书 + .pk8 私钥（Carlinkit 提取身份，xcertplay 已验证该路线）
- [ ] CarPlay 会话建立：HiFi 流 + 触控/按键/车辆状态回传

### Phase 3 — 音视频渲染
- [ ] ffmpeg CE6 ARM 软解 H.264（分辨率按车机屏，目标 800x480@30）
- [ ] DirectDraw/GDI 渲染 + 帧率调优（carlive 的渲染管线直接复用）
- [ ] AAC-LC 解码 → WaveOut 播放；waveIn 采集麦克风（Siri）
- [ ] 触控事件 → iAP2 触控消息回传 iPhone

### Phase 4 — 集成与打磨
- [ ] 常驻进程 + 导航路径一键启动
- [ ] 白天/黑夜模式、音频焦点（倒车雷达/导航播报优先）
- [ ] 长期稳定性、iOS 升级兼容性跟踪

## 参考仓库（third_party/，不入库）

| 目录 | 来源 | 用途 |
|---|---|---|
| xcertplay | shilapi/xcertplay | 完整软件 CarPlay 接收器（Kotlin），协议逻辑参考 |
| carlive | samcaicn/carlive | WinCE6 C++ 收流/ffmpeg/GDI/触控骨架，编译链路照抄 |
| AccessorySDK | maaiika/Carplay | 苹果 CarPlay 配件 SDK 泄露版 C++，核心协议实现 |
| FordAutoKit | typlo-gh/Ford-WINCE-Autokit | TC CE 文件管理器 + AutoKit CE 客户端（逆向分析用） |

## 风险清单

1. 软件认证身份可能被 iOS 更新封杀（DiPlay 同风险，无法规避）
2. CE6 SoC H.264 软解性能不足 → 降帧率/降分辨率兜底
3. 苹果 AccessorySDK 泄露源码的 C++ 标准较新，需降级适配 VS2008/cegcc
4. 法律灰色：MFi 逆向 + 泄露 SDK，仅限自用，勿分发固件
