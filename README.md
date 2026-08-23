# STM32H7 Ethernet Driver

面向 STM32H7 的可复用 Ethernet Driver Package。当前基于 STM32 HAL，提供 STM32H7 MAC/DMA、MDIO、LAN8720 PHY、板级 Port，以及可选的 CMSIS-RTOS2 异步 RX 适配层。

当前参考验证平台：

```text
MCU       : STM32H743VIT6
PHY       : LAN8720AI
Interface : RMII
RTOS      : FreeRTOS / CMSIS-RTOS2
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

- `Ethernet Driver`：MAC/DMA、Descriptor、RX/TX DMA Buffer ownership、Frame API、HAL RX event；
- `MDIO`：封装 STM32 HAL PHY Management API；
- `PHY`：具体 PHY 芯片行为，当前提供 LAN8720；
- `Port`：目标板绑定，只提供 HAL ETH Handle、PHY Reset 和 DMA SRAM 准备；
- `RTOS/CMSIS_RTOS2`：可选异步 RX Adapter，把 IRQ 事件转为 Thread Flag，并在任务上下文读取 Frame；
- `ethernetif` / LwIP、IP、UDP、TCP 和应用业务不进入 Driver Core。

运行时 callback、weak symbol、IRQ/Task 交接和 RX Buffer ownership 的完整原理说明见 [`docs/ETHERNET_RUNTIME_FLOW.md`](docs/ETHERNET_RUNTIME_FLOW.md)。

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

迁移到另一块 STM32H7 板时，首先复制整个 `Ethernet/` 目录；目标工程再负责 CubeMX、linker、MPU、NVIC、Task 资源以及 `ethernet_port.c`。

## 2. 接入流程总览

推荐顺序：

```text
1. 复制 Ethernet/
        ↓
2. CubeMX 配置 ETH / GPIO / MPU / NVIC
        ↓
3. 选择 Ethernet DMA 可访问 SRAM
        ↓
4. 修改 linker，固定 Descriptor 与 RX/TX Buffer
        ↓
5. 实现 ethernet_port.c
        ↓
6. 将 Driver 源码加入自己的构建系统
        ↓
7. 初始化 HAL ETH 与 Ethernet Driver
        ↓
8. PHY Reset / MDIO / Auto-negotiation
        ↓
9. 根据 PHY Speed / Duplex 配置并启动 MAC/DMA
        ↓
10. 需要异步 RX 时创建 RTOS Task 并接入 Adapter
        ↓
11. 后续接入 ethernetif / LwIP
```

STM32H7 Ethernet 最容易出现隐蔽错误的部分不是 API 调用，而是 **DMA SRAM、Descriptor、Buffer、MPU、Cache、linker 和 ownership**。不要把“CPU 能访问某块内存”直接等价成“Ethernet DMA 能访问”。

## 3. CubeMX 配置

### 3.1 ETH 与 RMII/MII

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

这些引脚必须按目标 PCB / 原理图配置。当前参考板使用 LAN8720AI，并由 PHY 向 STM32 输入 RMII REF_CLK。

如果 PHY 有独立 `nRST`，建议把它配置成普通 GPIO Output，并在 Port 中实现 Reset Assert / Release。

### 3.2 Descriptor 与 HAL RX Buffer Length

当前 STM32H743 Example：

```text
ETH_RX_DESC_CNT = 4
ETH_TX_DESC_CNT = 4
RxBuffLen       = 1536 B
RxDescAddress   = 0x30040000
TxDescAddress   = 0x30040080
```

CubeMX 生成的：

```text
DMARxDscrTab[] → .RxDescripSection
DMATxDscrTab[] → .TxDescripSection
```

最终物理地址仍由 linker 决定，必须与 CubeMX 中配置的 Descriptor 地址一致。

本 Driver 的 payload Buffer 不使用 CubeMX 自动创建的普通 `.bss` Buffer，而是定义通用 input section：

```text
.eth_dma_buffer.rx
.eth_dma_buffer.tx
```

目标工程 linker 再把这些 section 放到实际 DMA SRAM。

### 3.3 MPU

Cortex-M7 上必须让 MPU / Cache 策略与 DMA ownership 一致。

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
Size        : 256 B
Type        : Device
Cacheable   : No
Bufferable  : Yes
Shareable   : No
Execute     : Never
```

Region 2 编号更高，用于覆盖前 256 B Descriptor 区域。

当前 Example 的 I-Cache / D-Cache 均关闭；Ethernet SRAM 本身也配置为 Non-cacheable。若目标工程改成 Cacheable Buffer，必须重新设计并验证 `Clean / Invalidate`、32-byte Cache Line 对齐和 ownership 切换，不能直接复制当前策略。

### 3.4 ETH IRQ

异步 RX 需要开启 Ethernet global interrupt。

当前 Example：

```text
ETH_IRQn Preemption Priority = 5
Sub Priority                 = 0
```

`5` 只是当前参考工程参数，不是 Driver 固定要求。只要 ISR 路径最终调用 FreeRTOS/CMSIS-RTOS2 ISR-safe API，就必须核对目标工程：

```text
configPRIO_BITS
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
ETH_IRQn priority
```

当前 Example：

```text
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5
ETH_IRQn                                      = 5
```

### 3.5 FreeRTOS / CMSIS-RTOS2 Task

Driver Package **不创建 Task**，也不决定 Task 的：

```text
priority
stack size
static / dynamic allocation
lifetime
```

应用只需创建一个 Task，并让其最终执行：

```c
EthernetRtos_RuntimeTask(void *argument);
```

当前 Reference Example 已验证的 CubeMX 集成方式是 **As weak**：

```text
Task Entry : EthernetRtos_RuntimeTask
Generation : As weak
```

CubeMX 负责生成 Task Object、priority、stack、allocation 和 `osThreadNew()`，generated `freertos.c` 提供 weak stub；Package 在 `Ethernet/RTOS/CMSIS_RTOS2/Src/ethernet_rtos.c` 中提供同名强定义实现。该方式已完成 CubeMX Generate Code、Debug/Release Build、map/ELF 和 On-board async RX 回归。

非 CubeMX 用户可以直接使用 CMSIS-RTOS2 / FreeRTOS API 创建任务，入口仍指向 `EthernetRtos_RxTask()`。

## 4. DMA 内存设计

### 4.1 普通 static 数组的局限

例如：

```c
static uint8_t rx_buffer[1536];
```

只说明对象有静态存储期，并不说明它最终位于哪块 SRAM。STM32H7 不同 SRAM 位于不同 Bus / Domain；某些 RAM CPU 可访问，但 Ethernet DMA 不可访问。

因此必须同时确认：

```text
目标 SRAM 物理地址
Ethernet DMA Master 可达性
SRAM clock
Descriptor 地址与大小
RX/TX Buffer 地址与大小
32-byte alignment
MPU Memory Attribute
D-Cache 策略
CPU / DMA ownership
linker section
.map / ELF 实际结果
```

### 4.2 当前 STM32H743 Example

参考工程将 SRAM3 独立为 Ethernet DMA 区域：

```text
RAM_ETH
0x30040000 ~ 0x30047FFF
32 KiB
```

原 D2 RAM 对普通 section 只保留：

```text
RAM_D2
0x30000000 ~ 0x3003FFFF
256 KiB
```

当前布局：

```text
0x30040000  RX Descriptor，4 × 24 B，实际 96 B，预留 128 B
0x30040080  TX Descriptor，4 × 24 B，实际 96 B，预留 128 B
0x30042000  RX Pool，4 × 1536 B = 6144 B = 0x1800
0x30044000  TX Pool，4 × 1536 B = 6144 B = 0x1800
```

这些地址只属于当前 STM32H743 Example。更换 STM32H7 型号时必须重新查对应 Reference Manual，重新确认 DMA 可达 SRAM。

### 4.3 SRAM 时钟

当前板在 `MX_ETH_Init()` 前调用：

```c
EthernetPort_PrepareDmaMemory();
```

其 Example 实现会使能 D2 SRAM3 时钟。目标 MCU 如果不需要额外 SRAM clock，该 Port 函数可以为空，但不要把型号判断塞进通用 Driver。

## 5. Linker 配置

Driver 只定义 input section 名，目标工程决定物理地址。具体修改位置位于.`ld`文件中，示例中使用的链接文件为`example/STM32H743xx_FLASH.ld` ，若使用相同芯片且无其他内存配置，可直接复制使用。接下来就其中内容进行讲解。

### 5.1 MEMORY

当前 STM32H743 Example：

```ld
MEMORY
{
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM     (xrw) : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 256K
  RAM_ETH (xrw) : ORIGIN = 0x30040000, LENGTH = 32K
  RAM_D3  (xrw) : ORIGIN = 0x38000000, LENGTH = 64K
  ITCMRAM (xrw) : ORIGIN = 0x00000000, LENGTH = 64K
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
}
```

注意 `RAM_D2` 和 `RAM_ETH` 不能重叠，否则普通 `.bss` 仍可能侵入 DMA 专用区域。

### 5.2 Descriptor section

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

### 5.3 RX / TX payload Pool

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

当前 Pool 数量和大小由 Driver 基线决定：

```text
RX = 4 × 1536 B = 0x1800
TX = 4 × 1536 B = 0x1800
Alignment = 32 B
```

修改 Descriptor Count 或 Frame Buffer Size 后，必须同时重新设计 linker 预留和断言。

### 5.4 建议使用 linker ASSERT

当前 Example 至少检查：

```ld
ASSERT(ADDR(.RxDescripSection) == 0x30040000,
       "Ethernet RX descriptor address mismatch")
ASSERT(SIZEOF(.RxDescripSection) <= 0x80,
       "Ethernet RX descriptors exceed reserved slot")

ASSERT(ADDR(.TxDescripSection) == 0x30040080,
       "Ethernet TX descriptor address mismatch")
ASSERT(SIZEOF(.TxDescripSection) <= 0x80,
       "Ethernet TX descriptors exceed reserved slot")

ASSERT(ADDR(.eth_dma_rx) == 0x30042000,
       "Ethernet RX buffer address mismatch")
ASSERT(SIZEOF(.eth_dma_rx) == 0x1800,
       "Ethernet RX buffer pool size mismatch")

ASSERT(ADDR(.eth_dma_tx) == 0x30044000,
       "Ethernet TX buffer address mismatch")
ASSERT(SIZEOF(.eth_dma_tx) == 0x1800,
       "Ethernet TX buffer pool size mismatch")
```

建议额外保留 section 非空、区域边界和对齐检查。linker 配置属于板级硬件事实，应保持显式、可审查；本项目不使用 CubeMX Memory Management Tool 自动接管 Ethernet DMA section。

### 5.5 构建后检查 `.map` / ELF

成功链接不等于 DMA 布局正确。至少检查：

```bash
grep -E "RxDescripSection|TxDescripSection|eth_dma_rx|eth_dma_tx|DMARxDscrTab|DMATxDscrTab" \
  build/Debug/stm32H7ethernet_demo.map

arm-none-eabi-nm -n build/Debug/stm32H7ethernet_demo.elf | \
  grep -E "DMARxDscrTab|DMATxDscrTab"
```

当前 Example 已验证目标布局为：

```text
DMARxDscrTab = 0x30040000
DMATxDscrTab = 0x30040080
.eth_dma_rx  = 0x30042000 / 0x1800
.eth_dma_tx  = 0x30044000 / 0x1800
```

## 6. 实现 Ethernet Port

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

核心形式：

```c
#include "ethernet_port.h"

#include "eth.h"
#include "main.h"

ETH_HandleTypeDef *EthernetPort_GetHandle(void)
{
    return &heth;
}

void EthernetPort_PrepareDmaMemory(void)
{
    __HAL_RCC_D2SRAM3_CLK_ENABLE();
}

void EthernetPort_PhyResetAssert(void)
{
    HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_RESET);
}

void EthernetPort_PhyResetRelease(void)
{
    HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_SET);
}
```

通用 `ethernet_driver.c` / `ethernet_mdio.c` 不应 include 某个 Demo 的 `eth.h`，也不应假设 HAL Handle 一定叫 `heth`。

## 7. 将源码加入构建系统

Driver 是普通 C 源码，不依赖本仓库 CMake。根据是否使用 LAN8720 / CMSIS-RTOS2，加入对应文件。

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

目标工程还必须提供自己的 `ethernet_port.c`，并启用 STM32 HAL ETH module。当前实现以 STM32H7 HAL 1.11.6 API 完成验证；迁移到不同 CubeH7/HAL 版本时必须重新核对 Ethernet HAL API，而不是直接假设版本兼容。

## 8. 初始化与 PHY Bring-up

推荐顺序：

```text
MPU_Config()
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

所有等待都应带 timeout；不要用无限阻塞等待 PHY 或网络对端。

LAN8720 当前主要 API：

```c
Lan8720_IsReady(phy_address);
Lan8720_RestartAutoNegotiation(phy_address);
Lan8720_GetStatus(phy_address, &status);
```

PHY Driver 不依赖 FreeRTOS 或 LwIP。

## 9. Driver Frame API

```c
void EthernetDriver_Init(void);

bool EthernetDriver_ConfigureLink(
    EthernetLinkSpeed speed,
    EthernetDuplexMode duplex);

bool EthernetDriver_Start(void);

bool EthernetDriver_Transmit(
    const uint8_t *frame,
    uint16_t length,
    uint32_t timeout_ms);

EthernetRxResult EthernetDriver_Receive(
    uint8_t *frame,
    uint16_t capacity,
    uint16_t *length);
```

当前 `EthernetDriver_Start()` 使用 interrupt-mode start；TX 仍是 polling `HAL_ETH_Transmit()`，异步 TX completion ownership 尚未实现。

## 10. CMSIS-RTOS2 异步 RX

当前已验证链路：

```text
ETH IRQ
→ HAL_ETH_IRQHandler()
→ HAL_ETH_RxCpltCallback()
→ Driver RX event
→ osThreadFlagsSet()
→ EthernetRtos_RxTask()
→ EthernetDriver_Receive()
→ RX Frame Handler
```

Adapter 不在 ISR 中读取 Frame。任务每次被唤醒后必须持续 drain：

```text
EthernetDriver_Receive()
→ ...
→ ETHERNET_RX_NONE
```

因为 Thread Flag 是事件位，不是 Packet Counter，多次 IRQ 可以合并成一次任务唤醒。

应用注册任务上下文 Handler：

```c
EthernetRtos_SetRxFrameHandler(MyRxHandler, my_context);
```

Handler：

```c
void MyRxHandler(const uint8_t *frame, uint16_t length, void *context);
```

`frame` 只在 Handler 调用期间有效；Handler 返回后不得继续持有该指针。如上层需要长期保存，必须自行复制。

ISR / HAL RX complete callback 中禁止执行协议解析、应用业务、`printf`、长循环或阻塞操作。

## 11. Reference Example

完整参考工程：

```text
examples/STM32H743_LAN8720_FreeRTOS/
```

包含：

```text
STM32CubeMX .ioc
Core
CMSIS / STM32H7 HAL
FreeRTOS
当前板 BSP / Port
linker
CMake / toolchain
build / flash scripts
```

该 Example 使用 CMake + Ninja + GNU Arm Embedded Toolchain，仅作为一种构建方式。使用 STM32CubeIDE、Keil、IAR、Makefile 或其他工具链时，只需把 Driver 源码、include path、linker/MPU 和 Port 正确接入即可。

Example 的构建、烧录和测试方法见其 [`README.md`](examples/STM32H743_LAN8720_FreeRTOS/README.md)。

## 13. 当前验证状态

已验证：

```text
LAN8720 Reset / MDIO / PHY ID
Auto-negotiation
Link Up / Down
100 Mbit/s Full Duplex
STM32H7 MAC Speed / Duplex 同步
DMA Descriptor / RX/TX Pool 布局
Raw Ethernet TX
Raw Ethernet RX
Polling RX 1000 / 1000
ETH IRQ + CMSIS-RTOS2 async RX 1000 / 1000
Reference Example 新路径 Debug / Release / map / On-board 回归
CubeMX As weak Task Entry + Package strong implementation
```

当前未完成：

```text
Async TX completion ownership
DMA / MAC error recovery
完整 Link Down / Up MAC lifecycle
D-Cache-on 专项验证
LwIP / Ping
UDP Echo
TCP Echo
高负载 / 长时间压力测试
```

详细架构、内存和 RTOS 设计见 `docs/stm32h7_ethernet_project_docs/`；运行时 callback / ownership 原理见 [`docs/ETHERNET_RUNTIME_FLOW.md`](docs/ETHERNET_RUNTIME_FLOW.md)。