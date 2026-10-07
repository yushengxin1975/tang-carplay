# 唐 2015 WinCE 车机直连 CarPlay 项目（路线 v3）

在比亚迪唐 2015 旗舰版（MT3360/WinCE 6.0）上实现 iPhone 直连 CarPlay。双轨推进：

## 已确认硬件（probe v0.1 日志 + v0.3 实测，见 docs/HARDWARE.md）

- SoC：MediaTek MT33XX（MT3360 系），ARM1176JZF-S (procType 2934) 单核 ARMv6
- GPU：Mali-200 + UMP（零拷贝显示管线可用）
- RAM 384MB（free 282MB）｜屏 1024×600@16｜双 USB HCD（HCD1/HCD2）
- 视频硬解：VDEC（经 OMX_CORE.DLL + VDEC_DCH60.dll 调用，AutoKit 包内即有）
- 挂载：\SDMemory、\SDMemory2｜车控 MCU：LPC1758（VirtualSerial 通道）

## 关键认知修正（2026-10-07）

1. iPhone 15 = USB-C，插有数据功能的主机即完成 USB 枚举——Lightning 时代的
   USB Restricted Mode 叙事不适用，无需任何 PD 唤醒线
2. usbwatch 看不到 iPhone 事件 ≠ 没枚举——Drivers\Active 只显示已绑定客户端驱动的
   设备；BYD 原厂无 Apple VID 驱动所以无条目。装上 LoadClients\1452 即可被 UKW 认领
3. 写 TF 卡 FAT 会触发 BYD 防破解（probe v0.1 教训）；注册表 + \Windows 写入是
   AutoKit 每次运行的常规操作，安全
4. LoadClients 键名用十进制 VID：Apple 0x05AC=1452，MagicComm 0x1314=4884

## 双轨计划

### 轨道一（短期）：patched AutoKit 2019
- [x] 定位版本字符串 0x421FC（UTF-16LE "2019.06.27-2715"）
- [x] patch 为 "2018.06.20-2021"（5 字节差异，全在 30 字节目标区间内）
- [x] 部署到 D:\gps_byd（startup.exe / AutoKit.exe + 卡上原版备份）
- [ ] 上车验证：Carlinkit 主界面 → 插 iPhone → CarPlay
- 失败时：逆向 \temp\astepversion 找二级校验

### 轨道二（长期）：直连 iPhone（本路线，Phase 编号沿用）

#### Phase 1 — USB 栈绑定 iPhone（usbtest v0.2 零写卡版）
- [x] 复用 CEUSBKWrapper（CEUSBBOX.dll + ceusbkwrapper.dll，车上已验证）
- [x] 注册表：ClientDrivers\Usb_Kernel_Wrapper(UKW) + LoadClients\1452 + \4884
- [x] 零写卡改造（屏幕输出；\Windows 拷贝 + 注册表写入保留——AutoKit 常规操作）
- [ ] 上车验证四项：UKW_STACK / DEVICE_ENUM / CTRL_XFER / CLAIM
- 通过线 = USB 层全通，进入 Phase 2

#### Phase 2 — iAP2 + 软件 MFi 认证（PC 上开发为主）
- [ ] usbmux 帧层（参考 libimobiledevice/usbmuxd 协议，跑在 Ukw bulk 传输上）
- [ ] iAP2 link/session 层（参考 maaiika/Carplay 泄露 SDK 的 Transport 层，C++）
- [ ] PlatformPOSIX → PlatformWCE 移植层
- [ ] 软件认证：.p7b 证书 + .pk8 私钥（xcertplay 已验证该路线，DiPlay 同源）
- [ ] CarPlay 会话建立（HiFi 流 + 触控/按键回传）

#### Phase 3 — 音视频管线（MT3360 硬解，架构升级）
- [ ] OMX 硬解 H.264：复用 AutoKit 包内 OMX_CORE.DLL + VDEC_DCH60.dll
      （比 ffmpeg 软解强一个量级；ARM1176 软解只是兜底）
- [ ] UMP 零拷贝 → DirectDraw 显示（Mali-200 合成）
- [ ] 音频：AAC → libfdk-aac-0.dll（AutoKit 包内有）→ waveOut
- [ ] 麦克风采集（Siri）→ waveIn
- [ ] 触控事件 → iAP2 触控消息

#### Phase 4 — 集成
- [ ] 常驻 + startup.exe 入口（车机只认此文件名，换程序=换文件内容）
- [ ] 方向盘按键：LPC1758 MCU（VirtualSerial）通道
- [ ] 音频焦点（倒车提示/导航播报优先）
- [ ] iOS 升级兼容性跟踪（软件身份风险，与 DiPlay 同）

## 参考仓库（third_party/，不入库）

| 目录 | 来源 | 用途 |
|---|---|---|
| xcertplay | shilapi/xcertplay | 完整软件 CarPlay 接收器（Kotlin），协议+软件认证参考 |
| AccessorySDK | maaiika/Carplay | 苹果 CarPlay 配件 SDK 泄露版 C++，iAP2 核心参考 |
| carlive | samcaicn/carlive | WinCE6 C++ 渲染/音频骨架 + CeGCC 编译链 |
| CEUSBKWrapper | VNCAutomotive/CEUSBKWrapper | UKW USB 驱动开源上游（LGPL） |
| ludwig-cplay2air | ludwig-v/... | CPlay2Air 无线盒固件逆向（参考价值有限，非 CPC200 有线） |
| FordAutoKit | typlo-gh/Ford-WINCE-Autokit | 2019 AutoKit 发行包来源 + TC CE 工具 |

## 风险清单

1. 软件身份可能被 iOS 更新封杀（DiPlay 同风险）
2. AccessorySDK 泄露源码需降级适配 CeGCC/ARMv6
3. OMX 接口无文档——逆向 AutoKit 调用序列（PAL2.dll/D2.dll）
4. 法律灰色：MFi 逆向 + 泄露 SDK，仅限自用
5. 写卡触发 BYD 防破解——所有工具必须零写卡
