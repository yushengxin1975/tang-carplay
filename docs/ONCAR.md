# Phase 0 上车测试清单

## 准备

1. U 盘格式化为 **FAT32**（不要 exFAT/NTFS）
2. 拷入 `dist/probe.exe`（放 U 盘根目录）
3. 备用测试件（可选，任选其一，均在 third_party/ 不入库）：
   - `third_party/carlive/wince/tuptup.exe` — 投屏接收端，验证 GUI 程序能否长跑
   - `third_party/Ford-WINCE-Autokit/TC/cecmd.exe` — Total Commander CE，浏览车机文件系统

## 执行（车机侧）

唐 2015 运行第三方 EXE 的入口（按优先级尝试）：

1. **导航路径法**：设置 → 导航路径，指向 `\存储卡路径\probe.exe`（U 盘挂载名以 probe 探到的为准，常见 `\Storage Card` / `\SDMEM`），然后点导航图标启动
2. 若原车导航为地图卡方式：参考汽车之家"唐中控系统刷机经验谈"帖的 U 盘部署方式
3. 亿连安装帖验证过的路径：U 盘直装后被系统扫描（该机装过亿连 WinCE 版说明入口存在）

## 收集结果

- 弹窗出现 = EXE 能跑（第一关通过）
- 取回 U 盘里的 **probe_log.txt**，关注三行结论：
  - `OS VERDICT` — 期望 CE 6.0
  - `CPU VERDICT` — 期望 ARM，记下 procType/level
  - `USB VERDICT` — USB 相关驱动加载数量（**0 = Phase 1 需先解决 USB host 驱动**）
- 另外记下：屏幕分辨率、MEM 可用量、进程列表里的原车 launcher 名

## 判定表

| 结果 | 动作 |
|---|---|
| 全部符合预期 | 进 Phase 1：WinCE USB 客户端驱动 + usbmux 帧 |
| CE 5.0 | API 差异评估（usb client 模型不同），大概率仍可做 |
| 非 ARM | 项目终止 |
| USB VERDICT = 0 | 插着 U 盘重跑一次；仍为 0 则拆机确认 USB 口走线，Phase 1 预算 +2 周 |
| EXE 无法启动 | 换 tuptup.exe / TC 交叉验证；都不行 → 研究比亚迪对 EXE 启动的限制 |

## 已知风险

- 车机若只允许白名单 EXE（导航程序校验），需先破解启动器（参考唐刷机包帖）
- probe.log 的进程列表会暴露原车系统结构，是后续常驻集成的关键输入
