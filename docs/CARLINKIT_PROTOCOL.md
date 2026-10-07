# Carlinkit CPC2000-CP2A 协议参考

> 适用于 BYD 唐 2015 直连 CarPlay 重写项目（路径 B）的协议层文档
> 来源：niellun/FastCarPlay（Linux C++ 完整实现）+ catplay-labs/catplay（Rust CPlay2Air 协议）+ ludwig-v 反向文档 + AutoKit_WinCE/BIN 字符串逆向

## 1. 协议定位

```
Dell Broadclyt CPC2000(公糍CPC200-CP2A) 厂商 0x1314 产品 0x1520
  │
  │ (iAP2 / MFi 认证) — iPhone Lightning 输入
  ▼
 iAP2 session + H.264 + AAC mux
  │
  │ (USB 2.0 High-Speed bulk 传输 — Carlinkit 私有 PC↔Box 协议)
  ▼
 PC (我们 = BYD 唐 WinCE 6.0 ARM 客户端)
```

**Carlinkit 盒子是 iAP2/MFi 内置转接器**，对 PC 端暴露 **私有 RPC 协议**（非标准，非 iAP2）。我们在 PC 端需要：
1. **找盒子**（USB Host 枚举 VID 0x1314/PID 0x1520）
2. **打开 bulk IN/OUT endpoint**
3. **协议握手**（CMD_OPEN + CMD_VERSION + CMD_ENCRYPTION 协商）
4. **接收 H.264 视频 + AAC 音频帧**
5. **发送触摸 + 控制消息**
6. **心跳保活**（CMD_HEARTBEAT）

## 2. 物理传输

**USB endpoints**（参考 USB-to-USB CarPlay Dongle Reverse Engineering）：
- **Bulk IN**：Box → PC，承载 Video/Audio/Data
- **Bulk OUT**：PC → Box，承载 Touch/Control
- **Interrupt IN**（可选）：Box → PC 心跳 / 紧急事件
- **Control endpoint**（端点 0）：用于厂商特定请求

**BYD 唐 2015 实际**：通过 CEUSBKWrapper 的 `UsbDeviceList` + bulk transfer API 实现（LGPL/开源）。

## 3. 包格式（每条消息结构）

```
偏移  大小    字段           说明
0     4 字节  magic          0x55aa55aa = 明文 / 0x55bb55bb = AES 加密
4     4 字节  length         payload 字节数（可为负？观察 FastCarPlay 用 int32_t 解读）
8     4 字节  type           消息类型枚举
12    4 字节  typecheck       ~type（按位取反，用于完整性校验）

[可选 payload：length 字节，紧接 16 字节头后]
```

源码（`FastCarPlay/src/protocol/message.h:16-24`）：
```c
#pragma pack(push, 1)
struct Header {
    uint32_t magic;       // MAGIC 或 MAGIC_ENC
    int32_t  length;       // payload 字节数
    uint32_t type;         // 命令类型
    uint32_t typecheck;    // ~type
};
#pragma pack(pop)
```

## 4. 消息类型枚举（PC ↔ Box）

| 方向 | 命令 | 值 | 含义 |
|---|---|---|---|
| PC→Box | `CMD_OPEN` | 1 | PC 启动会话（携带 MAGIC） |
| Box→PC | `CMD_PLUGGED` | 2 | Box 通知 PC：iPhone 已插入 |
| Box→PC | `CMD_STATE` | 3 | Box 状态查询应答（IP、设备状态） |
| Box→PC | `CMD_UNPLUGGED` | 4 | Box 通知 PC：iPhone 拔开 |
| **PC→Box** | `CMD_TOUCH` | 5 | **PC 发送触摸事件给 Box → iPhone** |
| Box→PC | `CMD_VIDEO_DATA` | 6 | **H.264 视频帧（去封装后）** |
| Box→PC | `CMD_AUDIO_DATA` | 7 | **AAC 音频帧** |
| PC→Box | `CMD_CONTROL` | 8 | Box 控制命令（按键/Siri 等） |
| Box→PC | `CMD_APP_INFO` | 10 | App 信息（iPhone 应用状态） |
| Box→PC | `CMD_BLUETOOTH_INFO` | 13 | Box Bluetooth 配对信息 |
| Box→PC | `CMD_WIFI_INFO` | 14 | Box WiFi 状态 |
| Box→PC | `CMD_DEVICE_LIST` | 18 | **媒体设备列表（协商分辨率/编解码）** |
| PC→Box | `CMD_MULTI_TOUCH` | 23 | 多指触控 |
| PC→Box | `CMD_JSON_CONTROL` | 25 | JSON 格式控制命令 |
| Box→PC | `CMD_MANUFACTURER` | 20 | 制造商信息 |
| Box→PC | `CMD_MEDIA_INFO` | 42 | 媒体元数据（标题/专辑） |
| PC→Box | `CMD_SEND_FILE` | 153 | 发送小文件 |
| Box→PC | `CMD_DAYNIGHT` | 162 | 昼夜模式（UI 主题） |
| 双向 | `CMD_HEARTBEAT` | 170 | 心跳保活 |
| 双向 | `CMD_VERSION` | 204 | 协议版本协商 |
| 双向 | `CMD_ENCRYPTION` | 240 | **AES 密钥交换**（首次连接） |

## 8. 加锁

FastCarPlay 启用 AES-CBC 加密（`ENCRYPTION_BASE` 常量 + 动态密钥）。
握手流程（推测，参考 FastCarPlay 启动）：

1. PC 发送 `CMD_OPEN`
2. Box 应答（`CMD_OPEN` reply 或 `CMD_VERSION`）
3. PC 发送 `CMD_ENCRYPTION` + 加密参数
4. Box 应答（接受加密方式）
5. 后续包 `magic = MAGIC_ENC`，payload 由 AES 加密

**CE 端实现**：FastCarPlay 的 AESCipher 类（`protocol/aes_cipher.cpp`）用 mbedtls 或自实现——可直接复用到我们 CE 客户端。

## 9. 字节序 / 编码

小端（little-endian）。所有 u32/i32 字段在包中按 LE 存储。
字符串字段使用 UTF-8。

## 10. 状态机（PC 端）

```
           CMD_OPEN send ───────────────► cmdEnum(OPEN)
                                              │ reply
                                              ▼
              CMD_HEARTBEAT ◄─────────── 协议就绪 (STATUS=6)
                                              │
              CMD_DEVICE_LIST 使连线            │ iPhone attach
                                              │ CMD_PLUGGED
                                              ▼
                DATA 传输 ◄────────────────── STATUS_CONNECTED
                  │  │                          │
                  │  └── 心跳 / 状态           │
                  └────── 触摸 / 控制         │
                                              │
                  iPhone detach                │
                                              ▼
                CMD_UNPLUGGED ◄────────── 回到状态 6
```

PC 端代码模型（伪 C++）：
```cpp
class Connection {
    libusb_context_handle ctx_;
    libusb_device_handle boxDev_;
    libusb_transfer_pool xfers_;  // IN 异步接收
    AES cipher_;
    STATE send_state;
    State message_state;

    void start() {
        ctx_.init();
        device_.findByVidPid(0x1314, 0x1520);  // VID/PID 识别 Carlinkit
        openEndpoints();                       // 0x81 IN / 0x01 OUT
        submitTransfers(IN, VIDEO_WRITE_QUEUE_SIZE);
        send(CMD_OPEN);                         // 启动
        send(CMD_VERSION);                      // 协商
        send(CMD_ENCRYPTION);                   // 密钥交换
        state = WAIT_OPEN_REPLY;
    }

    void onMessage(Header h, byte[] data) {
        switch (h.type) {
            case CMD_OPEN:        /* box acked open */ break;
            case CMD_VERSION:     /* verify versions */ break;
            case CMD_ENCRYPTION:  /* set up cipher */ break;
            case CMD_HEARTBEAT:   reply(); break;
            case CMD_PLUGGED:     phoneConnected = true; break;
            case CMD_UNPLUGGED:   phoneConnected = false; break;
            case CMD_VIDEO_DATA:  enqueue(videoStream, data); break;
            case CMD_AUDIO_DATA:  enqueue(audioStream, data); break;
            case CMD_TOUCH:       /* ignore (box doesn't send touch) */ break;
            case CMD_CONTROL:     /* process button event */ break;
            case CMD_JSON_CONTROL: /* process JSON command */ break;
            case CMD_DEVICE_LIST:  /* parse negotiated media formats */ break;
        }
    }
};
```

**CE 端实现**：用 CEUSBKWrapper 的 `UsbDeviceList` + bulk transfer（替代 libusb）+ AESCipher 直接复刻。

## 11. 媒体协商（CMD_DEVICE_LIST 18）

FastCarPlay 启动时跟 Box 敲定：
- **视频**：H.264 等级（baseline/main/high）、分辨率（最常用 1024×768 / 1280×720）、码率上限、帧率
- **音频**：AAC-LC 采样率（44.1kHz/48kHz）、声道数、码率

CPC2000 盒子默认支持：
- H.264 baseline + main，720p@60 / 1080p@30
- AAC-LC 16-bit 48kHz 立体声

我们 CE 端只需声明解码能力（`OMXCore_h264_decode`），Box 自动匹配。

## 12. 触控 (CMD_TOUCH 5) / (CMD_MULTI_TOUCH 23)

FastCarPlay 接收 Win32 touch 事件转换为 Carlinkit 格式：

```cpp
struct TouchEntry {
    uint8_t  state;     // 1=down, 2=up, 0=move
    uint16_t x, y;      // 屏幕坐标 (1080p 参考)
    uint8_t  fingers;   // 多指 ID
};
```

CE 端把 `TouchPanelGetDeviceInfo` + `TouchPanelGetMode` + `TouchPanelReadCalibrationPoint` 收集的点击事件转换为这个格式发送。

## 13. 控制 (CMD_CONTROL 8) - Box→PC，按键事件

```c
#define BTN_SIRI              5
#define BTN_MICROPHONE        7
#define BTN_SCREEN_REFRESH    12
#define BTN_LEFT               100
#define BTN_RIGHT              101
#define BTN_SELECT_DOWN        104
#define BTN_SELECT_UP          105
#define BTN_BACK               106
#define BTN_DOWN               114
#define BTN_HOME               200
#define BTN_PLAY               201
#define BTN_PAUSE              202
#define BTN_NEXT_TRACK         204
#define BTN_PREVIOUS_TRACK     205
```

Box→PC 格式：1 字节按钮 ID。我们 CE 端收到后用 `keybd_event` 或 `PostMessage` 模拟 CarPlay 内应用按键，或者直接转发给上层的 UI 处理。

## 14. 控制 (CMD_JSON_CONTROL 25) - PC→Box，JSON 控制

PC→Box，JSON 字符串发送命令给 Box（如切换主题/打开应用）。Word 节奏可能包含 SDL Script。

CE 端实现：构建 JSON 字符串，用 cpprest 或者 hand-built JSON encoder。

## 15. 心跳 (CMD_HEARTBEAT 170)

双向。PC 定期（默认 1-3 秒）发送，Box 回同样的或不同状态。心跳避免 Box 在 5 秒后 USB 自动挂起。

CE 端：CreateThread 后台，每秒一次 `SendHeartbeat()`。

## 16. 直接关键观察

1. **协议 20 个命令就涵盖了 Carlinkit 完整功能**——**不需要逆向 AutoKit 全部二进制**
3. **代码设计参考优先级**：
   1. FastCarPlay `protocol/` 目录（C++ 实现）
   3. catplay `async/catplay_async/`（Rust 实现，看 event_reconciler.rs）
4. **CE 端使用 CEUSBKWrapper（LGPL）替换 libusb**——可以近乎直接照抄 FastCarPlay 的 `Connection::start/onMessage` 流程

## 17. 文件路径

参考实现：
- `third_party/FastCarPlay/src/protocol/` - 协议实现（C++）
- `third_party/FastCarPlay/src/connection.cpp` - 状态机
- `third_party/catplay/async/catplay_async/src/` - Rust 异步处理
- `third_party/ludwig-cplay2air/.../Custom_Firmware/Scripts/` - 盒子固件脚本

## 18. 下一步 — Phase B.1 设计要点

`client/cpp/protocol/` 主文件：
- `message.h/cpp` - 包类（带 AES 加密）
- `connection.h/cpp` - 连接/状态机
- `aes_cipher.h/cpp` - 加密
- `usb_transport.h/cpp` - CEUSBKWrapper 包装（替代 FastCarPlay 的 LibUsb）

测试：
- PC 测试（libusbdotnet 模拟 box）
- CE 测试 (实际)