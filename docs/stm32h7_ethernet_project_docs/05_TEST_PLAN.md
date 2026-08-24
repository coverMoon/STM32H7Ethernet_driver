# Test Plan

- 状态：Active
- 说明：结果始终区分 Static Review、Build Verified、On-board Verified、Measured。

## M0：项目基线

- [x] FreeRTOS 最小任务正常；
- [x] USART1 调试输出正常；
- [x] CubeMX 生成代码与手工代码边界明确；
- [x] 基础上板运行。

## M1：PHY Bring-up

已通过：

- [x] PHY Reset；
- [x] MDIO Read / Write；
- [x] PHY ID / Address / Strap；
- [x] Auto-negotiation；
- [x] Link Up / Down；
- [x] 100 Mbit/s；
- [x] Full Duplex；
- [x] 单次网线拔插状态恢复。

补充覆盖：10 Mbit/s、Half Duplex、连续快速插拔、多次 Reset、25 MHz 晶振与 RMII REF_CLK 独立测量。

## M2：MAC / DMA

### 1. Memory / Descriptor / Buffer

- [x] DMA 可达 SRAM选择 — Static Review；
- [x] RAM_ETH / Descriptor / Pool linker — Build / Map Verified；
- [x] MPU — Static Review + Build Verified；
- [x] SRAM3 clock 在 MX_ETH_Init 前准备；
- [x] RX ownership / recycle — On-board Verified；
- [x] TX copy-based async ownership / recycle — On-board Verified；
- [x] I-Cache Enabled — On-board Verified + RX performance Measured；
- [ ] TX 异常路径完整 recovery；
- [ ] D-Cache-on。

当前布局：

```text
DMARxDscrTab = 0x30040000 / 8 × 24 B
DMATxDscrTab = 0x30040100 / 4 × 24 B
.eth_dma_rx  = 0x30042000 / 0x3000 / 8 × 1536 B
.eth_dma_tx  = 0x30045000 / 0x1800 / 4 × 1536 B
```

MPU：SRAM3 32 KiB Normal Non-cacheable，前 512 B Device overlay。当前 I-Cache Enabled / D-Cache Disabled。

### 2. Raw Frame / RX

- [x] Raw TX baseline；
- [x] Raw RX 单帧；
- [x] polling RX 1000 / 1000；
- [x] async RX 1000 / 1000；
- [x] `EthernetRtos_RuntimeTask()` 改名与 TX completion 加入后的 async RX 1000 / 1000 回归；
- [x] RX high-load performance baseline — Measured；
- [x] 1514 B near-line-rate RX — Measured。

当前 RX 测试 EtherType `0x88B5`。

### 3. ETH IRQ + CMSIS-RTOS2 Runtime

- [x] ETH_IRQn；
- [x] IRQ priority 满足当前 FreeRTOS ISR API 约束；
- [x] `ETH_IRQHandler()` → HAL handler；
- [x] `HAL_ETH_Start_IT()`；
- [x] HAL RX complete → Driver RX event；
- [x] HAL TX complete → Driver TX event；
- [x] RX/TX Thread Flags；
- [x] `EthernetRtos_RuntimeTask()`；
- [x] RX 每次唤醒 drain 到 `ETHERNET_RX_NONE`；
- [x] TX completion task-side reclaim；
- [x] async RX 1000 / 1000；
- [x] async TX 1000-frame test。

### 4. Async TX completion ownership

当前路径：

```text
EthernetDriver_TransmitAsync()
→ copy Driver TX DMA Buffer
→ HAL_ETH_Transmit_IT()
→ TX IRQ
→ HAL_ETH_TxCpltCallback()
→ Driver TX event
→ Runtime Task
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ TX Buffer recycle
```

已完成：

- [x] 4×1536 B TX static pool；
- [x] `ETHERNET_TX_QUEUED / RETRY / ERROR`；
- [x] submit 前 completion reclaim backstop；
- [x] TX submit / reclaim 短 critical section 序列化；
- [x] HAL `pData` → `PacketAddress[]` → `TxFreeCallback()` ownership；
- [x] TX complete ISR 只通知，不直接 reclaim；
- [x] Runtime Task `HAL_ETH_ReleaseTxPacket()`；
- [x] 连续 1000-frame async TX 上板测试通过；
- [x] 测试后 RX 1000 / 1000 回归通过。

该结果验证基础 completion ownership 与 Buffer recycle，不代表 TX throughput 性能基线或异常 recovery。

### 5. Driver 可观测性

- [x] RX frame / error / drop；
- [x] RX buffer unavailable；
- [x] TX queued / retry / error / completion；
- [x] HAL error event count；
- [x] last HAL / DMA / MAC error snapshot；
- [x] `EthernetDriver_GetStats()` 只读快照 API；
- [x] 高负载测试中使用统计确认 RBU/AIS 行为。

### 6. Driver Package / Reference Example 回归

Reference Example：

```text
examples/STM32H743_LAN8720_FreeRTOS/
```

已完成：

- [x] Debug fresh Build；
- [x] Release fresh Build；
- [x] map / ELF RX/TX Descriptor；
- [x] map / ELF RX/TX Pool；
- [x] PHY / MAC startup；
- [x] async RX 1000 / 1000；
- [x] async TX 1000-frame completion recycle；
- [x] 无新增 HardFault；
- [x] CubeMX Generate Code 后 USER CODE 边界保持正确；
- [x] CubeMX 生成 I-Cache enable。

### 7. CubeMX Runtime Task generation 回归

当前采用：

```text
Task Name  = EthernetRuntime
Entry      = EthernetRtos_RuntimeTask
Generation = As weak
```

已完成：

- [x] CubeMX 6.18.1 UI 配置；
- [x] Generate Code；
- [x] `.ioc` diff 符合预期；
- [x] `freertos.c` 生成 `__weak` Task Entry；
- [x] CubeMX 仍管理 Task attributes / `osThreadNew()`；
- [x] Package 强定义链接无冲突；
- [x] Build；
- [x] async RX 回归；
- [x] async TX completion 回归。

### 8. RX 性能基线 — Measured

测试脚本：`test/send_eth_stress.py`。测试固件使用 I-Cache Enabled、D-Cache Disabled、RX8/TX4、clean Runtime，无 DWT/linker-wrap profiler。

60 B：

| 输入 | 数量 | 结果 | HAL/RBU events |
| --- | ---: | --- | ---: |
| 110 kpps | 200000 | 200000 / 200000 | 0 |
| 120 kpps | 200000 | 200000 / 200000 | 99 |
| 120 kpps | 1000000 | 1000000 / 1000000 | 205 |
| 130 kpps | 200000 | 200000 / 200000 | 52777 |
| 140 kpps | 200000 | 186539 / 200000 | 186457 |
| 148.59 kpps | 200000 | 175866 / 200000 | 175686 |

两组过载测试的实际接收平台均约 `130.6 kpps`，约为 100BASE-TX 最小帧理论 `148.8 kpps` 的 `87.8%`。

1514 B：

```text
8000 pps × 200000
→ 200000 / 200000
→ HAL/DMA error = 0
→ script Frame rate = 96.90 Mbit/s
→ estimated on-wire rate ≈ 98.43 Mbit/s
```

I-Cache 关闭时的历史 clean 60 B saturation 约 `71.1 kpps`；开启 I-Cache 后提升到约 `130.6 kpps`。该对比用于确认 I-Cache 是此前异常低小包性能的主要原因。

诊断实验归档：

- ReceiveView / one-copy：Rejected，未提升吞吐；
- software RX interrupt batching：Rejected，吞吐下降；
- RBUE-only disable：Rejected，吞吐明显下降；
- I-Cache Enabled：Validated。

### 9. M2 仍未完成

- [ ] DMA fatal / RBU / timeout recovery；
- [ ] Link Down / Up 完整 MAC lifecycle；
- [ ] Task stack high-water mark；
- [ ] D-Cache 场景；
- [ ] 真正长时间 / 高负载 Stress；
- [ ] TX throughput 性能基线是否纳入 M2 收尾，待后续决定。

注意：`120 kpps × 1,000,000` 约 8.3 s，`1514 B × 200000 @ 8000 pps` 约 25 s，只能作为扩展高负载测试，不能替代数十分钟/数小时长时间 Stress。

## M3：LwIP + Ping

- [ ] ethernetif；
- [ ] Static IPv4；
- [ ] ARP；
- [ ] Ping；
- [ ] 持续 Ping / Link 恢复。

## M4：UDP

- [ ] UDP Echo；
- [ ] 小包 / 接近 MTU；
- [ ] 高频双向；
- [ ] Link 恢复；
- [ ] 长时间；
- [ ] Drop / Error 统计。

## M5：TCP

- [ ] Connect / Send / Receive；
- [ ] Disconnect / Reconnect；
- [ ] Client crash / cable disconnect / MCU reset；
- [ ] 长时间连接。

## M6：压力与通用化

- [ ] 数小时持续 UDP；
- [ ] 大量小包 / 大包 / 双向高负载；
- [ ] 快速 Link Up / Down；
- [ ] stack high-water mark；
- [ ] LwIP memory pool / CPU load / RX/TX drop / DMA error；
- [ ] 新 STM32H7 板以复制 `Ethernet/` + 配置 CubeMX/linker/Port 为主完成迁移。

## 测试记录要求

记录固件 commit、Cube/HAL 版本、前置条件、操作步骤、预期/实际、错误计数、验证等级；成功编译不等于功能上板验证。
