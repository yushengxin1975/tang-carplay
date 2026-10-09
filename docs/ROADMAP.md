# 唐 2015 WinCE 车机 Carlinkit 客户端（路径 B）

为比亚迪唐 2015 旗舰版（MT3360/WinCE 6.0）打造**自主 Carlinkit 客户端**。取代当前依赖的 AutoKit WinCE 客户端。

## 项目状态（2026-10-08 更新）

| 轨道 | 状态 | 备注 |
|---|---|---|
| 软硬件档案（MT3360/ARM1176/Mali/OMX） | ✅ 完成 | docs/HARDWARE.md |
| patched AutoKit 临时可用版本 | ✅ 验证可用 | 仅作过渡，卡上有副本 |
| 直连 iPhone 路线 | ❌ 失败 | BYD OEM 限制 USB 客户端驱动注册路径 |
| **Carlinkit 客户端重写（本路线）** | 🔄 B.1 进行中 | 本文件 |

## Phase B.1 进度明细

- [x] 协议层骨架（message.h / protocol_const.h / connection / usb_transport 抽象）
- [x] AES-128-CBC：**程序生成 sbox（NIST 验证）+ FIPS-197 标准结构重写**
- [x] 单元测试 23 项 **双平台（ubuntu+windows CI）全过**，含 **NIST FIPS-197 C.1 KAT**
- [x] CI 修通：`.github/workflows/pc-unit-tests.yml`（matrix, apt/choco g++），每次 push 自动回归
- [x] UsbTransport_CE 实现（Ukw* 动态绑定，复用 usbtest v0.6 车上验证过的模式）
- [x] **CE 编译闭环**：carlinkit_ce.exe (ARM 80KB) 在 CI 产出，PC 测试未破（23/23）
- [x] UsbTransport_PC stub（占位，后续接 libusb-1.0）
- [ ] CE 端编译（CeGCC 编译 usb_transport.cpp 的 _WIN32_WCE 分支——需要扩展 tools/build_all.sh）
- [ ] UsbTransport_PC 真实实现（libusb-1.0，PC 连盒子调试用）
- [ ] 媒体层（OMX H.264 / fdk-aac / DirectDraw / WaveOut / TouchPanel）
- [ ] CE GUI 入口（startup.exe 兼容，零写卡）
- [ ] 上车集成测试（M2）

### CI 修通过程抓出的真实 bug（测试的价值实证）

1. 手打 sbox 表错行（26 个值/重复行）→ 改为程序生成
2. decrypt 逆 ShiftRows 链式赋值逻辑错误 + t 未声明
3. encrypt 的 MixColumns 根本不是 GF(2^8) 矩阵乘法（自审未发现）→ FIPS-197 重写
4. unique_ptr delete 栈对象 → double free（CE 上将是随机崩溃）
5. const 成员函数锁非 mutable mutex → 编译失败
6. include 路径不一致 → 统一为 -Iclient/include + -Iclient/cpp 相对式

## 已知 AutoKit 缺陷（要修掉的）

| 缺陷 | 性质 | 新路线对策 |
|---|---|---|
| 音量无法调节 | AutoKit 软件 bug | 重写后完全控制音量映射到 BYD mixer |
| 音质差 | USB 音频 vs 原车蓝牙 | 优化 fdk-aac 编码参数 / 考虑转回蓝牙 |
| 分辨率低 | AutoKit 协议层只取低分辨率 | 直接控制 stream 通过 ICMP/握手取最高分辨率（1080p@60） |
| 插拔两次 Carlinkit 才连上 | AutoKit USB 握手 bug | 重写连接状态机 |

## 架构

```
iPhone → [Lightning/C] → Carlinkit 盒子 (iAP2 + MFi 内部)
                                    │
                                    │  私有协议 over USB bulk/control
                                    ▼
                            Windows CE 6.0 ARM (BYD 唐)
                                    │
        ┌───────────────────────────┼─────────────────────────────┐
        ▼                           ▼                            ▼
  CEUSBKWrapper            FastCarPlay 协议移植              OMX 硬解管线
  (USB kernel driver)        (Box protocol parser)            (MT3360 VDEC)
                                    │                              │
                                    ▼                              ▼
                              视频 H.264 + 音频 AAC      Mali-200 合成
                              decode/sync                (DirectDraw →)
                                    │
                                    ▼
                              WaveOut + BYD mixer
```

**关键技术决策**：

1. **iAP2 / MFi 由 Carlinkit 盒子内部处理**——我们不直接跟 iPhone iAP2 握手。盒子和 iPhone 之间的私有协议是 Carlinkit 的黑盒，**但盒子对外接口协议（PC ↔ Box）已被开源项目（FastCarPlay）逆向**。
3. **视频硬解直接用 AutoKit 包内的 OMX 硬解 DLL**（开源 OMX IL 标准的 MT33xx 优化实现），避开 ffmpeg 软解 ARMv6 性能瓶颈。
5. **音频用 fdk-aac-0.dll**（AutoKit 包内）+ Windows WaveAPI 输出。
7. **触屏回传**：BYD 屏幕有 Touch panel 驱动 → 我们的程序捕获 → 编码为 Carlinkit 触屏包发送给盒子。

## 阶段计划

### Phase B.0 — 准备阶段（PC 上，1 周）
- 读 FastCarPlay 源码，把 Carlinkit 协议处理层（C++）分析清楚
- 读 catplay-labs/catplay 源码（CPlay2Air 协议 Rust 实现）作为对比
- 读 ludwig-v/wireless-carplay-dongle-reverse-engineering（文档化协议）
- **产出文档**：`docs/CARLINKIT_PROTOCOL.md`（协议分层、数据包格式、消息类型）

### Phase B.1 — 用户态重新编译 CarLinkit 客户端到 CE6 ARM（PC 开发 + 模拟测试，3-4 周）
- 新项目目录 `client/`：
    - `cpp/` C++ 源码（核心：协议 parser、packet builder）
    - `ce_main.c` C 入口（用 CeGCC 编译）
    - `win_main.cpp` Windows 入口（PC 调试用）
    - `Makefile.cegcc` + `Makefile.host`
- 复用 ENLYZE/CEUSBKWrapper 的 CEUSBBOX/ceusbkwrapper（LGPL OK）
- 复用 AutoKit 的 OMX_CORE.DLL + VDEC_DCH60.dll + libfdk-aac-0.dll + avcodec-56.dll（运行时 DLL，放进 libx/ 目录）
- **验证**：PC 上跑 win_main，用 libusbdotnet 模拟 box 协议做端到端测试

### Phase B.2 — 车上集成（v0.1，2 周）
- 编译 CE 版本，部署到卡
- 通过 startup.exe 启动（替换 AutoKit）
- **验证**：连接 Carlinkit 盒子 + iPhone，看视频流和音频是否能跑
- **第一次成功**：我们有了自主流媒体客户端

### Phase B.3 — 优化与修复 AutoKit 4 大问题（每关 1-2 周）
- **3.1** 音量控制：写自定义 BYD mixer hook（or AudioRouting API）+ 把 BYD 方向盘音量消息 → Wave API
- **3.2** 画质提升：协议层设最高 H.264 profile / resolution，使用 OMX 解码到更高分辨率后扫描再 MediaSource
- **3.3** 连接稳定性：实现完整状态机（attach → 配置 → 流的稳定握手），处理 box reset / iPhone trust / iOS 重启

### Phase B.4 — UI + 集成（1-2 周）
- 触屏捕获（Windows CE TouchPanel API）→ 发送给盒子
- 音量/亮度等控件（BYD CAN 消息 or KBD 驱动 hook）
- 中文输入支持（如需）
- 与 BYD 原车 UI 集成（最小侵入：全屏覆盖）

## 项目仓库结构（计划）

```
tang-carplay/
├── client/                        ← 主项目（新建）
│   ├── cpp/
│   │   ├── protocol/             ← Carlinkit 协议 parser
│   │   │   ├── discovery.{cpp,h}
│   │   │   ├── media_stream.{cpp,h}
│   │   │   ├── packet.{cpp,h}
│   │   │   └── touch_input.{cpp,h}
│   │   ├── decode/
│   │   │   ├── omx_h264.{cpp,h}  ← OMX IL 包装
│   │   │   ├── aac_decode.{cpp,h}
│   │   │   └── directdraw_render.{cpp,h}
│   │   ├── audio/
│   │   │   ├── waveout_sink.{cpp,h}
│   │   │   └── volume_control.{cpp,h}
│   │   └── ce_main.c             ← Windows CE 入口
│   ├── Makefile.cegcc            ← CeGCC ARM 交叉编译
│   ├── Makefile.host             ← PC 调试
│   └── libx/                     ← 运行时依赖（OMX/AAC DLLs）
├── docs/
│   ├── HARDWARE.md               ← 已完成
│   ├── ONCAR.md
│   ├── ONETRIP.md
│   ├── ROADMAP.md                ← 本文件
│   └── CARLINKIT_PROTOCOL.md     ← Phase B.0 产出
├── probe/                         ← 已完成的零写卡工具（保留）
├── usbtest/                       ← Phase 1 工具（保留，已知 BYD OEM 限制）
├── usbwatch/                      ← 已完成
├── bench/                         ← 已完成
├── third_party/                   ← 参考源码（不入库）
│   ├── FastCarPlay/              ← Phase B.0 参考
│   ├── catplay/                  ← Phase B.0 参考
│   ├── carlive/                  ← Phase B.0/1 渲染参考
│   ├── CEUSBKWrapper/            ← Phase B.1 USB 栈
│   ├── AutoKit_WinCE/            ← BIN 资源（OMX/AAC DLLs 来源）
│   └── xcertplay/                 ← 参考（Android 上成功做 CarPlay）
└── patch-backup/                 ← AutoKit patch 备份
```

## 风险与依赖

| 项 | 风险 | 缓解 |
|---|---|---|
| Carlinkit 盒子协议变 | 中 | 固件提取+逆向参考 FastCarPlay |
| iPhone iAP2 兼容性 | 已自动（Carlinkit 盒子有 MFi） | 不直接碰 iPhone |
| OMX 硬解在 CE | 已验证（AutoKit 用着） | 直接复用 AutoKit 的 OMX DLLs |
| BYD mixer 音量 | 中（BYD 私有） | 试用 WaveAudio + 找 BYD 音量控制 API |
| 触屏 BYD 驱动 | 低（标准 TouchPanel API） | 与 AutoKit 同样的方法 |

## 阶段性交付与里程碑

| 里程碑 | 交付物 | 周期 |
|---|---|---|
| M0: docs/CARLINKIT_PROTOCOL.md | 协议文档 | 1 周 |
| M1: client/ 端到端 PC 测试 | PC 上跑通模拟 box 流 | 4 周 |
| M2: 卡上能运行 patched 客户端 | CE 版本部署 + Carlinkit 连上 + 视频音频输出 | +2 周 |
| M3: AutoKit 4 大问题修复完 | 音量/画质/连接稳定 | +4-6 周 |
| M4: 生产可用 | UI 完整 + 长期测试通过 | +1-2 周 |
| **总计** | | **3 个月** |

## 回退策略

任何阶段如果发现不可行：
- **M2/M3 失败**（CE 版本跑不通）→ 退回 patched AutoKit + 用项目档案做 CarPlay 知识库
- **M1 失败**（FastCarPlay 移植到 CE6 卡住）→ 评估是否需要更强的工具链（vcperf？llvm？）

## 当前活跃工作

**等待用户指示**开始 Phase B.0（PC 上读 FastCarPlay 源码）。