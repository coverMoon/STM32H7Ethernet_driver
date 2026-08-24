# Latest Handoff

- 来源工作单元：M2 RX 性能瓶颈定位 + clean baseline 收口
- 日期：2026-08-24
- 当前阶段：M2 MAC / DMA
- 当前远程定位：根 `Ethernet/` 为 Driver Package，完整 STM32H743 Demo 位于 `examples/`
- 本轮测试固件基线：`a6cb50e7bbdfec1ec871139629db9c9ee93e3302`

## 1. 本工作单元完成状态

本轮完成：

```text
RX Descriptor 4 → 8
↓
高负载小包性能诊断
↓
ReceiveView / batching / RBUE-only 单变量实验
↓
DWT + HAL_ETH_ReadData / HAL_ETH_IRQHandler profiling
↓
定位 I-Cache Disabled 为主要性能问题
↓
CubeMX Enable I-Cache
↓
删除全部临时 profiler / linker wrap
↓
clean 60 B / 1514 B stress baseline
```

最终不保留临时 profiling、RBUE mask、RX batching 或 ReceiveView 实验代码。

## 2. 当前设计决定

新增：

```text
D026 Accepted：STM32H743 Reference Example DMA / Cache 基线 v2
```

当前基线：

```text
RAM_ETH = SRAM3 = 0x30040000 / 32 KiB
RX Desc = 0x30040000 / 8 × 24 B
TX Desc = 0x30040100 / 4 × 24 B
RX Pool = 0x30042000 / 0x3000 / 8 × 1536 B
TX Pool = 0x30045000 / 0x1800 / 4 × 1536 B

MPU     = SRAM3 Normal Non-cacheable + first 512 B Device overlay
I-Cache = Enabled
D-Cache = Disabled
```

D016 已由 D026 替代；D019 的 Buffer Count / layout / RX 内存基线由 D026 替代，TX async ownership 继续遵守 D025。

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
→ Demo / Application Frame Handler
```

RX 继续使用 copy-first：

```text
DMA Buffer
→ Driver CPU Frame
→ RTOS Adapter Frame
→ Handler
```

不使用 Zero Copy / ReceiveView。

## 4. 当前 TX Runtime

```text
Caller Frame
→ EthernetDriver_TransmitAsync()
→ completion reclaim backstop
→ acquire Driver TX DMA Buffer
→ memcpy
→ HAL_ETH_Transmit_IT()
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

当前 async TX 1000-frame completion ownership 已上板验证。尚未做 TX throughput benchmark。

## 5. RX 性能诊断结论

### 已排除/拒绝

ReceiveView / second-copy reduction：

```text
约 68.65 kpps
低于当时 clean baseline ~71.1 kpps
→ Rejected
```

software RX interrupt batching：

```text
约 63.9~64.0 kpps
并混入 RBUE/RIE 行为变化
→ Rejected
```

RBUE-only disable：

```text
40 kpps wall-cycle 显著恶化
80 kpps profiling RX 149231 → 114867
→ Rejected
```

### profiling 关键结果

I-Cache Disabled 时，40 kpps profiler 显示：

```text
EthernetDriver_Receive frame_avg ≈ 5659 cycles
HAL_ETH_ReadData hal_frame_avg   ≈ 4064 cycles
RI-only HAL IRQ                  ≈ 2142 cycles
```

80 kpps 饱和时几乎每次 RX IRQ 都带 RBU/AIS；Thread Flag `wake≈2`，说明饱和时 Runtime Task 已持续 drain，task wakeup 不是主瓶颈。

I-Cache Enabled 后同类 profiler 降至约：

```text
frame_avg     ≈ 2564 cycles
hal_frame_avg ≈ 1943 cycles
RI IRQ        ≈ 1207 cycles
```

随后 clean stress 独立确认吞吐显著提升。

## 6. RX clean baseline — Measured

### 60 B

```text
80 kpps  × 200000 → 200000 / 200000, HAL=0
90 kpps  × 200000 → 200000 / 200000, HAL=0
100 kpps × 200000 → 200000 / 200000, HAL=0
110 kpps × 200000 → 200000 / 200000, HAL=0
120 kpps × 200000 → 200000 / 200000, HAL=99
120 kpps × 1M     → 1000000 / 1000000, HAL=205
130 kpps × 200000 → 200000 / 200000, HAL=52777
140 kpps × 200000 → 186539 / 200000, HAL=186457
148.59 kpps       → 175866 / 200000, HAL=175686
```

两组过载点的实际接收平台均约：

```text
~130.6 kpps
```

100BASE-TX 最小帧理论 packet-rate 约 `148.8 kpps`，当前约达到 `87.8%`。

历史 I-Cache Disabled clean saturation 约 `71.1 kpps`；I-Cache Enabled 后约 `130.6 kpps`。

### 1514 B

```text
8000 pps × 200000
Elapsed = 25.000 s
RX      = 200000 / 200000
HAL/DMA = 0
script frame rate = 96.90 Mbit/s
estimated on-wire ≈ 98.43 Mbit/s
```

RX 大帧已确认接近 100M 线速。

注意：`120 kpps × 1M` 约 8.3 s、1514 B 测试约 25 s，只能算扩展高负载测试，不等价于真正长时间 Stress。

## 7. 当前 stats / telemetry

`EthernetDriver_GetStats()` 当前提供：

```text
rx_frames
rx_errors
rx_dropped
rx_buffer_unavailable

tx_queued
tx_retries
tx_errors
tx_completed

hal_error_events
last_hal_error_code
last_dma_error_code
last_mac_error_code
```

高负载 RX 已用该统计观测 RBU/AIS。`last_dma_error_code=0x00004080` 表示最近一次 DMA error snapshot，不等价于测试结束时仍持续故障。

## 8. 当前测试等级

已确认：

- Static Review：PASS；
- Debug / Release Build：PASS；
- map / ELF DMA layout：PASS；
- PHY / MAC startup：On-board PASS；
- async RX / TX ownership：On-board PASS；
- Driver stats：On-board PASS；
- I-Cache Enabled：On-board Verified；
- RX 60 B high-load / 1514 B near-line-rate：Measured。

## 9. 当前仍未完成

- DMA fatal / RBU / timeout recovery；
- async TX 异常路径完整 recovery；
- 完整 Link Down / Up MAC lifecycle；
- Task stack high-water mark；
- D-Cache-on；
- 真正长时间 Stress；
- TX throughput performance baseline；
- LwIP / Ping / UDP / TCP。

## 10. 下一工作单元候选

候选 1：**Link Down / Up 完整 MAC lifecycle**

目标：运行中拔线/插回后，重新读取 PHY 状态，正确 stop/reconfigure/start MAC/DMA，并验证 RX/TX ownership 与统计恢复。

候选 2：**TX throughput baseline**

如果决定 M2 同时冻结 TX 性能，则建立 60 B / 1514 B async TX benchmark，记录 queued/retry/completed/error 与实际 PC 接收结果。

下一对话只选择其中一个，不同时推进。
