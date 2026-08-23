# Project Status

- 更新时间：2026-08-23
- 当前阶段：M2 — MAC / DMA Runtime
- 当前状态：copy-based async RX / async TX、统一 `EthernetRtos_RuntimeTask()`、Driver Package 与 Reference Example 均已完成基础 Build / Map / On-board 回归。D024 / D025 已 Accepted。

## 1. 已确认完成

### 硬件 / PHY

- [x] STM32H743VIT6 + LAN8720AI + RMII；
- [x] PHY Reset / MDIO / ID / Address / Strap；
- [x] Auto-negotiation、Link、100M Full Duplex；
- [x] USART1 调试输出。

### DMA / Memory

- [x] RAM_ETH = SRAM3 `0x30040000 / 32 KiB`；
- [x] RX Desc `0x30040000`；
- [x] TX Desc `0x30040080`；
- [x] RX Pool `0x30042000 / 0x1800 / 4×1536 B`；
- [x] TX Pool `0x30044000 / 0x1800 / 4×1536 B`；
- [x] MPU Non-cacheable + Descriptor Device overlay；
- [x] linker ASSERT / map 验证；
- [x] copy-based RX recycle；
- [x] copy-based async TX completion recycle。

### MAC / DMA Runtime

- [x] PHY Speed / Duplex → MAC；
- [x] `HAL_ETH_Start_IT()`；
- [x] Raw TX / RX；
- [x] polling RX 1000 / 1000；
- [x] ETH IRQ；
- [x] Driver generic RX event；
- [x] Driver generic TX event；
- [x] CMSIS-RTOS2 RX/TX Thread Flag；
- [x] `EthernetRtos_RuntimeTask()`；
- [x] RX drain 到 `ETHERNET_RX_NONE`；
- [x] async RX 1000 / 1000；
- [x] `HAL_ETH_Transmit_IT()` async submit；
- [x] task-side `HAL_ETH_ReleaseTxPacket()`；
- [x] `HAL_ETH_TxFreeCallback()` TX Buffer recycle；
- [x] async TX 1000-frame test；
- [x] RuntimeTask 改名 / TX completion 加入后的 async RX 1000 / 1000 回归。

### Driver Package / Reference Example

```text
Ethernet/                                  ← Driver Package
examples/STM32H743_LAN8720_FreeRTOS/       ← Reference Example
README.md                                  ← Integration Guide
docs/ETHERNET_RUNTIME_FLOW.md              ← RX/TX Runtime 原理说明
```

- [x] Driver Core 不直接依赖 Demo `eth.h` / `heth`；
- [x] Port API 建立；
- [x] CMSIS-RTOS2 Adapter 建立；
- [x] Adapter 不创建 Task；
- [x] Frame Handler 保持任务上下文；
- [x] `0x88B5` 测试逻辑留在 Example；
- [x] 完整 Demo 位于 `examples/`；
- [x] Example CMake 通过 `../../Ethernet` 引用 Package；
- [x] Debug / Release Build 回归；
- [x] map / ELF 地址回归；
- [x] async RX / TX 上板回归。

### CubeMX Runtime Task 边界

当前已采用并验证：

```text
Task Name  : EthernetRuntime
Task Entry : EthernetRtos_RuntimeTask
Generation : As weak
```

- [x] CubeMX 6.18.1 Generate Code；
- [x] generated `freertos.c` 产生 weak Task Entry；
- [x] CubeMX 继续管理 Task attributes / `osThreadNew()`；
- [x] Package 同名强定义链接无冲突；
- [x] Runtime Task 同时处理 RX deferred processing 与 TX completion reclaim；
- [x] Build / On-board RX/TX 回归通过；
- [x] D024 Accepted。

## 2. 当前接口

Port：

```text
EthernetPort_GetHandle()
EthernetPort_PrepareDmaMemory()
EthernetPort_PhyResetAssert()
EthernetPort_PhyResetRelease()
```

Driver / PHY：

```text
EthernetDriver_Init()
EthernetDriver_SetRxEventHandler()
EthernetDriver_SetTxEventHandler()
EthernetDriver_ConfigureLink()
EthernetDriver_Start()
EthernetDriver_TransmitAsync()
EthernetDriver_ProcessTxCompletions()
EthernetDriver_Receive()
EthernetMdio_Read()/Write()
Lan8720_IsReady()
Lan8720_RestartAutoNegotiation()
Lan8720_GetStatus()
```

CMSIS-RTOS2：

```text
EthernetRtos_SetRxFrameHandler()
EthernetRtos_IsReady()
EthernetRtos_RuntimeTask()
```

其中：

```text
EthernetDriver_SetRxEventHandler()
EthernetDriver_SetTxEventHandler()
→ Driver → RTOS Adapter 内部绑定
→ 由 RuntimeTask 启动时自动完成

EthernetRtos_SetRxFrameHandler()
→ RTOS Adapter → Application / ethernetif

EthernetDriver_TransmitAsync()
→ Application / ethernetif → Driver TX
```

## 3. 当前 RX Runtime

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
→ Frame Handler
```

当前 RX 1000 / 1000 回归通过。

## 4. 当前 TX Runtime

```text
Caller Frame
→ EthernetDriver_TransmitAsync()
→ copy Driver TX DMA Buffer
→ HAL_ETH_Transmit_IT()
→ DMA
→ TX IRQ
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

Driver 当前没有软件 TX Queue；临时资源不足返回 `ETHERNET_TX_RETRY`。TX submit 与 reclaim 用短 critical section 序列化。

## 5. Runtime 原理文档

`docs/ETHERNET_RUNTIME_FLOW.md` 已扩展为 RX/TX 双向说明，覆盖：

- CubeMX weak Task Entry 与 Package strong implementation；
- HAL 固定 callback；
- RX/TX Driver event Handler；
- Thread Flag 的事件位语义；
- RX drain 与两次 copy；
- TX async submit / backpressure；
- HAL `pData → PacketAddress[] → TxFreeCallback()`；
- task-side TX completion reclaim；
- TX critical section；
- RX/TX 共用 Runtime Task 的理由和处理顺序。

## 6. 当前测试等级

已确认：

- Static Review：PASS；
- Debug Build：PASS；
- Release Build：PASS；
- map / ELF DMA layout：PASS；
- On-board PHY / MAC startup：PASS；
- On-board async RX 1000 / 1000：PASS；
- On-board async TX 1000-frame completion ownership：PASS；
- RuntimeTask 改名后的 RX regression：PASS；
- CubeMX `As weak` / Package strong implementation：PASS。

未完成：Measured 高负载性能、长时间 Stress、D-Cache-on。

## 7. M2 尚未完成

- [ ] RX/TX error / drop 统计；
- [ ] DMA fatal / RBU / timeout recovery；
- [ ] Link Down / Up 完整 MAC lifecycle；
- [ ] Task stack high-water mark；
- [ ] D-Cache 场景；
- [ ] 长时间 / 高负载。

## 8. 下一工作单元建议

推荐下一步只做 **RX/TX 可观测性与基础统计**：

```text
TX queued / retry / completion
RX frame / error / drop
HAL / DMA error snapshot
```

目标是先让后续 Link lifecycle、error recovery、LwIP 和 Stress Test 有稳定可读的统计依据；不要在同一工作单元同时实现完整 recovery 或进入 LwIP。
