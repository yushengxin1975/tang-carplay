# Phase B 启动检查清单

> 路径 B：为 BYD 唐 2015 重写 Carlinkit 客户端，摆脱 AutoKit 闭源依赖

## 当前阶段：B.0 — 准备阶段（PC 上，1 周）

### 立即可做的事

#### 1. 拉取 FastCarPlay 源码分析协议

```bash
cd third_party
git clone --depth 1 https://github.com/niellun/FastCarPlay.git
ls FastCarPlay/src/
```

关键文件：
- `BoxCarConnectionPackets/` 协议包定义
- `BoxCarConnectionState.cpp` 状态机
- `MediaOutput/MediaOutput.cpp` 媒体流处理

#### 2. 拉取 catplay（CPlay2Air 固件 Rust 实现）

```bash
cd third_party
git clone --depth 1 https://github.com/catplay-labs/catplay.git
```

作为 Rust 实现的对比参考（不同语言但同样协议）。

#### 3. 拉取 ludwig-v 反无线研究（文档化协议）

```bash
cd third_party
git clone --depth 1 https://github.com/ludwig-v/wireless-carplay-dongle-reverse-engineering.git
```

重点读 `docs/` 和协议分析文档（无线版，但 CPC200 有线版协议底层相同）。

### Phase B.0 产出物

`docs/CARLINKIT_PROTOCOL.md`（约 3000-5000 字）：

1. **协议总览**：盒子角色（iAP2/MFi 内部 PC）、消息层、流控制
2. **连接握手**：attach → 配置 → 流开启
3. **包格式**：
   - 头结构（magic, length, type, sequence）
   - 消息类型（discovery、media stream、touch input、audio）
4. **流协商**：分辨率 / 编码 / 音频格式握手
5. **USB 传输**：bulk vs control，端点地址
7. **PC client 端对照表**（参考 FastCarPlay 实现细节）

### 完成 Phase B.0 的判定标准

- [ ] 三份源码克隆到 third_party/
- [ ] docs/CARLINKIT_PROTOCOL.md 写完（用户审阅）
- [ ] Phase B.1 计划文档（client/ 目录结构 + 编译链 + 关键组件设计）

## 关联文档

- `docs/ROADMAP.md` — 完整项目路线图（路径 B 全阶段）
- `docs/HARDWARE.md` — 硬件档案（必须熟读）

## 立即动手的事

1. 拉 FastCarPlay 源码并通读 protocol parser（~30 分钟浏览）
3. 起草 CARLINKIT_PROTOCOL.md 框架
4. 列出 Phase B.1 的关键不确定点