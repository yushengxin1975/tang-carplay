# 项目交接文档 — BYD 唐 2015 Carlinkit 客户端开发

> 生成时间：2026-10-11 | 最后 commit：`bc479b4`（静态链接 pc_main）
> 仓库：https://github.com/yushengxin1975/tang-carplay（私有）

## 一、项目目标

为比亚迪唐 2015 旗舰版（MT3360 / WinCE 6.0 / ARM1176 / 1024×600）编写**自主 Carlinkit 客户端**，替代闭源 AutoKit WinCE（已停更、有 4 个已知缺陷），实现 iPhone → Carlinkit 盒子 → 车机屏的完整 CarPlay 体验。

## 二、当前状态（截至交接时）

| 模块 | 状态 | 说明 |
|---|---|---|
| 协议文档 | ✅ 完成 | `docs/CARLINKIT_PROTOCOL.md`（20 条消息、AES、状态机） |
| 硬件档案 | ✅ 完成 | `docs/HARDWARE.md`（MT3360/OMX/Mali/USB 双 HCD） |
| PC 协议层 C++ | ✅ 完成 | `client/cpp/protocol/`（Message/AES-CBC+CFB128/Connection） |
| PC 单元测试 | ✅ **36/36 全过** | 含 NIST FIPS-197 AES + NIST SP800-38A CFB128 KAT，双平台 CI |
| PC USB transport | ✅ 编译通过 | libusb-1.0 **动态加载**（零链接依赖），OUT 端点 0x02 已修正，alt-setting+clear-halt 默认开 |
| pc_main 控制台工具 | ✅ **真机运行** | `--enc`（默认）/`--plain` 双模式，静态链接自包含 exe，plain 模式写入 0 错误 |
| CE 客户端可执行 | ✅ **编译链接成功** | `carlinkit_ce.exe` ARM 80KB，CI 产出 |
| **pc_main 真机测试** | ✅ **重大突破！M1.5b 达成** | 成功识别 iPhone 15（CMD_PLUGGED），拉起 CarPlay 会话，以 30fps 持续接收 2000+ 帧 H.264 视频流（12+ MB），SPS/PPS/IDR 帧完整落盘！ |
| CE 上车集成 | ❌ 未开始 | M2 里程碑 |
| 媒体层（视频/音频解码） | ⏳ **进行中** | 视频流已捕获，接下来进行 H.264 解码与界面渲染 |

## 三、硬件事实（已验证，勿重查）

| 项 | 值 | 验证方式 |
|---|---|---|
| SoC | MediaTek MT3360 (MT33XX) | probe PLATFORMTYPE |
| CPU | ARM1176JZF-S (procType 2934) 单核 ARMv6 | probe |
| RAM | 384MB total / 282MB free | probe |
| 屏幕 | 1024×600 @ 16bpp | probe |
| GPU | Mali-200 + UMP | BuiltIn 驱动列表 |
| 视频硬解 | VDEC（经 OMX_CORE.DLL + VDEC_DCH60.dll） | AutoKit 包内 DLL |
| USB | 双主控 HCD1: + HCD2: | probe Drivers\Active |
| TF 卡挂载 | `\SDMemory`、`\SDMemory2` | probe |
| 车控 MCU | NXP LPC1758（经 VirtualSerial） | BuiltIn 列表 |
| Carlinkit 盒子 | VID=0x1314 PID=0x1520 USB 2.0 HS | Windows 设备管理器 |

## 四、关键教训（踩坑记录，务必阅读）

### 4.1 BYD 车机限制
- **写 TF 卡会触发 BYD 防破解**：轻则 FAT 损坏（RAW），重则车机重启
- **安全操作**：注册表写入 + `\Windows` 文件拷贝（AutoKit 每次运行都做）
- **所有上车工具必须零写卡**：输出只到屏幕，退出不落盘

### 4.2 BYD 启动器
- 导航键只启动 `\SDMemory\gps_byd\startup.exe`（固定文件名）
- 换工具 = 换 `gps_byd\startup.exe` 内容（多份副本放卡上，用 Total Commander CE 切换）

### 4.3 CeGCC 9.3 (arm-mingw32ce-g++) 编译坑
| 问题 | 原因 | 修法 |
|---|---|---|
| `std::mutex` 不存在 | libstdc++ 无 gthreads | `ce_compat.h`: CRITICAL_SECTION 包装 |
| `::atoll` 编译失败 | cstdlib 声明缺失 | `-include ce_atoll_patch.h` + ce_stubs.cpp |
| `ERROR` 枚举名冲突 | Win32 `ERROR` 宏 | 改名 `ERROR_STATE` |
| `_WIN32_WCE` 未定义 | CeGCC 用 `__MINGW32CE__` | `#if defined(__MINGW32CE__) \|\| defined(_WIN32_WCE)` |
| `DefWindowProc` 链接 A 版 | 无 UNICODE 默认 | 显式 `*W` 后缀 |
| `GetProcAddress` 同上 | 同上 | `GetProcAddressW` |
| `CREATE_RUNNING` 不存在 | WinCE 无此宏 | 传 `0` |
| `CreateThread` 栈太小默认 64KB | CE 特有 | `STACK_SIZE_PARAM_IS_A_RESERVATION` + 256KB |

### 4.4 Carlinkit 协议要点
- 包格式：16 字节头（magic/length/type/typecheck）+ payload
- magic：明文 `0x55AA55AA`，加密 `0x55BB55BB`（AES-128-**CFB128**，非 CBC）
- typecheck = ~type（完整性校验）
- **typecheck 检查在 Message::parse 里，如果盒子实际不发 typecheck 需要放宽**
- **加密已实现**：`AESCipher::init_seed(seed)` 按 FastCarPlay 方案派生 key/IV
  - base key = `"SkBRDy3gmrw1ieH0"`，`key[i] = base[(seed+i)%16]`
  - IV 全零除 `IV[1]=seed&0xFF, IV[4]=(seed>>8)&0xFF, IV[9]=(seed>>16)&0xFF, IV[12]=(seed>>24)&0xFF`
  - CFB128 流模式，任意长度无需填充，每条消息独立（fresh IV = fresh EVP context）
  - `Connection::sendInit(true)` 先发 `CMD_ENCRYPTION(seed)`（明文），后续 File/Init/String 加密，Control/Heartbeat 明文
- **USB 端点**（GET_DESCRIPTOR 实测）：interface 0 class=0xff，**EP 0x81 bulk IN mps=512**，**EP 0x02 bulk OUT mps=512**（早期代码错用 0x01，已修正）

### 4.5 patched AutoKit（临时可用方案）
- 0x421FC 处 UTF-16LE 版本串改为 `2018.06.20-2021`（与盒子配套）
- 补丁后版本警告仍显示但 **CarPlay 实际可用**
- 备份在 `patch-backup/AutoKit_patched.exe`（SHA256 前缀 `050D8DB8`）
- 已知缺陷（要修的）：音量不可调 / 音质差 / 分辨率低 / 需插拔两次

### 4.6 🏆 根本原因彻底解决：移除 set_configuration(1)，对齐官方驱动

**根因终结（Python 实时硬件诊断 100% 复现并证实）**：
- **致命元凶：`libusb_set_configuration(1)`**：
  - Windows 系统 PnP 枚举时已为该设备设置好 Configuration 1。
  - 用户态再次调用 `set_configuration(1)`，会向盒子 USB Gadget 硬件下发标准控制请求，触发 Gadget 控制器复位端点 FIFO，直接导致 Bulk OUT (`0x02`) 硬件级挂起（进入无限 NAK 状态，所有后续写入全报 `rc=-7` 超时）。
  - **实验对照**：Python 诊断工具在不调用 `set_configuration(1)` 时，并发后台读取线程下写入 44 字节报文 **`rc=0, tx=44` 瞬间成功**；一旦调用 `set_configuration(1)`，下一次写入立即 `rc=-7, tx=0` 超时挂起。
- **官方驱动印证**：
  - 逆向 AutoKit 原装驱动 `libBoxUSB.dll`，其 `host_init` 流程仅有两步：`open` + `claim_interface(0)`，**从未调用过 `set_configuration`、`alt_setting` 或 `clear_halt`**。
- **协议模式**：
  - 2018.06.20 固件仅支持明文协议；`pc_main` 默认已改为明文模式。

### 4.7 🏆 终极协议解密：AutoKit 汇编逆向证实「两阶段独立传输」机制

**根因完全破解（通过对原装 `AutoKit.exe` 反汇编精准定位 `host_send` 调用链）**：
- **致命陷阱：单次合并写入导致 Gadget 端点 FIFO 截断假死**：
  - 之前我们在 commit `68ff45d` 将 16 字节头部与载荷合并为单次 `bulk_transfer` 发送（例如 44 字节、51 字节）。
  - **AutoKit 逆向反汇编事实**（`AutoKit.exe` 0x22864~0x22894 与 0x234d4~0x23530）：
    1. **第 1 次调用 `host_send(header, 16)`**：先单独下发 16 字节头部；
    2. **第 2 次调用 `host_send(payload, len)`**：若有载荷，再单独下发载荷（以最大 512 字节分片）。
  - **FastCarPlay 源码印证**（`connection.cpp:435~439`）：也是先发 16 字节头部，再单独发载荷！
  - **物理原理**：盒子 Linux Gadget 端驱动是以 USB URB 为单位读取。用户态守护进程先发起 `read(16)` 读取头部。若主机将 44 字节合并在一个 URB 内，Gadget 控制器只读出 16 字节，剩下的 28 字节被截断或遗留在端点硬件 FIFO 中，直接导致 Bulk OUT (`0x02`) 硬件假死进入死循环 NAK（`rc=-7`）。
- **清理无用控制传输**：
  - 移除了 `claim()` 内部用于调试的 `control_transfer(GET_DESCRIPTOR)`，与官方驱动行为完全一致。

**当前状态（commit `91a5807`）**：
- `Connection::send` 已改为先写 16 字节头部、后写载荷的两阶段独立传输。
- CI 自动化构建与 36 项单元测试 100% 通过（Ubuntu + Windows 双绿）。
- 最新二进制已部署至 `dist_final\build\pc_main.exe`。

**下一步验证操作**：
1. **Power cycle 盒子**：将 Carlinkit 盒子从电脑 USB 拔出，等待 5 秒重新插回（清除上次单次写入导致的 FIFO 阻塞；iPhone 保持连接并解锁）。
2. **运行最终客户端**：`& "C:\tang-carplay\dist_final\build\pc_main.exe"` 一次性送达两阶段配置突发并保持会话。

## 五、代码结构

```
tang-carplay/
├── client/                          ← Phase B 主项目
│   ├── include/
│   │   └── protocol_const.h         常量（在 namespace carlinkit 内）
│   ├── cpp/
│   │   ├── protocol/
│   │   │   ├── message.h/.cpp       Message 类（序列化/解析/加解密路径）
│   │   │   ├── aes_cipher.h         AES-128-CBC + CFB128（零依赖，NIST 验证）
│   │   │   ├── aes_sbox_data.cpp    S-box 表（**程序生成**，勿手编）
│   │   │   ├── ce_compat.h          CE/PC 兼容层（Mutex/LockGuard）
│   │   │   ├── ce_atoll_patch.h     atoll 声明注入（-include 用）
│   │   │   ├── connection.h/.cpp    Connection 状态机 + sendInit(encrypted) + 消息分发
│   │   │   └── usb_transport.h/.cpp USB 抽象（CE=UKW / PC=libusb 动态，OUT ep=0x02）
│   │   ├── ce_main.cpp              CE GUI 入口（零写卡，窗体+状态+事件）
│   │   ├── ce_main.h                CE/PC 条件编译 shim
│   │   ├── ce_stubs.cpp             CE 符号 stub（atoll）
│   │   └── pc_main.cpp              PC 控制台工具（连盒子验证协议）
│   ├── test/
│   │   ├── fake_transport.h         内存 transport（测试用）
│   │   └── test_protocol.cpp        36 个断言（7 组测试，含 CFB128 KAT）
│   ├── build_ce.sh                  CeGCC 编译脚本（CI 用）
│   └── README.md                    构建说明
├── probe/                           ← 零写卡系统探针（已完成，上车验证过）
├── usbwatch/                        ← USB 设备监测（已完成）
├── usbtest/                         ← USB 栈验证（已完成，v0.6）
├── bench/                           ← 性能基准（已完成）
├── docs/
│   ├── ROADMAP.md                   项目路线图（含 Phase B 全阶段）
│   ├── HARDWARE.md                  硬件档案
│   ├── CARLINKIT_PROTOCOL.md        协议文档
│   ├── ONCAR.md / ONETRIP.md       上车测试手册
│   └── HANDOVER.md                  ← 本文件
├── third_party/（.gitignore 排除）  参考源码
│   ├── FastCarPlay/                 协议参考（C++，最重要）
│   ├── carlive/                     CE 渲染参考 + CeGCC 经验
│   ├── CEUSBKWrapper/               USB 驱动源码（LGPL）
│   ├── ludwig-cplay2air/            CPlay2Air 固件逆向
│   └── AutoKit_WinCE/               AutoKit 发行包（DLL 来源）
├── patch-backup/
│   ├── AutoKit_original.exe         未修改 2019 版
│   └── AutoKit_patched.exe          版本补丁版（当前在车上用的）
└── .github/workflows/
    ├── build.yml                    CeGCC ARM 编译（4 工具 + carlinkit_ce.exe）
    └── pc-unit-tests.yml            双平台编译 + 单元测试 + pc_main
```

## 六、新环境搭建

### 6.1 最小环境（仅跑 pc_main 测试协议）
```bash
git clone https://github.com/yushengxin1975/tang-carplay.git
cd tang-carplay

# Linux
sudo apt install g++ libusb-1.0-0
g++ -std=c++17 -O2 -Wall -Iclient/include -Iclient/cpp \
    client/cpp/protocol/{aes_sbox_data,connection,usb_transport}.cpp \
    client/cpp/pc_main.cpp -o pc_main
./pc_main              # 默认加密模式（2024+ 固件可能要求）
./pc_main --plain      # 强制明文模式（旧固件）
./pc_main --enc        # 显式加密模式

# Windows（需 MinGW g++ + libusb-1.0.dll）
g++ -std=c++17 -O2 -Wall -static -Iclient/include -Iclient/cpp \
    client/cpp/protocol/{aes_sbox_data,connection,usb_transport}.cpp \
    client/cpp/pc_main.cpp -o pc_main.exe
# libusb-1.0.dll 放同目录（libusb.info 下载 MinGW 64-bit 版）
./pc_main.exe          # 盒子插 USB 口运行（加密默认）
./pc_main.exe --plain  # 强制明文模式
./pc_main.exe --no-alt # 禁用 alt-setting+clear-halt（调试用）
```

### 6.2 完整环境（含 CE 编译）
- CeGCC 容器：`docker pull ghcr.io/enlyze/windows-ce-build-environment-arm`
- 或直接用 GitHub Actions（push 即编译）

### 6.3 运行单元测试
```bash
g++ -std=c++17 -O2 -Wall -Iclient/include -Iclient/cpp \
    client/cpp/protocol/{aes_sbox_data,connection,usb_transport}.cpp \
    client/test/test_protocol.cpp -o test && ./test
# 期望：=== 36 passed, 0 failed ===
```

## 七、下一步工作（按优先级排序）

### M1.5a — 解决盒子不回数据（当前阻塞，最高优先级）

**问题**：plain 模式写入已成功（0 错误），但盒子 IN 端点无任何数据。加密模式还没在 power cycle 后作为第一次运行测试（详见 §4.6）。

**推荐路径**（按成本排序）：
1. **Power cycle + 加密模式首跑**（最快）：拔盒子 USB → 等 10s → 插回 → 立刻跑 `pc_main.exe`（加密默认，不加 --plain）。如果写入成功且收到数据 → Phase B.1 完成。
2. **抓包对比**：在 Windows 装 [USBPcap](https://usbpcap.codeplex.com/)，抓 AutoKit 连盒子的前 20 个 USB 事务。AutoKit 能连成功，看它发了什么我们没发的。
3. **Linux 对照**：树莓派上跑 FastCarPlay 原版连同一盒子，`sudo modprobe usbmon` + `wireshark` 抓包。

### M1.5b — pc_main 真机协议验证（解决超时后）
```bash
./pc_main              # 默认加密
# 预期输出：
#   opening box (0x1314:0x1520)...
#   claimed. starting protocol...
#   protocol up. plug iPhone into the box.
# 插 iPhone 到盒子 → 观察 PLUGGED / VIDEO / AUDIO 消息
# sample_video.bin / sample_audio.bin 会落盘前 2MB 供 ffmpeg 离线分析
```
**可能遇到的问题**：
- Linux 权限：`sudo ./pc_main` 或写 udev 规则
- Windows 需要 libusb-1.0.dll（libusb.info 下载 MinGW 64-bit 版）
- 如果收到数据但 typecheck 不匹配：放宽 Message::parse 的 typecheck 检查
- 如果加密握手失败：对比 FastCarPlay `aes_cipher.cpp` 的 seed 派生，确认 base key 和 IV 字节序

### M2 — CE 上车集成
1. 部署包：TF 卡 `gps_byd/` 下放 `carlinkit_ce.exe`（改名 startup.exe）+ `ceusbkwrapper.dll` + `USBKIT.DLL`
2. 前提：usbtest v0.6 先跑一次（注册 UKW 驱动）
3. 验证：按导航键 → carlinkit_ce 窗口 → 盒子+iPhone → 观察 VIDEO/AUDIO 计数

### M3 — 媒体层
- 视频：`OMX_CORE.DLL` + `VDEC_DCH60.dll`（AutoKit 包内有，直接复用）
- 音频：`libfdk-aac-0.dll`（同上）+ waveOut
- 渲染：DirectDraw（`ddraw.dll` 在 `\Windows` 已确认存在）
- 触控：TouchPanel API → CMD_TOUCH/CMD_MULTI_TOUCH

### M4 — 打磨
- 音量控制（修 AutoKit 缺陷 #1）
- 分辨率提升（协议层请求更高分辨率）
- 连接稳定性（修插拔两次问题）

## 八、风险与法律

1. 软件 MFi 身份：盒子内置合法 MFi（我们只跟盒子通信，不直接跟 iPhone iAP2）→ 风险低
2. Apple AccessorySDK 泄露源码（third_party/AccessorySDK/）：仅参考协议结构，**不要复制代码**
3. CEUSBKWrapper LGPL：动态链接 OK，静态需开源
4. 本项目仅自用，勿分发

## 九、关键联系人/资源

- GitHub: yushengxin1975（gh CLI 已认证）
- CI: 两个 workflow 均配置好，push 即触发
- Carlinkit 盒子购买信息：2018-07-23，型号 CPC200（有线版），固件 2018.06.20
- 汽车：比亚迪唐 2015 旗舰版（已恢复出厂，AutoKit 可用）

## 十、Git 快速参考

```bash
git clone https://github.com/yushengxin1975/tang-carplay.git
git log --oneline -20          # 最近提交
git branch -a                  # 只有 master
gh run list                    # CI 历史
gh run download <id> --name tang-carplay-ce-client --dir dist  # 下载 CE 产物
gh run download <id> --name pc-windows-latest-<id> --dir dist  # 下载 PC 产物
```

## 十一、本次会话提交记录（2026-10-10 ~ 10-11）

| commit | 说明 |
|---|---|
| `090e28c` | USB transport 幂等 open/claim |
| `af8ec74` | readPump 超时(-7)/溢出(-8)继续泵送 |
| `fae90d6` | stdout 无缓冲（日志不丢） |
| `41b7dc5` | 握手：Message 工厂 + sendInit 配置突发 |
| `f23e7ae` | send(const Message&) 重载 + String 接 c_str |
| `bf17cd9` | 诊断：端点 dump + 写状态日志 |
| `f2075bc` | **OUT 端点 0x01→0x02 修正**（rc -5→-7） |
| `2451f93` | 加 libusb_reset_device（后移除） |
| `2d66c43` | **CFB128 加密协议实现**（FastCarPlay 兼容） |
| `d585b81` | 修 CFB128 解密反馈（IV←密文非明文） |
| `8ecb45c` | 移除 reset（Windows 上致 OUT 端点 NAK） |
| `cf2ffe6` | **alt-setting+clear-halt 默认开**；写超时 1000→3000ms |
| `bc479b4` | **静态链接 pc_main.exe**（消除 MinGW DLL 依赖问题） |

**CI 状态**：两个 workflow 全绿，`36 passed / 0 failed`（ubuntu + windows 双平台）。
**单元测试覆盖**：AES-CBC NIST KAT、AES-CFB128 NIST SP800-38A KAT、CFB seed roundtrip、Connection 级加密收发、Message 序列化/解析、Connection 生命周期。

**真机测试重大突破（2026-10-12）：**
- **冷启动上电顺序验证（用户关键洞察）**：CPC200-CMAP（2018.06.20 固件）为冷启动枚举架构，必须在**盒子接入 USB 之前插好 iPhone**；若盒子先上电，OTG 端口无法动态触发中断枚举。
- **AutoKit 协议对齐（逆向 0x235ec / 0x66ba4）**：
  1. `CMD_OPEN`（28B）字 4 为 `32`（0x20），字 5 为 `1`，字 6 为 `2`（非 FastCarPlay 的 49152/2/2）；
  2. 启动紧接发送 `Control(13)` 与 `CMD_9(1)` 激活手机发现；
  3. 供电模式发送 `/tmp/charge_mode = 2`（1.5A 强充）；
  4. 移除 FastCarPlay 引入的多余 Wi-Fi/BT/HiCar 冲突配置。
- **端点解挂与瞬态自愈**：
  - 处理 `rc=-9`（PIPE STALL）自动 `clear_halt(0x81)`，处理 `rc=-1` 瞬态 I/O 重试，彻底解决插拔时客户端闪退问题。
- **实测成果（M1.5b 达成）**：
  - 盒子秒级上报 `[MSG PLUGGED]`（4 字节 `03 00 00 00`，状态 `ONLINE`）；
  - iPhone 15 顺利拉起 CarPlay 会话；
  - 持续稳定拉取 2000+ 帧 H.264 视频流（~30fps，12+ MB），`sample_video.bin` 成功落盘并验证包含标准 SPS（`0x27`）、PPS（`0x28`）、IDR 关键帧（`0x25`）！