# Ethernet DMA / MPU / Cache Design

本文记录 Driver 对 DMA 内存、Descriptor、Buffer、MPU、Cache 和 linker 的约束，并以 STM32H743 Reference Example 的当前已验证布局作为示例。完整用户接入流程以根 README 为准；RX/TX Runtime 调用链见 `docs/ETHERNET_RUNTIME_FLOW.md`。

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

原因：Ethernet DMA 可达、与普通应用 RAM 分离、容量足够、便于用明确 MPU Region 管理。

## 3. Descriptor

当前 `.ioc` / HAL 基线：

```text
ETH_RX_DESC_CNT = 8
ETH_TX_DESC_CNT = 4
sizeof(ETH_DMADescTypeDef) = 24 B
```

布局：

| 对象 | 地址 | 实际大小 | linker 预留 |
| --- | --- | ---: | ---: |
| RX Descriptor | `0x30040000` | `8 × 24 = 192 B` | 256 B |
| TX Descriptor | `0x30040100` | `4 × 24 = 96 B` | 256 B |

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

.TxDescripSection 0x30040100 (NOLOAD) :
{
    . = ALIGN(32);
    KEEP(*(.TxDescripSection))
    . = ALIGN(32);
} >RAM_ETH
```

当前每组 slot 上限均为 `0x100`。

## 4. RX / TX Buffer Pool

Driver 数组数量直接跟随 HAL Descriptor Count：

```text
RX Count  = ETH_RX_DESC_CNT = 8
TX Count  = ETH_TX_DESC_CNT = 4
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
| RX Pool | `0x30042000 ~ 0x30044FFF` | `0x3000` / 12288 B / 8×1536 B |
| TX Pool | `0x30045000 ~ 0x300467FF` | `0x1800` / 6144 B / 4×1536 B |

```ld
.eth_dma_rx 0x30042000 (NOLOAD) :
{
    . = ALIGN(32);
    KEEP(*(.eth_dma_buffer.rx))
    . = ALIGN(32);
} >RAM_ETH

.eth_dma_tx 0x30045000 (NOLOAD) :
{
    . = ALIGN(32);
    KEEP(*(.eth_dma_buffer.tx))
    . = ALIGN(32);
} >RAM_ETH
```

关键布局：

```text
0x30040000  RX Descriptor
0x30040100  TX Descriptor
0x30040200  Descriptor overlay end + 1
0x30042000  RX Pool
0x30045000  TX Pool
0x30046800  TX Pool end + 1
0x30048000  RAM_ETH end + 1
```

## 5. Linker ASSERT

Reference Example 当前至少保证：

```text
ADDR(.RxDescripSection) = 0x30040000
SIZEOF(.RxDescripSection) <= 0x100
ADDR(.TxDescripSection) = 0x30040100
SIZEOF(.TxDescripSection) <= 0x100
ADDR(.eth_dma_rx)       = 0x30042000
SIZEOF(.eth_dma_rx)     = 0x3000
ADDR(.eth_dma_tx)       = 0x30045000
SIZEOF(.eth_dma_tx)     = 0x1800
```

Descriptor Count / Buffer Count / Buffer Size 变化后必须同步修改 `.ioc`、linker 预留/断言，并重新检查 map/ELF。

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

上层不会长期持有 DMA RX Buffer。当前 RX 仍有两次 copy：

```text
DMA RX Buffer
→ Driver CPU-side Frame
→ RTOS Adapter Frame
```

ReceiveView / one-copy 诊断实验没有提升最终吞吐，已回退；第一版继续保留 copy-first ownership。

## 7. TX ownership

当前 TX 为 copy-based async：

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
2. 返回 `ETHERNET_TX_QUEUED` 后 caller Frame 可立即复用；
3. `tx_config.pData` 保存 Driver TX DMA Buffer 地址，HAL 将其记录到 `TxDescList.PacketAddress[]`；
4. `HAL_ETH_ReleaseTxPacket()` 完成后调用 `HAL_ETH_TxFreeCallback()`；
5. 当前没有隐藏的软件 TX Queue；无 Buffer/Descriptor 时返回 `ETHERNET_TX_RETRY`。

当前 TX 只有一次 copy：

```text
Caller Frame
→ Driver TX DMA Buffer
→ DMA
```

## 8. TX submit / reclaim 并发保护

`HAL_ETH_Transmit_IT()` 与 `HAL_ETH_ReleaseTxPacket()` 都会操作 HAL TX Descriptor bookkeeping。当前 Driver 使用短 PRIMASK critical section 序列化 TX Pool acquire/release、HAL submit 和 reclaim；Frame `memcpy()` 不放在整个关中断区内。

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
Size          512 B
TEX           0
Access        Full Access
XN            Yes
Cacheable     No
Bufferable    Yes
```

Region 2 编号更高，因此：

```text
0x30040000 ~ 0x300401FF  Device / Non-cacheable
0x30040200 ~ 0x30047FFF  Normal / Non-cacheable
```

512 B overlay 覆盖 RX/TX Descriptor 预留区，不覆盖 payload Pool。

## 10. I-Cache / D-Cache

当前 Reference Example：

```text
I-Cache = Enabled
D-Cache = Disabled
Ethernet SRAM3 = Non-cacheable
```

I-Cache 已通过 RX 性能单变量实验验证为关键配置：关闭时 60 B clean saturation 约 `71.1 kpps`；开启后 clean saturation 约 `130.6 kpps`。I-Cache 影响 Flash 代码取指，不改变 Ethernet DMA Buffer 的 data coherence。

D-Cache-on 尚未验证。如果以后把 Buffer 放到 Cacheable RAM，必须重新设计：

- Clean / Invalidate 时机；
- 地址/长度向 32-byte Cache Line 扩展；
- ownership 切换；
- memory barrier；
- HAL Descriptor 生命周期。

在内存属性不变前，不提前往通用 Driver 加未验证的 Cache maintenance 抽象。

## 11. SRAM3 clock / Port

当前板 Port：

```text
examples/STM32H743_LAN8720_FreeRTOS/BSP/stm32h743vit6_iot/ethernet_port.c
```

`EthernetPort_PrepareDmaMemory()` 显式调用 D2 SRAM3 clock enable，并在 `MX_ETH_Init()` 前执行：

```text
MPU_Config
→ I-Cache enable
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
→ ETH Descriptor count / addresses
→ MPU Regions
→ CPU Cache enable state

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

CubeMX 6.18.1 可能在 `.ioc` 内保留 MMT metadata，但当前 `MMTConfigApplied=false`；项目实际 DMA layout 仍以 `.ioc` + linker + map/ELF 为准。

## 13. Map / ELF 验证

在 Reference Example 目录构建后：

```bash
grep -E "RxDescripSection|TxDescripSection|eth_dma_rx|eth_dma_tx|DMARxDscrTab|DMATxDscrTab" \
  build/Debug/stm32H7ethernet_demo.map

arm-none-eabi-nm -n build/Debug/stm32H7ethernet_demo.elf | \
  grep -E "DMARxDscrTab|DMATxDscrTab"
```

当前目标布局：

```text
DMARxDscrTab = 0x30040000
DMATxDscrTab = 0x30040100
.eth_dma_rx  = 0x30042000 / 0x3000
.eth_dma_tx  = 0x30045000 / 0x1800
```

## 14. 当前验证边界

已验证：

```text
RX copy-based ownership + async RX
TX copy-based async ownership + 1000-frame completion recycle
current RX8/TX4 linker layout
I-Cache Enabled
60 B high-load RX clean baseline
1514 B near-line-rate RX baseline
```

Measured RX：

```text
60 B @ 120 kpps × 1,000,000 → 100% received
60 B saturation platform     → ~130.6 kpps
1514 B @ 8000 pps × 200,000 → 100% received, 0 HAL/DMA error
```

尚未验证：

```text
D-Cache-on
真正长时间 Stress
DMA fatal / timeout recovery
完整 Link lifecycle 下的 Buffer/Descriptor recovery
TX throughput baseline
```

## 15. 跨板迁移

必须重新确认目标 MCU 的 DMA 可达 SRAM、容量、clock、MPU base/size/attribute、Descriptor/Buffer 地址、linker MEMORY、map/ELF 实际地址。

通用 `Ethernet/` 中不得出现当前板物理事实：

```text
0x30040000
SRAM3
RAM_D2
RAM_ETH
```

这些只属于 Reference Example / 目标板配置。
