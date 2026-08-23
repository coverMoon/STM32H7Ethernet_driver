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
- 替代：D016

原倾向为专用 DMA 区域 + 明确 MPU 属性；已由 STM32H743 实际 SRAM3 / MPU / linker 方案替代。

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

CubeMX/ST 生成内容的基本原则继续保留：`.ioc`、Core、CMSIS、HAL、FreeRTOS generated files、`cmake/stm32cubemx/CMakeLists.txt` 属于生成侧；Core 手工代码只进 USER CODE 区域。旧的根目录 `Drivers/Ethernet/**` 手工维护路径已被 Package 化替代。

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

运行时机制、callback、weak symbol 与 ownership 原理说明独立放在 `docs/ETHERNET_RUNTIME_FLOW.md`，README 只提供入口链接，不复制整篇原理说明。

---

## D016 STM32H743 Ethernet DMA 内存与 MPU

- 状态：Accepted
- 日期：2026-08-21

当前 STM32H743 参考板：

```text
RAM_ETH = SRAM3 = 0x30040000 / 32 KiB
RAM_D2  = 0x30000000 / 256 KiB
RX Desc = 0x30040000
TX Desc = 0x30040080
ETH_RX_DESC_CNT = 4
ETH_TX_DESC_CNT = 4
```

HAL Descriptor 24 B，4 个实际 96 B，每组预留 128 B。MPU：SRAM3 Normal Non-cacheable；前 256 B Device overlay。当前 I/D Cache Disabled。板级代码在 MX_ETH_Init 前使能 D2 SRAM3 时钟。

---

## D017 板级 linker 与自动化策略

- 状态：Accepted
- 日期：2026-08-21

板级 linker 显式管理 Ethernet DMA 内存和 section，不使用 regex/string patch 自动修改 linker。自动化优先用于 map/ELF、alignment、越界、section 非空等验证。

第二阶段仓库整理后，当前参考 linker 位于：

```text
examples/STM32H743_LAN8720_FreeRTOS/STM32H743xx_FLASH.ld
```

---

## D018 Ethernet Memory Management Tool 边界

- 状态：Accepted
- 日期：2026-08-21

不使用 CubeMX Memory Management Tool 自动管理 Ethernet DMA linker section。`.ioc` 保存 Descriptor 地址与 MPU；Example linker 保存物理 DMA SRAM/section；Port 处理板级 SRAM 准备。

---

## D019 第一版 Payload Buffer 与 ownership

- 状态：Accepted
- 日期：2026-08-21
- TX ownership 子项替代：D025

```text
RX Buffer Count = 4
TX Buffer Count = 4
Buffer Size     = 1536 B
Alignment       = 32 B
```

Driver input section：`.eth_dma_buffer.rx` / `.eth_dma_buffer.tx`。当前参考板：

```text
RX Pool = 0x30042000 / 0x1800
TX Pool = 0x30044000 / 0x1800
```

RX：DMA Buffer → HAL Link callback → copy CPU frame → 立即归还 pool。

本决定建立时 TX 仍为 polling：caller → copy TX DMA Buffer → `HAL_ETH_Transmit(timeout)` → HAL_OK 后归还。该 TX polling ownership 已由 D025 的 async TX completion ownership 替代；Buffer Count / Size / Alignment、RX ownership 与 section 约束继续有效。

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

本决定建立时入口为 `EthernetRtos_RxTask()`，RX 路径为：

```text
HAL_ETH_RxCpltCallback()
→ Driver generic RX event
→ CMSIS-RTOS2 Adapter
→ Thread Flag
→ Runtime Task
→ drain EthernetDriver_Receive()
```

完整 Frame 在任务上下文通过同步 Handler 交付；Frame pointer 只在 Handler 调用期间有效。`0x88B5` 测试逻辑属于 Example。

D024 将 Task 名称和职责扩展为 RX/TX 共用的 `EthernetRtos_RuntimeTask()`，但不改变本决定的 RX Frame 交付边界。

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

其中 Core、ST Drivers、FreeRTOS、`.ioc`、linker、CMake、build/flash 脚本都属于 Reference Example，不是 Driver 固定依赖。

`docs/stm32h7_ethernet_project_docs/00_PROJECT.md` ～ `08_HANDOFF.md` 路径暂时保持稳定，避免破坏项目状态读取约定。原 `docs/BOARD_PORTING.md` 的用户迁移职责合并到根 README，删除重复维护源。

---

## D023 CubeMX Ethernet RX Task 生成方式

- 状态：Superseded
- 日期：2026-08-22
- 替代：D024

当时 Reference Example 使用：

```text
Task Entry : EthernetRtos_RxTask
Generation : As weak
```

CubeMX 6.18.1 已确认：CubeMX 继续生成 Task attributes、priority、stack、allocation 和 `osThreadNew()`；generated `freertos.c` 生成 weak stub；Package 提供同名强定义。

该方案已经完成 Generate Code、Debug/Release Build、map/ELF 检查和 On-board async RX 1000 / 1000 回归。

后续由于同一任务同时承担 TX completion reclaim，D024 将 Task 名称和 Entry 更新为 `EthernetRuntime / EthernetRtos_RuntimeTask`，继续沿用 `As weak + Package strong implementation` 的构建边界。

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

CubeMX 负责：

```text
Task object
priority
stack
allocation
osThreadNew()
weak Task stub
```

Package 负责：

```text
EthernetRtos_RuntimeTask() strong implementation
runtime task handle
RX complete event registration
TX complete event registration
RX/TX Thread Flag wait
RX drain
TX completion reclaim
```

Runtime Task 启动时自动注册：

```text
EthernetDriver_SetRxEventHandler(EthernetRtos_OnRxEvent)
EthernetDriver_SetTxEventHandler(EthernetRtos_OnTxEvent)
```

`EthernetRtos_IsReady()` 只有在两类 ISR event 都完成绑定后才为 true。

RX/TX 共用一个 Runtime Task；同一次唤醒同时含 RX/TX flag 时，当前先处理 TX completion，再 drain RX。该方案已完成 CubeMX Generate Code、Build、async TX 上板测试以及 RuntimeTask 改名后的 async RX 1000 / 1000 回归。

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

- `EthernetDriver_TransmitAsync()` 返回 `ETHERNET_TX_QUEUED` 后 caller 原始 Frame 可立即复用；
- 当前不向 Application 暴露 per-frame TX completion callback；completion 主要用于 Driver 内部 Buffer recycle；
- `ETHERNET_TX_RETRY` 表示临时没有 TX Buffer / Descriptor 等可用资源，Driver 不隐藏软件 TX Queue；
- `tx_config.pData` 保存 Driver TX DMA Buffer 地址，HAL 通过 `PacketAddress[]` 在 `HAL_ETH_ReleaseTxPacket()` 时把该地址传给 `HAL_ETH_TxFreeCallback()`；
- TX complete ISR 只做 event forwarding，不在 ISR 中 reclaim Descriptor / Buffer；
- `EthernetDriver_TransmitAsync()` 在正式 submit 前执行一次 completion reclaim backstop；
- `HAL_ETH_Transmit_IT()` 与 `HAL_ETH_ReleaseTxPacket()` 及 TX Pool acquire/release 用短 PRIMASK critical section 序列化，避免并发修改 HAL/Driver TX bookkeeping；Frame memcpy 不放在整个关中断区。

该方案已完成连续 1000-frame async TX 上板测试，并在同一 RuntimeTask 结构下重新完成 async RX 1000 / 1000 回归。
