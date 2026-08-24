# STM32H7 Ethernet Driver

面向 STM32H7 的可复用 Ethernet Driver Package。当前基于 STM32 HAL，提供 MAC/DMA、MDIO、LAN8720 PHY、板级 Port，以及可选的 CMSIS-RTOS2 异步 RX/TX Runtime Adapter。

当前参考验证平台：

```text
MCU       : STM32H743VIT6
PHY       : LAN8720AI
Interface : RMII
RTOS      : FreeRTOS / CMSIS-RTOS2
CubeH7    : V1.13.0
HAL ETH   : 1.11.6
```

完整参考工程位于：

```text
examples/STM32H743_LAN8720_FreeRTOS/
```

## 1. Driver Package 架构

```text
Application / LwIP
        ↓
RTOS Adapter / ethernetif
        ↓
Ethernet Driver
    ├── STM32H7 MAC / DMA
    └── MDIO
        ↓
PHY Driver
        ↓
Ethernet Port
        ↓
STM32 HAL / Board
```

职责边界：

- `Ethernet Driver`：MAC/DMA、Descriptor、RX/TX DMA Buffer ownership、Frame API、运行统计；
- `MDIO`：封装 STM32 HAL PHY Management API；
- `PHY`：具体 PHY 芯片行为，当前提供 LAN8720；
- `Port`：目标板绑定，只提供 HAL ETH Handle、PHY Reset 和 DMA SRAM 准备；
- `RTOS/CMSIS_RTOS2`：可选异步 Adapter，把 IRQ 事件转为 Thread Flag，在任务上下文完成 RX drain / TX completion reclaim；
- `ethernetif` / LwIP、IP、UDP、TCP 和应用业务不进入 Driver Core。

运行时 callback、weak symbol、IRQ/Task 交接和 Buffer ownership 的详细说明见 [`docs/ETHERNET_RUNTIME_FLOW.md`](docs/ETHERNET_RUNTIME_FLOW.md)。

目录：

```text
Ethernet/
├── Inc/
│   ├── ethernet_driver.h
│   └── ethernet_mdio.h
├── Src/
│   ├── ethernet_driver.c
│   └── ethernet_mdio.c
├── PHY/
│   └── LAN8720/
│       ├── Inc/lan8720.h
│       └── Src/lan8720.c
├── Port/
│   └── Inc/ethernet_port.h
└── RTOS/
    └── CMSIS_RTOS2/
        ├── Inc/ethernet_rtos.h
        └── Src/ethernet_rtos.c
```

迁移到另一块 STM32H7 板时，首先复制整个 `Ethernet/` 目录；目标工程负责自己的 CubeMX、linker、MPU、NVIC、Task 资源以及 `ethernet_port.c`。

## 2. Reference Example RX 性能基线

以下结果来自 STM32H743VIT6 + LAN8720AI + RMII、480 MHz Cortex-M7、I-Cache Enabled、D-Cache Disabled、RX8/TX4、clean Runtime。测试脚本为 `test/send_eth_stress.py`，测试 EtherType 为 `0x88B5`。

### 60 B 小包

| 输入速率 | 数量 | 接收 | HAL/RBU events |
| --- | ---: | ---: | ---: |
| 80 kpps | 200000 | 200000 | 0 |
| 90 kpps | 200000 | 200000 | 0 |
| 100 kpps | 200000 | 200000 | 0 |
| 110 kpps | 200000 | 200000 | 0 |
| 120 kpps | 1000000 | 1000000 | 205 |
| 130 kpps | 200000 | 200000 | 52777 |
| 140 kpps | 200000 | 186539 | 186457 |
| 148.59 kpps | 200000 | 175866 | 175686 |

两组过载点对应的实际接收平台均约为：

```text
~130.6 kpps
```

100BASE-TX 最小帧理论 packet-rate 约 `148.8 kpps`，当前 RX 饱和平台约达到理论值的 `87.8%`。

`120 kpps × 1,000,000` 测试中全部接收，`err/drop/no_buf = 0`，仅出现 205 次 HAL/RBU event。

### 1514 B 大包

```text
8000 pps × 200000
RX      = 200000 / 200000
HAL/DMA = 0
script frame rate = 96.90 Mbit/s
estimated on-wire ≈ 98.43 Mbit/s
```

该结果确认 RX 大帧已接近 100 Mbit/s 物理线速。

I-Cache 曾作为单变量验证：I-Cache Disabled 时 60 B clean saturation 约 `71.1 kpps`，Enabled 后约 `130.6 kpps`。因此当前 Reference Example 默认开启 I-Cache。

> 当前只建立了 RX throughput 性能基线。TX 已完成 async 1000-frame completion ownership 上板验证，但 TX 小包/大包 throughput、retry/backpressure 与线速基线尚未测量。

## 3. 接入流程总览

推荐顺序：

```text
1. 复制 Ethernet/
        ↓
2. CubeMX 配置 ETH / GPIO / MPU / NVIC / I-Cache
        ↓
3. 选择 Ethernet DMA 可访问 SRAM
        ↓
4. 修改 linker，固定 Descriptor 与 RX/TX Buffer
        ↓
5. 实现 ethernet_port.c
        ↓
6. 将 Driver 源码加入构建系统
        ↓
7. 初始化 HAL ETH 与 Ethernet Driver
        ↓
8. PHY Reset / MDIO / Auto-negotiation
        ↓
9. 根据 PHY Speed / Duplex 配置并启动 MAC/DMA
        ↓
10. 需要异步 Runtime 时创建 RTOS Task 并接入 Adapter
        ↓
11. 后续接入 ethernetif / LwIP
```

STM32H7 Ethernet 最容易出现隐蔽错误的部分是 DMA SRAM、Descriptor、Buffer、MPU、Cache、linker 和 ownership。不要把“CPU 能访问某块内存”直接等价成“Ethernet DMA 能访问”。

## 4. CubeMX 配置

### 4.1 ETH 与 RMII/MII

在 CubeMX 中启用：

```text
Connectivity
→ ETH
→ Media Interface: RMII 或 MII
```

RMII 至少涉及：

```text
REF_CLK
MDC
MDIO
CRS_DV
RXD0
RXD1
TX_EN
TXD0
TXD1
```

当前参考板使用 LAN8720AI，并由 PHY 向 STM32 输入 RMII REF_CLK。PHY `nRST` 使用普通 GPIO，由 Port 实现 Reset Assert / Release。

### 4.2 Descriptor 与 Buffer

当前 STM32H743 Example：

```text
ETH_RX_DESC_CNT = 8
ETH_TX_DESC_CNT = 4
RxBuffLen       = 1536 B
RxDescAddress   = 0x30040000
TxDescAddress   = 0x30040100
```

CubeMX 生成：

```text
DMARxDscrTab[] → .RxDescripSection
DMATxDscrTab[] → .TxDescripSection
```

Driver 的 payload Buffer 使用通用 input section：

```text
.eth_dma_buffer.rx
.eth_dma_buffer.tx
```

物理地址由目标工程 linker 决定。

### 4.3 MPU / Cache

当前 STM32H743 Example：

```text
Region 1
Base        : 0x30040000
Size        : 32 KiB
Type        : Normal
Cacheable   : No
Bufferable  : No
Shareable   : Yes
Execute     : Never

Region 2
Base        : 0x30040000
Size        : 512 B
Type        : Device
Cacheable   : No
Bufferable  : Yes
Execute     : Never
```

Region 2 编号更高，用于覆盖 RX/TX Descriptor 区域。

当前 CPU Cache：

```text
I-Cache = Enabled
D-Cache = Disabled
```

I-Cache 与 Ethernet DMA data coherence 无冲突；它只改善 CPU 指令取值。若目标工程启用 D-Cache 或把 DMA Buffer 放入 Cacheable RAM，必须重新设计并验证 `Clean / Invalidate`、32-byte Cache Line 对齐和 ownership 切换。

### 4.4 ETH IRQ

当前 Example：

```text
ETH_IRQn Preemption Priority = 5
Sub Priority                 = 0
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5
```

`5` 只是当前参考工程参数。目标工程必须根据自己的 `configPRIO_BITS` 与 FreeRTOS interrupt priority 约束重新核对。

### 4.5 FreeRTOS / CMSIS-RTOS2 Task

Driver Package不创建 Task，也不冻结 priority、stack 或 allocation。

当前 Reference Example 使用：

```text
Task Name  : EthernetRuntime
Task Entry : EthernetRtos_RuntimeTask
Generation : As weak
```

CubeMX 负责 Task Object、priority、stack、allocation 和 `osThreadNew()`；generated `freertos.c` 提供 weak stub，Package 提供同名强定义实现。

非 CubeMX 用户可以直接用 CMSIS-RTOS2 / FreeRTOS 创建任务，入口指向 `EthernetRtos_RuntimeTask()`。

## 5. 当前 STM32H743 DMA 内存布局

参考工程将 SRAM3 独立为 Ethernet DMA 区域：

```text
RAM_ETH = 0x30040000 ~ 0x30047FFF / 32 KiB
RAM_D2  = 0x30000000 ~ 0x3003FFFF / 256 KiB
```

当前布局：

```text
0x30040000  RX Descriptor, 8 × 24 B, linker slot 0x100
0x30040100  TX Descriptor, 4 × 24 B, linker slot 0x100
0x30042000  RX Pool, 8 × 1536 B = 0x3000
0x30045000  TX Pool, 4 × 1536 B = 0x1800
```

这些地址只属于当前 STM32H743 Example。更换 STM32H7 型号时必须重新确认 Ethernet DMA 可达 SRAM。

当前板在 `MX_ETH_Init()` 前调用：

```c
EthernetPort_PrepareDmaMemory();
```

其实现会使能 D2 SRAM3 clock。

## 6. Linker 配置

当前 Example linker：

```text
examples/STM32H743_LAN8720_FreeRTOS/STM32H743xx_FLASH.ld
```

核心 section：

```ld
.RxDescripSection 0x30040000 (NOLOAD) : { ... } >RAM_ETH
.TxDescripSection 0x30040100 (NOLOAD) : { ... } >RAM_ETH
.eth_dma_rx       0x30042000 (NOLOAD) : { ... } >RAM_ETH
.eth_dma_tx       0x30045000 (NOLOAD) : { ... } >RAM_ETH
```

当前 ASSERT 约束：

```text
RX descriptor slot <= 0x100
TX descriptor slot <= 0x100
RX pool size = 0x3000
TX pool size = 0x1800
```

成功链接不等于 DMA 布局正确。构建后至少检查：

```bash
grep -E "RxDescripSection|TxDescripSection|eth_dma_rx|eth_dma_tx|DMARxDscrTab|DMATxDscrTab" \
  build/Debug/stm32H7ethernet_demo.map

arm-none-eabi-nm -n build/Debug/stm32H7ethernet_demo.elf | \
  grep -E "DMARxDscrTab|DMATxDscrTab"
```

## 7. 实现 Ethernet Port

头文件：

```text
Ethernet/Port/Inc/ethernet_port.h
```

接口：

```c
ETH_HandleTypeDef *EthernetPort_GetHandle(void);
void EthernetPort_PrepareDmaMemory(void);
void EthernetPort_PhyResetAssert(void);
void EthernetPort_PhyResetRelease(void);
```

当前参考实现：

```text
examples/STM32H743_LAN8720_FreeRTOS/BSP/stm32h743vit6_iot/ethernet_port.c
```

通用 `ethernet_driver.c` / `ethernet_mdio.c` 不应 include 某个 Demo 的 `eth.h`，也不应假设 HAL Handle 一定叫 `heth`。

## 8. 将源码加入构建系统

Core：

```text
Ethernet/Src/ethernet_driver.c
Ethernet/Src/ethernet_mdio.c
```

LAN8720：

```text
Ethernet/PHY/LAN8720/Src/lan8720.c
```

CMSIS-RTOS2 Adapter：

```text
Ethernet/RTOS/CMSIS_RTOS2/Src/ethernet_rtos.c
```

Include path：

```text
Ethernet/Inc
Ethernet/Port/Inc
Ethernet/PHY/LAN8720/Inc
Ethernet/RTOS/CMSIS_RTOS2/Inc
```

目标工程还必须提供自己的 `ethernet_port.c`，并启用 STM32 HAL ETH module。当前实现以 STM32H7 HAL 1.11.6 API 完成验证；不同 CubeH7/HAL 版本必须重新核对 Ethernet HAL API。

## 9. 初始化与 PHY Bring-up

推荐顺序：

```text
MPU_Config()
↓
SCB_EnableICache()
↓
HAL_Init()
↓
SystemClock_Config()
↓
EthernetPort_PrepareDmaMemory()
↓
MX_GPIO_Init()
↓
MX_ETH_Init()
↓
EthernetDriver_Init()
↓
创建 RTOS Task / 启动 Scheduler
↓
PHY Reset Release
↓
PHY Ready / Auto-negotiation
↓
读取 Link / Speed / Duplex
↓
EthernetDriver_ConfigureLink()
↓
EthernetDriver_Start()
```

如果使用 RTOS Adapter，在 `EthernetDriver_Start()` 前应确保：

```c
EthernetRtos_IsReady() == true
```

所有等待都应带 timeout。

## 10. Driver Frame API

```c
void EthernetDriver_Init(void);
void EthernetDriver_SetRxEventHandler(EthernetDriverRxEventHandler handler, void *context);
void EthernetDriver_SetTxEventHandler(EthernetDriverTxEventHandler handler, void *context);

bool EthernetDriver_ConfigureLink(
    EthernetLinkSpeed speed,
    EthernetDuplexMode duplex);

bool EthernetDriver_Start(void);

EthernetTxResult EthernetDriver_TransmitAsync(
    const uint8_t *frame,
    uint16_t length);

void EthernetDriver_ProcessTxCompletions(void);

EthernetRxResult EthernetDriver_Receive(
    uint8_t *frame,
    uint16_t capacity,
    uint16_t *length);

bool EthernetDriver_GetStats(EthernetDriverStats *stats);
```

TX 返回值：

```text
ETHERNET_TX_QUEUED → 已复制并提交，caller Frame 可立即复用
ETHERNET_TX_RETRY  → 临时无 Buffer / Descriptor，可稍后重试
ETHERNET_TX_ERROR  → 参数或 Driver 状态错误
```

当前不隐藏软件 TX Queue。

## 11. CMSIS-RTOS2 异步 Runtime

RX：

```text
ETH IRQ
→ HAL_ETH_RxCpltCallback()
→ Driver RX event
→ osThreadFlagsSet()
→ EthernetRtos_RuntimeTask()
→ drain EthernetDriver_Receive()
→ RX Frame Handler
```

TX：

```text
HAL_ETH_Transmit_IT()
→ TX IRQ
→ HAL_ETH_TxCpltCallback()
→ Driver TX event
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
```

应用注册任务上下文 RX Handler：

```c
EthernetRtos_SetRxFrameHandler(MyRxHandler, my_context);
```

`frame` 只在 Handler 调用期间有效；如需长期保存必须自行复制。

Thread Flag 是事件位，不是 Packet Counter，因此 Runtime Task 每次收到 RX event 后必须持续 drain 到 `ETHERNET_RX_NONE`。

## 12. Driver stats

`EthernetDriver_GetStats()` 提供只读快照：

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

统计用于诊断和测试，不控制 Driver 行为。高速路径内不做 `printf`。

## 13. Reference Example 与当前验证

完整参考工程：

```text
examples/STM32H743_LAN8720_FreeRTOS/
```

已验证：

```text
LAN8720 Reset / MDIO / PHY ID
Auto-negotiation
Link Up / Down
100 Mbit/s Full Duplex
STM32H7 MAC Speed / Duplex 同步
current DMA Descriptor / Pool layout
Raw Ethernet TX / RX
Polling RX 1000 / 1000
CMSIS-RTOS2 async RX 1000 / 1000
Async TX 1000-frame completion recycle
Driver stats
I-Cache Enabled
60 B RX high-load baseline
1514 B RX near-line-rate baseline
```

尚未完成：

```text
DMA fatal / RBU / timeout recovery
完整 Link Down / Up MAC lifecycle
Task stack high-water mark
D-Cache-on 专项验证
真正长时间 Stress
TX throughput baseline
LwIP / Ping / UDP / TCP
```

Example 的构建、烧录和测试方法见 [`examples/STM32H743_LAN8720_FreeRTOS/README.md`](examples/STM32H743_LAN8720_FreeRTOS/README.md)。详细架构、内存和项目状态见 `docs/stm32h7_ethernet_project_docs/`。
