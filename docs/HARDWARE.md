# 唐 2015 车机硬件档案（probe 实测）

来源：probe v0.1 日志（StartUp_log.txt，数据恢复所得）+ v0.3 屏幕实测。

## 核心平台

| 项 | 值 | 备注 |
|---|---|---|
| PLATFORMTYPE | **MT33XX** | MediaTek MT3360 系车机 SoC |
| OS | Windows CE 6.0 (build 0, platform=3) | |
| CPU | ARM **procType=2934 (0xB76) = ARM1176JZF-S**，单核，level=4 rev=7 | ARMv6，无 NEON |
| RAM | 393340KB total / 288984KB avail | 384MB 物理 |
| 屏幕 | 1024×600 @ 16bpp | |
| GPU | Mali-200 + UMP（BuiltIn 驱动列表实锤） | UMP = 零拷贝内存分配器 |
| 视频硬解 | VDEC（TVD/AUDDEC 同族）| 经 OMX_CORE.DLL + VDEC_DCH60.dll 调用 |
| 挂载 | \SDMemory、\SDMemory2 | TF 卡槽 ×2 |
| USB | HCD1: + HCD2: 双主控已加载 | |

## BuiltIn 驱动要点（共 50 项）

UsbOtg / USBHCD1（USB）、MALI / UMP（GPU）、TVD（倒车视频）、AUDDEC（音频解码）、
SDHC×3、GPS、NANDFLASH、fastcamera、**LPC1758**（车控 MCU，方向盘按键/CAN）、
VirtualSerial（用户态串口，通 LPC1758 的通道）、BtA2dpSnd + HciExt（蓝牙音频）、
CMMDRV、WCM、Misc、CLI

## \Windows 关键 DLL 存在性

| DLL | 状态 | 用途 |
|---|---|---|
| ddraw.dll | ✅ | DirectDraw（渲染） |
| ws2.dll | ✅ | Winsock |
| waveapi.dll | ✅ | 音频 |
| usbmsc.dll | ✅ | USB 大容量存储（原车 U 盘功能） |
| aygshell.dll / toolhelp.dll | ✅ | shell / 进程快照 |
| uhcd/ohcd/ehci/hcd.dll | ❌（自定义 HCD 命名） | HCD1:/HCD2: 实际加载 |
| nk.bin | ❌（XIP 内核） | |

## AutoKit 发行包与硬件的对应

- `hwdec_mt33XX` 标记 = 本平台硬解开关
- `OMX_CORE.DLL` (1.4MB) + `VDEC_DCH60.dll` = MT3360 VDEC 硬解管线
- `swscale-3.dll` / `avcodec-56.dll` = 软解兜底路径
- `usehwdec` 标志文件 = 启用硬解（2019 版新增）
- 结论：AutoKit 在本机走 OMX 硬解；直连项目 Phase 3 直接复用同一管线

## 运行进程

toolhelp 快照返回空（静默失败）——Phase 4 集成时换 EnumProcesses 方式再取。
