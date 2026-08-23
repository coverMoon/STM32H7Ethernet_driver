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
- [ ] TX 异常路径完整 recovery；
- [ ] D-Cache-on。

基线布局：

```text
DMARxDscrTab = 0x30040000
DMATxDscrTab = 0x30040080
.eth_dma_rx  = 0x30042000 / 0x1800
.eth_dma_tx  = 0x30044000 / 0x1800
```

### 2. Raw Frame / RX

- [x] Raw TX baseline；
- [x] Raw RX 单帧；
- [x] polling RX 1000 / 1000；
- [x] async RX 1000 / 1000；
- [x] `EthernetRtos_RuntimeTask()` 改名与 TX completion 加入后的 async RX 1000 / 1000 回归。

当前 RX 测试 EtherType `0x88B5`，PC 基础回归约 5 ms / Frame。1000 / 1000 只验证基础连续 ownership，不是 Stress。

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

该结果验证基础 completion ownership 与 Buffer recycle，不代表高负载吞吐或异常 recovery。

### 5. Driver Package / Reference Example 回归

Reference Example 当前路径：

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
- [x] CubeMX Generate Code 后 USER CODE 边界保持正确。

### 6. CubeMX Runtime Task generation 回归

当前采用：

```text
Task Name  = EthernetRuntime
Entry      = EthernetRtos_RuntimeTask
Generation = As weak
```

当前 Reference Example 已完成：

- [x] CubeMX 6.18.1 UI 配置；
- [x] Generate Code；
- [x] `.ioc` diff 符合预期；
- [x] `freertos.c` 生成 `__weak` Task Entry；
- [x] CubeMX 仍管理 Task attributes / `osThreadNew()`；
- [x] Package 强定义链接无冲突；
- [x] Build；
- [x] async RX 回归；
- [x] async TX completion 回归。

### 7. M2 仍未完成

- [ ] RX/TX error / drop 统计；
- [ ] DMA fatal / RBU / timeout recovery；
- [ ] Link Down / Up 完整 MAC lifecycle；
- [ ] Task stack high-water mark；
- [ ] 长时间 / 高负载；
- [ ] D-Cache 场景。

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
