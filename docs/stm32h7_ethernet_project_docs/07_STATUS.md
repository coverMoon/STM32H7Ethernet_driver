# Project Status

- 更新时间：2026-08-24
- 当前阶段：M2 — MAC / DMA Runtime
- 当前状态：RX 性能瓶颈定位已完成并收口；Reference Example 已启用 I-Cache，清理全部临时 profiler，并完成 60 B 小包与 1514 B 大包 RX clean baseline。TX async ownership 已验证，但 TX throughput 尚未建立 Measured 基线。

## 1. 已确认完成

### 硬件 / PHY

- [x] STM32H743VIT6 + LAN8720AI + RMII；
- [x] PHY Reset / MDIO / ID / Address / Strap；
- [x] Auto-negotiation、Link、100M Full Duplex；
- [x] USART1 调试输出。

### DMA / Memory / Cache

当前 Reference Example：

```text
RAM_ETH = SRAM3 = 0x30040000 / 32 KiB
RAM_D2  = 0x30000000 / 256 KiB

RX Desc = 0x30040000 / 8 × 24 B
TX Desc = 0x30040100 / 4 × 24 B
RX Pool = 0x30042000 / 0x3000 / 8 × 1536 B
TX Pool = 0x30045000 / 0x1800 / 4 × 1536 B

MPU     = SRAM3 Normal Non-cacheable + first 512 B Device overlay
I-Cache = Enabled
D-Cache = Disabled
```

- [x] linker / map / ELF layout；
- [x] RX copy-based ownership / recycle；
- [x] TX copy-based async ownership / completion recycle；
- [x] I-Cache Enabled 上板验证；
- [ ] D-Cache-on。

### MAC / DMA Runtime

- [x] PHY Speed / Duplex → MAC；
- [x] `HAL_ETH_Start_IT()`；
- [x] Raw TX / RX；
- [x] polling RX 1000 / 1000；
- [x] ETH IRQ；
- [x] Driver generic RX/TX event；
- [x] CMSIS-RTOS2 RX/TX Thread Flag；
- [x] `EthernetRtos_RuntimeTask()`；
- [x] RX drain 到 `ETHERNET_RX_NONE`；
- [x] async RX 1000 / 1000；
- [x] `HAL_ETH_Transmit_IT()` async submit；
- [x] task-side `HAL_ETH_ReleaseTxPacket()`；
- [x] `HAL_ETH_TxFreeCallback()` TX Buffer recycle；
- [x] async TX 1000-frame test；
- [x] `EthernetDriver_GetStats()` 与 RX/TX/HAL/DMA/MAC 统计快照。

### Driver Package / Reference Example

```text
Ethernet/                                  ← Driver Package
examples/STM32H743_LAN8720_FreeRTOS/       ← Reference Example
README.md                                  ← Integration Guide + RX baseline
docs/ETHERNET_RUNTIME_FLOW.md              ← RX/TX Runtime 原理说明
```

- [x] Driver Core 不直接依赖 Demo `eth.h` / `heth`；
- [x] Port API 建立；
- [x] CMSIS-RTOS2 Adapter 建立；
- [x] Adapter 不创建 Task；
- [x] Frame Handler 保持任务上下文；
- [x] `0x88B5` 测试逻辑留在 Example；
- [x] Example CMake 通过 `../../Ethernet` 引用 Package；
- [x] Debug / Release Build、map / ELF、On-board RX/TX 回归。

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
EthernetDriver_GetStats()
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

当前保持 copy-first 两次 copy，不使用临时 ReceiveView/zero-copy 实验代码。

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

Driver 当前没有软件 TX Queue；临时资源不足返回 `ETHERNET_TX_RETRY`。TX submit 与 reclaim 用短 critical section 序列化。

## 5. RX 性能定位结论

本轮通过 DWT / linker-wrap 仅做临时诊断，最终代码已全部清理 profiler，恢复 clean Runtime。

历史主要结果：

```text
I-Cache Disabled, clean 60 B saturation ≈ 71.1 kpps
I-Cache Enabled, clean 60 B saturation  ≈ 130.6 kpps
```

I-Cache 开启后的 hot-path profiler 曾显示 `HAL_ETH_ReadData()` 与 `HAL_ETH_IRQHandler()` 耗时均显著下降；clean stress 再次独立确认吞吐提升。因此此前异常低的小包 RX 性能主要由 I-Cache Disabled 导致。

诊断实验：

- ReceiveView / one-copy：Rejected，未提升吞吐；
- software RX interrupt batching：Rejected，吞吐下降；
- RBUE-only disable：Rejected，吞吐明显下降；
- I-Cache Enabled：Validated，并成为 D026 当前基线。

## 6. RX clean performance baseline — Measured

测试脚本：`test/send_eth_stress.py`。测试固件基于 clean Runtime、I-Cache Enabled、D-Cache Disabled、RX8/TX4。

### 60 B

| 输入 | 数量 | 接收 | HAL/RBU events |
| --- | ---: | ---: | ---: |
| 80 kpps | 200000 | 200000 | 0 |
| 90 kpps | 200000 | 200000 | 0 |
| 100 kpps | 200000 | 200000 | 0 |
| 110 kpps | 200000 | 200000 | 0 |
| 120 kpps | 200000 | 200000 | 99 |
| 120 kpps | 1000000 | 1000000 | 205 |
| 130 kpps | 200000 | 200000 | 52777 |
| 140 kpps | 200000 | 186539 | 186457 |
| 148.59 kpps | 200000 | 175866 | 175686 |

两组过载测试对应实际接收平台均约 `130.6 kpps`。100BASE-TX 最小帧理论极限约 `148.8 kpps`，当前约为理论 packet-rate 的 `87.8%`。

`120 kpps × 1,000,000`：8.333 s，100% received，205 HAL/RBU events，`err/drop/no_buf = 0`。可作为当前扩展高负载工作点，但不等价于长时间 Stress。

### 1514 B

```text
8000 pps × 200000
Elapsed = 25.000 s
RX      = 200000 / 200000
HAL/DMA = 0
script frame rate = 96.90 Mbit/s
estimated on-wire ≈ 98.43 Mbit/s
```

该结果足以确认 RX 大帧接近 100M 线速。

## 7. TX 性能状态

当前只完成：

```text
async TX 1000-frame submit
TX complete event
HAL_ETH_ReleaseTxPacket()
HAL_ETH_TxFreeCallback()
TX Buffer recycle
```

这是 On-board functional/ownership 验证，不是 throughput benchmark。TX 小包/大包 PPS、线速、retry/backpressure 曲线尚未测量；是否在 M2 收尾前建立 TX 性能基线后续单独决定。

## 8. 当前测试等级

已确认：

- Static Review：PASS；
- Debug Build：PASS；
- Release Build：PASS；
- map / ELF DMA layout：PASS；
- On-board PHY / MAC startup：PASS；
- On-board async RX / TX ownership：PASS；
- Driver stats：PASS；
- I-Cache Enabled：On-board Verified；
- RX high-load / near-line-rate：Measured。

未完成：真正长时间 Stress、D-Cache-on、完整 recovery/lifecycle、TX throughput baseline。

## 9. M2 尚未完成

- [ ] DMA fatal / RBU / timeout recovery；
- [ ] async TX 异常路径完整 recovery；
- [ ] Link Down / Up 完整 MAC lifecycle；
- [ ] Task stack high-water mark；
- [ ] D-Cache 场景；
- [ ] 真正长时间 Stress；
- [ ] TX throughput 性能基线是否作为 M2 退出项，待讨论。

## 10. 下一工作单元候选

优先候选：

1. **Link Down / Up 完整 MAC lifecycle**：验证运行中拔线、重协商、MAC/DMA stop/reconfigure/start、RX/TX ownership 恢复；
2. **TX throughput baseline**：如果决定 M2 同时冻结 TX 性能，则建立 60 B / 1514 B async TX benchmark 与 retry/completion 统计。

不要把两个候选同时塞进一个工作单元。
