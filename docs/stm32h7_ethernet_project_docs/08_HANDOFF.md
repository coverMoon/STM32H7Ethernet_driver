# Latest Handoff

- 来源工作单元：M2 Async TX completion ownership + Ethernet Runtime Task 收敛
- 日期：2026-08-23
- 当前阶段：M2 MAC / DMA
- 当前远程定位：根 `Ethernet/` 为 Driver Package，完整 STM32H743 Demo 位于 `examples/`

## 1. 本工作单元完成状态

本轮已经完成并验证：

```text
EthernetRtos_RxTask
→ 重命名并扩展为 EthernetRtos_RuntimeTask

Runtime Task
→ RX deferred processing
→ TX completion reclaim

TX
→ HAL_ETH_Transmit_IT()
→ TX IRQ
→ task-side HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ Driver TX Buffer recycle
```

当前新增设计决定：

```text
D024 Accepted：Ethernet Runtime Task 与 RX/TX 事件边界
D025 Accepted：第一版 Async TX completion ownership
```

D023 已由 D024 替代；D019 的 TX polling ownership 子项已由 D025 替代。

## 2. 当前 CubeMX Runtime Task

Reference Example：

```text
Task Name  : EthernetRuntime
Entry      : EthernetRtos_RuntimeTask
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
RX/TX event registration
RX/TX Thread Flag wait
RX drain
TX completion reclaim
```

`EthernetRtos_IsReady()` 在 RX/TX 两类 event 都完成绑定后才为 true。

## 3. 当前 RX Runtime 已验证路径

```text
ETH IRQ
→ HAL_ETH_IRQHandler()
→ HAL_ETH_RxCpltCallback()
→ Driver RX event
→ EthernetRtos_OnRxEvent()
→ RX Thread Flag
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_Receive()
→ HAL_ETH_ReadData()
→ RxLink / RxAllocate callback
→ RX Buffer recycle
→ Demo / Application Frame Handler
```

当前已重新完成 async RX 1000 / 1000 回归。

## 4. 当前 TX Runtime 已验证路径

```text
Caller Frame
→ EthernetDriver_TransmitAsync()
→ completion reclaim backstop
→ acquire Driver TX DMA Buffer
→ memcpy
→ HAL_ETH_Transmit_IT()
→ HAL / DMA ownership
→ TX complete IRQ
→ HAL_ETH_TxCpltCallback()
→ Driver TX event
→ EthernetRtos_OnTxEvent()
→ TX Thread Flag
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ TX Buffer recycle
```

当前 async TX 1000-frame test 已通过。

### TX API 语义

```text
ETHERNET_TX_QUEUED
→ 已复制并成功提交，caller Frame 可立即复用

ETHERNET_TX_RETRY
→ 临时无 Buffer / Descriptor 等资源，可稍后重试

ETHERNET_TX_ERROR
→ 参数或 Driver 状态错误
```

当前不提供软件 TX Queue，也不向 Application 暴露 per-frame TX completion callback。

## 5. TX 并发与 ownership 约束

`HAL_ETH_Transmit_IT()` 与 `HAL_ETH_ReleaseTxPacket()` 共同操作 HAL TX Descriptor bookkeeping。

当前 Driver 使用短 PRIMASK critical section 序列化：

```text
TX Pool acquire/release
HAL_ETH_Transmit_IT()
HAL_ETH_ReleaseTxPacket()
```

Frame memcpy 不放在整个关中断区。

提交时 `tx_config.pData = dma_buffer`；HAL 将该地址保存到 `TxDescList.PacketAddress[]`，`HAL_ETH_ReleaseTxPacket()` 确认 Descriptor 完成后调用 `HAL_ETH_TxFreeCallback()`，最终归还 Driver TX Pool。

## 6. 当前 DMA / ownership 基线

```text
RAM_ETH      = 0x30040000 / 32 KiB
RX Desc      = 0x30040000
TX Desc      = 0x30040080
RX Pool      = 0x30042000 / 0x1800 / 4 × 1536 B
TX Pool      = 0x30044000 / 0x1800 / 4 × 1536 B
```

RX：

```text
DMA Buffer
→ HAL_ETH_RxLinkCallback()
→ copy Driver CPU Frame
→ 立即 release RX Buffer
→ EthernetDriver_Receive() 再 copy 给 RTOS Adapter
→ Frame Handler
```

TX：

```text
Caller Frame
→ copy TX DMA Buffer
→ DMA
→ completion
→ HAL_ETH_TxFreeCallback()
→ release TX Buffer
```

当前仍是 copy-first，未进入 Zero Copy。

## 7. 当前接口

Driver：

```text
EthernetDriver_Init()
EthernetDriver_SetRxEventHandler()
EthernetDriver_SetTxEventHandler()
EthernetDriver_ConfigureLink()
EthernetDriver_Start()
EthernetDriver_TransmitAsync()
EthernetDriver_ProcessTxCompletions()
EthernetDriver_Receive()
```

RTOS Adapter：

```text
EthernetRtos_SetRxFrameHandler()
EthernetRtos_IsReady()
EthernetRtos_RuntimeTask()
```

Port / PHY / MDIO 接口未改变。

## 8. Runtime 技术文档

`docs/ETHERNET_RUNTIME_FLOW.md` 已扩展为 RX/TX 双向 Runtime 原理文档，重点解释：

- weak Task 与 Package strong implementation；
- HAL 固定 callback 与项目运行时 Handler 的区别；
- RX/TX Thread Flag；
- RX drain 与 Buffer recycle；
- TX async submit / completion reclaim；
- `pData → PacketAddress[] → TxFreeCallback()`；
- TX critical section；
- 单 Runtime Task 同时处理 RX/TX 的原因。

## 9. 当前测试等级

已确认：

- Static Review：PASS；
- Debug Build：PASS；
- Release Build：PASS；
- map / ELF DMA layout：PASS；
- On-board PHY / MAC startup：PASS；
- On-board async RX 1000 / 1000：PASS；
- On-board async TX 1000-frame completion recycle：PASS；
- RuntimeTask 改名后的 RX 1000 / 1000 regression：PASS；
- CubeMX `EthernetRuntime / EthernetRtos_RuntimeTask / As weak`：PASS。

未完成：Measured 高负载性能、长时间 Stress、D-Cache-on。

## 10. 当前仍未完成

- RX/TX error/drop 统计；
- DMA fatal / RBU / timeout recovery；
- async TX 异常路径完整 recovery；
- 完整 Link Down / Up MAC lifecycle；
- Task stack high-water mark；
- D-Cache-on；
- LwIP / Ping / UDP / TCP；
- 高负载 / 长时间 Stress。

## 11. 下一工作单元建议

优先只做：

> **RX/TX 可观测性与基础统计**

建议最小范围：

```text
RX frame / receive error / drop
TX queued / retry / completion
HAL ETH error / DMA error snapshot
只读 stats API
```

完成标准：能够在不向 ISR 加 `printf` 的前提下，通过任务上下文或调试接口看到基础 RX/TX 计数，为后续 error recovery、Link lifecycle、LwIP 和 Stress Test 提供观测依据。

不要在该工作单元同时进入完整 recovery 或 LwIP。
