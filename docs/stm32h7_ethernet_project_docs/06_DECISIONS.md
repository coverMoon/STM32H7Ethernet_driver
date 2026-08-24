# Design Decisions

本文件只记录会跨模块影响后续工作的设计决定。

状态：`Accepted` 后续默认遵守；`Proposed` 尚待验证冻结；`Superseded` 已由后续决定替代；`Rejected` 不采用。改变 Accepted 决定时新增决定并标记替代关系，不直接删除历史。

---

## D001 第一验证平台

- 状态：Accepted
- 日期：2026-08-20

第一验证平台：STM32H743VIT6 + LAN8720AI + RMII。Driver 总体目标仍是 STM32H7 可复用组件。

---

## D002 软件运行环境

- 状态：Accepted
- 日期：2026-08-20

采用 STM32 HAL + FreeRTOS + LwIP；Ethernet RX/TX 按 FreeRTOS 异步场景设计。

---

## D003 基础软件分层

- 状态：Accepted
- 日期：2026-08-20

```text
Application
→ LwIP
→ ethernetif / RTOS Adapter
→ Ethernet Driver
→ PHY Driver
→ Ethernet Port
→ STM32 HAL / Hardware
```

Ethernet Driver 不处理 IP/UDP/TCP 或机器人业务。

---

## D004 底层语言边界

- 状态：Accepted
- 日期：2026-08-20

BSP/Port、PHY、MAC/DMA、ethernetif 和 LwIP 接口优先使用 C；上层应用可使用 C/C++。

---

## D005 第一版网络能力范围

- 状态：Accepted
- 日期：2026-08-20

第一版基础验证：Static IPv4、ICMP Ping、UDP Echo、TCP Echo。暂不加入 DHCP、DNS、mDNS、TLS、HTTP。

---

## D006 第一版不主动追求 Zero Copy

- 状态：Accepted
- 日期：2026-08-20

优先稳定、可验证的数据路径。允许 ethernetif / DMA Buffer / LwIP pbuf 间复制；正确性、压力和性能测量完成前不主动引入 Zero Copy。

---

## D007 DMA / Cache 第一版方案

- 状态：Superseded
- 日期：2026-08-20
- 替代：D016，后续再由 D026 更新 Reference Example 基线

原倾向为专用 DMA 区域 + 明确 MPU 属性；后续由 STM32H743 实际 SRAM3 / MPU / linker 方案替代。

---

## D008 PHY Link 检测

- 状态：Accepted
- 日期：2026-08-20

采用周期轮询 PHY Link，暂不依赖 LAN8720 nINT。Bring-up 的 200 ms 周期不冻结为长期参数。

---

## D009 LwIP 应用 API

- 状态：Proposed
- 日期：2026-08-20

Socket / Netconn / Raw API 尚未冻结，在 LwIP Runtime 设计时决定。

---

## D010 多对话项目状态管理

- 状态：Accepted
- 日期：2026-08-20

远程仓库当前 main 是项目状态源。跨对话重要信息进入代码、DECISIONS、STATUS、HANDOFF 和专题文档；每个对话只推进一个工作单元。

---

## D011 FreeRTOS 与 HAL 时间基线

- 状态：Accepted
- 日期：2026-08-20

```text
TIM6    → HAL 1 ms Tick / HAL timeout
SysTick → FreeRTOS Kernel Tick
```

ETH ISR 调用 RTOS API 前必须核对实际 FreeRTOSConfig 与 NVIC priority。本决定不冻结最终 ETH IRQ / Task priority。

---

## D012 基础调试输出

- 状态：Accepted
- 日期：2026-08-20

当前参考板使用 USART1 PA9 TX / PA10 RX，115200 8N1；`printf` 仅用于低频 Bring-up/诊断，禁止进入 ETH IRQ 和高速数据路径。

---

## D013 CubeMX 生成代码与手工代码边界

- 状态：Superseded
- 日期：2026-08-20
- 替代：D020、D022

CubeMX/ST 生成内容的基本原则继续保留：`.ioc`、Core、CMSIS、HAL、FreeRTOS generated files、`cmake/stm32cubemx/CMakeLists.txt` 属于生成侧；Core 手工代码只进 USER CODE 区域。通用 Driver 目录与板级绑定由 D020/D022 定义。

---

## D014 PHY Driver 与 RTOS 边界

- 状态：Accepted
- 日期：2026-08-20

LAN8720 PHY Driver 与 FreeRTOS/LwIP 解耦，只通过 MDIO Wrapper 提供 Ready、Auto-negotiation、Status 等非阻塞接口；等待和 timeout 由调用层负责。

---

## D015 文档受众与阶段信息边界

- 状态：Accepted
- 日期：2026-08-21

根 README 面向 Driver 使用者，不作为开发日志；项目控制文档可包含 M0/M1/M2、工作单元、Accepted/Proposed 和测试状态。Static Review、Build Verified、On-board Verified、Measured 必须区分。

运行时机制、callback、weak symbol 与 ownership 原理说明独立放在 `docs/ETHERNET_RUNTIME_FLOW.md`。README 可以保留对用户有直接价值的 Reference Example 性能基线，但不复制完整诊断过程。

---

## D016 STM32H743 Ethernet DMA 内存与 MPU（旧基线）

- 状态：Superseded
- 日期：2026-08-21
- 替代：D026

历史基线为 RX4/TX4、TX Descriptor `0x30040080`、RX/TX Pool 各 4×1536 B、I/D Cache Disabled。该配置已被后续 RX8 + I-Cache Enabled 的 Reference Example 基线替代。

---

## D017 板级 linker 与自动化策略

- 状态：Accepted
- 日期：2026-08-21

板级 linker 显式管理 Ethernet DMA 内存和 section，不使用 regex/string patch 自动修改 linker。自动化优先用于 map/ELF、alignment、越界、section 非空等验证。

当前参考 linker：

```text
examples/STM32H743_LAN8720_FreeRTOS/STM32H743xx_FLASH.ld
```

---

## D018 Ethernet Memory Management Tool 边界

- 状态：Accepted
- 日期：2026-08-21

不使用 CubeMX Memory Management Tool 自动管理 Ethernet DMA linker section。`.ioc` 保存 Descriptor 地址、count、MPU/Cache 配置；Example linker 保存物理 DMA SRAM/section；Port 处理板级 SRAM 准备。

---

## D019 第一版 Payload Buffer 与 ownership（旧内存基线）

- 状态：Superseded
- 日期：2026-08-21
- TX ownership 子项替代：D025
- Buffer Count / layout / RX 基线替代：D026

历史配置为 RX4/TX4、RX Pool `0x30042000 / 0x1800`、TX Pool `0x30044000 / 0x1800`。RX 采用 DMA Buffer → HAL Link callback → copy CPU frame → 立即归还 pool；该 copy-first 语义继续保留，但当前 count / 地址由 D026 更新。TX polling ownership 已由 D025 的 async TX completion ownership 替代。

---

## D020 Driver Package 与 Port 边界

- 状态：Accepted
- 日期：2026-08-22
- 替代：D013 中 Ethernet 通用源码目录与板级绑定部分

仓库产品是根目录 `Ethernet/**`。通用 Driver 不直接 include Demo `eth.h`，也不依赖全局 Handle 名 `heth`。

Port API：

```text
EthernetPort_GetHandle()
EthernetPort_PrepareDmaMemory()
EthernetPort_PhyResetAssert()
EthernetPort_PhyResetRelease()
```

HAL Handle、PHY Reset、DMA SRAM clock 属于目标工程；物理 DMA 地址、MPU、linker 不进入通用 Driver。

---

## D021 RTOS Adapter 与 RX Frame 交付边界

- 状态：Accepted
- 日期：2026-08-22
- Task 命名/双向 Runtime 扩展：D024

Driver Core 与 FreeRTOS/CMSIS-RTOS2 解耦。可选 `Ethernet/RTOS/CMSIS_RTOS2` Adapter 不创建 Task；Application/CubeMX 决定 Task priority、stack、allocation。

RX 路径：

```text
HAL_ETH_RxCpltCallback()
→ Driver generic RX event
→ CMSIS-RTOS2 Adapter
→ Thread Flag
→ Runtime Task
→ drain EthernetDriver_Receive()
```

完整 Frame 在任务上下文通过同步 Handler 交付；Frame pointer 只在 Handler 调用期间有效。`0x88B5` 测试逻辑属于 Example。

---

## D022 仓库产品与 Reference Example 目录边界

- 状态：Accepted
- 日期：2026-08-22

仓库根目录只突出 Driver Package 与用户入口：

```text
Ethernet/   ← 可复制产品
README.md   ← Driver Integration Guide
examples/   ← 完整参考工程
docs/       ← 技术记录与项目控制文档
```

STM32H743VIT6 + LAN8720AI + FreeRTOS 完整 CubeMX 工程统一位于：

```text
examples/STM32H743_LAN8720_FreeRTOS/
```

Core、ST Drivers、FreeRTOS、`.ioc`、linker、CMake、build/flash 脚本都属于 Reference Example，不是 Driver 固定依赖。

---

## D023 CubeMX Ethernet RX Task 生成方式

- 状态：Superseded
- 日期：2026-08-22
- 替代：D024

历史方案使用 `EthernetRtos_RxTask / As weak`。后续由于同一任务承担 TX completion reclaim，D024 将 Task 名称和 Entry 更新为 `EthernetRuntime / EthernetRtos_RuntimeTask`，继续沿用 `As weak + Package strong implementation` 的构建边界。

---

## D024 Ethernet Runtime Task 与 RX/TX 事件边界

- 状态：Accepted
- 日期：2026-08-23
- 替代：D023；扩展 D021 的 Task 命名与职责

当前 Reference Example 使用：

```text
Task Name  : EthernetRuntime
Task Entry : EthernetRtos_RuntimeTask
Generation : As weak
```

CubeMX 负责 Task object、priority、stack、allocation、`osThreadNew()` 和 weak Task stub；Package 负责强定义 Runtime Task、RX/TX event registration、Thread Flag wait、RX drain 和 TX completion reclaim。

同一次唤醒同时含 RX/TX flag 时，当前先处理 TX completion，再 drain RX。该方案已完成 CubeMX Generate Code、Build、async TX 上板测试以及 async RX 回归。

---

## D025 第一版 Async TX completion ownership

- 状态：Accepted
- 日期：2026-08-23
- 替代：D019 中 TX polling ownership 子项

第一版 TX 使用 copy-based async ownership：

```text
Caller Frame
→ copy Driver static TX DMA Buffer
→ HAL_ETH_Transmit_IT()
→ HAL / DMA ownership
→ TX complete IRQ
→ HAL_ETH_TxCpltCallback()
→ Driver generic TX event
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ Driver TX Pool
```

规则：

- 返回 `ETHERNET_TX_QUEUED` 后 caller 原始 Frame 可立即复用；
- 当前不向 Application 暴露 per-frame TX completion callback；
- `ETHERNET_TX_RETRY` 表示临时没有 TX Buffer / Descriptor 等资源，Driver 不隐藏软件 TX Queue；
- TX complete ISR 只做 event forwarding，不在 ISR 中 reclaim Descriptor / Buffer；
- submit 前执行一次 completion reclaim backstop；
- HAL TX submit/reclaim 与 TX Pool ownership 用短 PRIMASK critical section 序列化，Frame memcpy 不放在整个关中断区。

该方案已完成连续 1000-frame async TX 上板测试和 RX 回归。该测试只证明 ownership/completion recycle，不代表 TX throughput 性能基线。

---

## D026 STM32H743 Reference Example DMA / Cache 基线 v2

- 状态：Accepted
- 日期：2026-08-24
- 替代：D016；D019 中 Buffer Count / layout / RX 内存基线

当前 Reference Example 冻结为：

```text
RAM_ETH = SRAM3 = 0x30040000 / 32 KiB
RAM_D2  = 0x30000000 / 256 KiB

ETH_RX_DESC_CNT = 8
ETH_TX_DESC_CNT = 4
RX Desc = 0x30040000 / 8 × 24 B / linker slot 0x100
TX Desc = 0x30040100 / 4 × 24 B / linker slot 0x100

RX Pool = 0x30042000 / 0x3000 / 8 × 1536 B
TX Pool = 0x30045000 / 0x1800 / 4 × 1536 B
Alignment = 32 B
```

MPU / Cache：

```text
SRAM3 whole region : Normal Non-cacheable / Shareable
0x30040000~0x300401FF : 512 B Device overlay for descriptors
I-Cache : Enabled
D-Cache : Disabled
```

RX ownership 继续使用 copy-first：DMA Buffer → Driver CPU frame → RTOS Adapter Frame。TX ownership 继续遵守 D025。

I-Cache Enabled 已完成单变量和 clean stress 实测：60 B RX clean saturation 从历史约 `71.1 kpps` 提升到约 `130.6 kpps`；1514 B @ 8000 pps、200000 帧零丢包，估算 on-wire 约 `98.43 Mbit/s`。因此 I-Cache Enabled 是当前 Reference Example 的正式基线配置。

D-Cache-on 仍未验证；不得把当前 Non-cacheable SRAM3 方案等同于未来 Cacheable DMA Buffer 方案。
