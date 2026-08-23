# Ethernet DMA / MPU / Cache Design

本文记录 Driver 对 DMA 内存、Descriptor、Buffer、MPU、Cache 和 linker 的约束，并以 STM32H743 Reference Example 的已验证布局作为示例。完整用户接入流程以根 README 为准；RX/TX Runtime 调用链见 `docs/ETHERNET_RUNTIME_FLOW.md`。

## 1. 设计原则

Ethernet DMA 数据路径必须满足：

- DMA Master 能访问 Descriptor / Buffer 所在 SRAM；
- Descriptor / Buffer 地址由 linker 显式控制；
- Memory Attribute 与 Cache 策略一致；
- 32-byte Cache Line 对齐明确；
- CPU / DMA ownership 明确；
- `.map` / ELF 可验证实际地址；
- 物理 SRAM 地址不写入通用 Driver。

普通 `static` 数组不能自动等价于 DMA-safe memory。

## 2. STM32H743 Reference Example

当前选择 SRAM3：

```text
RAM_ETH / SRAM3
Base = 0x30040000
Size = 32 KiB
End  = 0x30047FFF
```

普通 D2 RAM：

```text
RAM_D2
0x30000000 ~ 0x3003FFFF
256 KiB
```

板级 linker：

```text
examples/STM32H743_LAN8720_FreeRTOS/STM32H743xx_FLASH.ld
```

Reason：Ethernet DMA 可达、与普通应用 RAM 分离、容量足够、便于用一个 MPU Region 管理。

## 3. Descriptor

```text
ETH_RX_DESC_CNT = 4
ETH_TX_DESC_CNT = 4
sizeof(ETH_DMADescTypeDef) = 24 B
4 × 24 B = 96 B
```

布局：

| 对象 | 地址 | 实际大小 | 预留 |
| --- | --- | ---: | ---: |
| RX Descriptor | `0x30040000` | 96 B | 128 B |
| TX Descriptor | `0x30040080` | 96 B | 128 B |

CubeMX input section：

```text
.RxDescripSection
.TxDescripSection
```

Reference linker：

```ld
.RxDescripSection 0x30040000 (NOLOAD) :
{
    . = ALIGN(32);
    KEEP(*(.RxDescripSection))
    . = ALIGN(32);
} >RAM_ETH

.TxDescripSection 0x30040080 (NOLOAD) :
{
    . = ALIGN(32);
    KEEP(*(.TxDescripSection))
    . = ALIGN(32);
} >RAM_ETH
```

同时检查地址、section 非空、每组不超过 128 B slot。

## 4. RX / TX Buffer Pool

Driver 当前配置：

```text
RX Count  = 4
TX Count  = 4
Buffer    = 1536 B
Alignment = 32 B
```

Driver 只定义 input section：

```text
.eth_dma_buffer.rx
.eth_dma_buffer.tx
```

Reference Example 的物理布局：

| 对象 | 地址范围 | 大小 |
| --- | --- | ---: |
| RX Pool | `0x30042000 ~ 0x300437FF` | `0x1800` / 6144 B |
| TX Pool | `0x30044000 ~ 0x300457FF` | `0x1800` / 6144 B |

```ld
.eth_dma_rx 0x30042000 (NOLOAD) :
{
    . = ALIGN(32);
    KEEP(*(.eth_dma_buffer.rx))
    . = ALIGN(32);
} >RAM_ETH

.eth_dma_tx 0x30044000 (NOLOAD) :
{
    . = ALIGN(32);
    KEEP(*(.eth_dma_buffer.tx))
    . = ALIGN(32);
} >RAM_ETH
```

关键布局：

```text
0x30040000  RX Descriptor
0x30040080  TX Descriptor
0x30042000  RX Pool
0x30044000  TX Pool
0x30045800  TX Pool end + 1
0x30048000  RAM_ETH end + 1
```

## 5. Linker ASSERT

Reference Example 使用地址/大小断言，至少保证：

```text
ADDR(.RxDescripSection) = 0x30040000
ADDR(.TxDescripSection) = 0x30040080
ADDR(.eth_dma_rx)       = 0x30042000
SIZEOF(.eth_dma_rx)     = 0x1800
ADDR(.eth_dma_tx)       = 0x30044000
SIZEOF(.eth_dma_tx)     = 0x1800
```

Descriptor Count / Buffer Count / Buffer Size 变化后必须同步修改 linker 预留与断言。

## 6. RX ownership

当前 `HAL_ETH_Start_IT()` 建立 RX Descriptor，并通过强符号 `HAL_ETH_RxAllocateCallback()` 从静态 RX Pool 获取 Buffer。

任务上下文读取：

```text
DMA owns RX Buffer
→ Frame received / IRQ
→ Runtime Task calls EthernetDriver_Receive()
→ HAL_ETH_ReadData()
→ HAL_ETH_RxLinkCallback()
→ memcpy to Driver CPU-side frame storage
→ RX DMA Buffer immediately released to pool
→ HAL rebuilds descriptor and reallocates
→ EthernetDriver_Receive() copies to RTOS Adapter frame
→ Frame Handler
```

上层不会长期持有 DMA RX Buffer。copy-based ownership 已完成单帧、polling 1000/1000、async 1000/1000 上板验证。

当前 RX 有两次 copy：

```text
DMA RX Buffer
→ Driver CPU-side Frame
→ RTOS Adapter Frame
```

第一版接受该开销以换取简单明确的 ownership。

## 7. TX ownership

当前 TX 已切换为 copy-based async：

```text
Caller Frame
→ acquire static TX DMA Buffer
→ memcpy
→ HAL_ETH_Transmit_IT()
→ HAL / DMA owns Buffer
→ TX complete IRQ
→ Runtime Task
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ release TX Buffer to Driver Pool
```

关键点：

1. `EthernetDriver_TransmitAsync()` 在提交前先尝试 `EthernetDriver_ProcessTxCompletions()`，作为 completion reclaim backstop；
2. caller Frame 只在 `memcpy()` 前属于 caller，返回 `ETHERNET_TX_QUEUED` 后即可复用；
3. `tx_config.pData` 保存 Driver TX DMA Buffer 地址，HAL 会把该地址记录到 `TxDescList.PacketAddress[]`；
4. `HAL_ETH_ReleaseTxPacket()` 确认 Descriptor `OWN == 0` 后调用 `HAL_ETH_TxFreeCallback()`；
5. `HAL_ETH_TxFreeCallback()` 最终把对应 Driver TX Buffer 标记为空闲；
6. 当前没有隐藏的软件 TX Queue；无 Buffer/Descriptor 时返回 `ETHERNET_TX_RETRY`。

当前 TX 只有一次 copy：

```text
Caller Frame
→ Driver TX DMA Buffer
→ DMA
```

## 8. TX submit / reclaim 并发保护

`HAL_ETH_Transmit_IT()` 与 `HAL_ETH_ReleaseTxPacket()` 都会操作 HAL TX Descriptor bookkeeping：

```text
CurTxDesc
PacketAddress[]
BuffersInUse
releaseIndex
Descriptor OWN / IOC
```

当前 Driver 使用短 PRIMASK critical section 序列化：

```text
TX Pool acquire/release
HAL_ETH_Transmit_IT()
HAL_ETH_ReleaseTxPacket()
```

Frame `memcpy()` 不放在整个关中断区内。

这个临界区用于保证 TX submit 与 reclaim 不并发破坏同一套 HAL/Driver ownership 状态；Driver Core 因此仍不依赖 FreeRTOS mutex。

## 9. MPU

当前 Example `.ioc` 配置 Cortex-M7 MPU，不使用 CubeMX Memory Management Tool 自动管理 Ethernet linker section。

Region 1：

```text
Base          0x30040000
Size          32 KiB
TEX           1
Access        Full Access
XN            Yes
Shareable     Yes
Cacheable     No
Bufferable    No
```

Region 2：

```text
Base          0x30040000
Size          256 B
TEX           0
Access        Full Access
XN            Yes
Shareable     No
Cacheable     No
Bufferable    Yes
```

Region 2 编号更高，因此：

```text
0x30040000 ~ 0x300400FF  Device / Non-cacheable
0x30040100 ~ 0x30047FFF  Normal / Non-cacheable
```

## 10. D-Cache

当前 Example I-Cache / D-Cache Disabled，且 Ethernet SRAM3 Non-cacheable。

如果以后把 Buffer 放到 Cacheable RAM，必须重新设计：

- Clean / Invalidate 时机；
- 地址/长度向 32-byte Cache Line 扩展；
- ownership 切换；
- memory barrier；
- HAL Descriptor 生命周期。

在内存属性不变前，不提前往通用 Driver 加没有实际需求的 Cache maintenance 抽象。

## 11. SRAM3 clock / Port

当前板 Port：

```text
examples/STM32H743_LAN8720_FreeRTOS/BSP/stm32h743vit6_iot/ethernet_port.c
```

`EthernetPort_PrepareDmaMemory()` 显式调用 D2 SRAM3 clock enable，并在 `MX_ETH_Init()` 前执行：

```text
MPU_Config
→ HAL_Init
→ SystemClock_Config
→ EthernetPort_PrepareDmaMemory
→ MX_GPIO_Init
→ MX_ETH_Init
→ EthernetDriver_Init
```

## 12. CubeMX / MMT 边界

```text
Example .ioc
→ ETH Descriptor addresses
→ MPU Regions

Example linker
→ RAM_ETH physical region
→ Descriptor output sections
→ RX/TX Pool output sections
→ ASSERT

Port
→ current board SRAM preparation

Driver
→ generic Buffer input sections
→ ownership
```

`cmake/stm32cubemx/CMakeLists.txt` 属于生成文件，不手工修改。

CubeMX 6.18.1 可能在 `.ioc` 内保留 MMT metadata，但当前 `MMTConfigApplied=false`；项目实际 DMA layout 仍以 linker + map/ELF 为准。

## 13. Map / ELF 验证

在 Reference Example 目录构建后：

```bash
grep -E "RxDescripSection|TxDescripSection|eth_dma_rx|eth_dma_tx|DMARxDscrTab|DMATxDscrTab" \
  build/Debug/stm32H7ethernet_demo.map

arm-none-eabi-nm -n build/Debug/stm32H7ethernet_demo.elf | \
  grep -E "DMARxDscrTab|DMATxDscrTab"
```

当前 Reference Example 迁移到 `examples/` 并完成 RuntimeTask / async TX 修改后，仍已验证：

```text
DMARxDscrTab = 0x30040000
DMATxDscrTab = 0x30040080
.eth_dma_rx  = 0x30042000 / 0x1800
.eth_dma_tx  = 0x30044000 / 0x1800
```

## 14. 当前验证边界

已验证：

```text
RX copy-based ownership + async RX 1000/1000
TX copy-based async ownership + 1000-frame completion recycle
RuntimeTask 改名后的 RX 回归
map / ELF DMA layout
```

尚未验证：

```text
D-Cache-on
高负载 / 长时间 Stress
DMA fatal / timeout recovery
Link lifecycle 下的 Buffer/Descriptor recovery
```

## 15. 跨板迁移

必须重新确认目标 MCU 的：DMA 可达 SRAM、容量、clock、MPU base/size/attribute、Descriptor/Buffer 地址、linker MEMORY、map/ELF 实际地址。

通用 `Ethernet/` 中不得出现当前板物理事实：

```text
0x30040000
SRAM3
RAM_D2
RAM_ETH
```

这些只属于 Reference Example / 目标板配置。
