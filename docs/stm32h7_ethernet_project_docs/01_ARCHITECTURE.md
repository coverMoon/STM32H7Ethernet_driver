# STM32H7 Ethernet Architecture

本文描述 STM32H7 Ethernet Driver Package 的稳定分层、依赖方向和模块职责。当前验证硬件为 STM32H743VIT6 + LAN8720AI + RMII；Reference Example 位于 `examples/STM32H743_LAN8720_FreeRTOS/`。

运行时 callback、weak symbol、IRQ/Task 交接以及 RX/TX Buffer ownership 的详细原理说明见：[`docs/ETHERNET_RUNTIME_FLOW.md`](../ETHERNET_RUNTIME_FLOW.md)。

## 1. 总体分层

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

只允许稳定的自上而下依赖。Application 不直接操作 HAL ETH；Driver 不处理 IP/UDP/TCP/机器人业务；PHY 不依赖 RTOS/LwIP；Port 不处理 Frame 或协议。

## 2. Driver Package

根目录：

```text
Ethernet/
├── Inc/
├── Src/
├── PHY/
├── Port/
└── RTOS/
```

这是用户迁移到另一工程时复制的产品目录。STM32CubeMX 生成代码、HAL、FreeRTOS、linker、CMake、BSP Example 都不放入 Package。

## 3. Ethernet Port

Port 解决通用 Driver 与目标工程之间必须存在的板级绑定：

```c
ETH_HandleTypeDef *EthernetPort_GetHandle(void);
void EthernetPort_PrepareDmaMemory(void);
void EthernetPort_PhyResetAssert(void);
void EthernetPort_PhyResetRelease(void);
```

通用 Driver 不 include Demo `eth.h`，也不假设 Handle 名为 `heth`。当前参考板实现位于：

```text
examples/STM32H743_LAN8720_FreeRTOS/BSP/stm32h743vit6_iot/ethernet_port.c
```

当前实现绑定 CubeMX `heth`、PC0 PHY reset 和 D2 SRAM3 clock。

## 4. MDIO / PHY

MDIO Wrapper 封装当前 STM32H7 HAL 1.11.6 PHY Management API：

```text
EthernetMdio_Read()
EthernetMdio_Write()
```

LAN8720 Driver：

```text
Lan8720_IsReady()
Lan8720_RestartAutoNegotiation()
Lan8720_GetStatus()
```

PHY Driver 只通过 MDIO Wrapper 访问 PHY，不依赖 RTOS/LwIP；Reset 后等待、polling 和 timeout 由调用层管理。

## 5. MAC / DMA Driver

当前 Frame API：

```c
void EthernetDriver_Init(void);
void EthernetDriver_SetRxEventHandler(EthernetDriverRxEventHandler handler, void *context);
void EthernetDriver_SetTxEventHandler(EthernetDriverTxEventHandler handler, void *context);
bool EthernetDriver_ConfigureLink(EthernetLinkSpeed speed, EthernetDuplexMode duplex);
bool EthernetDriver_Start(void);
EthernetTxResult EthernetDriver_TransmitAsync(const uint8_t *frame, uint16_t length);
void EthernetDriver_ProcessTxCompletions(void);
EthernetRxResult EthernetDriver_Receive(uint8_t *frame, uint16_t capacity, uint16_t *length);
bool EthernetDriver_GetStats(EthernetDriverStats *stats);
```

Driver Core 不依赖 FreeRTOS。HAL RX/TX complete callback 只向上转发 ISR event，不在 ISR 中执行 Frame 读取、Descriptor reclaim、协议解析或应用业务。

当前 Reference Example 使用：

```text
RX Descriptor = 8
TX Descriptor = 4
RX Pool       = 8 × 1536 B
TX Pool       = 4 × 1536 B
Alignment     = 32 B
```

### RX ownership

```text
RX DMA Buffer
→ HAL_ETH_RxLinkCallback()
→ copy 到 Driver CPU 单帧暂存
→ 立即归还 RX Pool
→ EthernetDriver_Receive() 再 copy 给调用者
```

第一版保持 copy-first，不主动追求 Zero Copy。

### TX ownership

```text
caller frame
→ copy 到 Driver TX DMA Buffer
→ HAL_ETH_Transmit_IT()
→ DMA owns Buffer
→ TX complete event
→ Runtime Task
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ Buffer 回到 TX Pool
```

`EthernetDriver_TransmitAsync()` 返回 `ETHERNET_TX_QUEUED` 后，caller 原始 Frame 即可复用；当前不暴露 per-frame application completion callback。临时资源不足返回 `ETHERNET_TX_RETRY`，Driver 不隐藏软件 TX Queue。

TX submit 与 completion reclaim 会操作同一套 HAL Descriptor bookkeeping，当前 Driver 用短临界区序列化 `HAL_ETH_Transmit_IT()` / `HAL_ETH_ReleaseTxPacket()` 及 TX Pool ownership。

## 6. CMSIS-RTOS2 Adapter

可选目录：

```text
Ethernet/RTOS/CMSIS_RTOS2/
```

Adapter 不创建 Task。应用/CubeMX 管理 Task object、priority、stack、allocation；Package 提供：

```text
EthernetRtos_SetRxFrameHandler()
EthernetRtos_IsReady()
EthernetRtos_RuntimeTask()
```

Runtime Task 启动时自动绑定 Driver RX/TX event。运行链路：

```text
RX:
ETH IRQ
→ HAL_ETH_RxCpltCallback()
→ Driver RX event
→ RX Thread Flag
→ EthernetRtos_RuntimeTask()
→ drain EthernetDriver_Receive() until ETHERNET_RX_NONE
→ synchronous Frame Handler

TX:
HAL_ETH_Transmit_IT()
→ TX IRQ
→ HAL_ETH_TxCpltCallback()
→ Driver TX event
→ TX Thread Flag
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
```

Frame Handler 在任务上下文执行，frame pointer 仅在 Handler 调用期间有效。

当前 Reference Example 已采用并验证：

```text
Task Name  : EthernetRuntime
Task Entry : EthernetRtos_RuntimeTask
Generation : As weak
```

CubeMX 管理 Task attributes / `osThreadNew()`，Package 提供同名强定义 Task Entry。

## 7. Runtime Handler 边界

当前运行时函数指针注册：

```text
EthernetDriver_SetRxEventHandler()
→ Driver → RTOS Adapter

EthernetDriver_SetTxEventHandler()
→ Driver → RTOS Adapter

EthernetRtos_SetRxFrameHandler()
→ RTOS Adapter → Application / ethernetif
```

前两项由 `EthernetRtos_RuntimeTask()` 内部自动完成；普通用户通常只需要设置 RX Frame Handler 并调用 async TX API。

HAL 的 `HAL_ETH_RxCpltCallback()`、`HAL_ETH_RxLinkCallback()`、`HAL_ETH_RxAllocateCallback()`、`HAL_ETH_TxCpltCallback()`、`HAL_ETH_TxFreeCallback()` 属于 HAL 固定 callback，不要与上述运行时 Handler 注册混为一类。

## 8. ethernetif / LwIP

未来 `ethernetif` 位于 LwIP 与 Frame API 之间：

```text
LwIP pbuf
↕
ethernetif
↕
Ethernet Frame API / RTOS runtime
```

当前 ethernetif / LwIP 尚未实现。Driver 不提前包含 pbuf、IP、Socket 或协议语义。

## 9. DMA / MPU / Cache / linker 边界

物理 DMA 地址属于目标工程，不属于 Driver Package。必须显式确定：DMA Master 可达 SRAM、Descriptor / Buffer 地址、32-byte alignment、MPU Memory Attribute、D-Cache 策略、ownership、linker section 和 map/ELF 实际地址。

当前 STM32H743 Example：

```text
RAM_ETH = SRAM3 = 0x30040000 / 32 KiB
RX Desc = 0x30040000 / 8 × 24 B
TX Desc = 0x30040100 / 4 × 24 B
RX Pool = 0x30042000 / 0x3000 / 8 × 1536 B
TX Pool = 0x30045000 / 0x1800 / 4 × 1536 B
```

MPU：整个 SRAM3 为 Normal Non-cacheable；`0x30040000 ~ 0x300401FF` 为 512 B Device overlay，覆盖 Descriptor 区域。

Reference Example 当前：

```text
I-Cache = Enabled
D-Cache = Disabled
```

I-Cache 只影响 CPU 指令取值，不改变 Ethernet DMA data coherence；D-Cache-on 仍需单独设计和验证。详细见 `03_MEMORY_DMA.md`。

## 10. CubeMX 与维护边界

Reference Example 的 CubeMX/ST 管理内容：

```text
examples/STM32H743_LAN8720_FreeRTOS/stm32H7ethernet_demo.ioc
examples/STM32H743_LAN8720_FreeRTOS/Core/**
examples/STM32H743_LAN8720_FreeRTOS/Drivers/**
examples/STM32H743_LAN8720_FreeRTOS/Middlewares/**
examples/STM32H743_LAN8720_FreeRTOS/cmake/stm32cubemx/CMakeLists.txt
```

Core 手工代码只进入 USER CODE；`cmake/stm32cubemx/CMakeLists.txt` 不手工修改。

手工维护产品：`Ethernet/**`。板级 linker 和 Example CMake 属于 Reference Example 配置。

## 11. 可移植性

换板时优先只改变：

```text
目标 CubeMX 配置
目标 ethernet_port.c
板级 linker / MPU / DMA SRAM
PHY Driver（仅 PHY 型号变化时）
Task / RTOS 资源配置
```

不应因为 PCB 差异修改通用 Frame ownership、MDIO Wrapper、RTOS Adapter 或未来 ethernetif。

## 12. 当前验证

已 On-board Verified / Measured：

```text
PHY bring-up
Raw TX/RX
polling RX 1000/1000
async RX 1000/1000
async TX 1000-frame completion recycle
EthernetRuntime + As weak integration
Driver stats / HAL-DMA error snapshot
60 B RX high-load clean baseline
1514 B RX near-line-rate baseline
```

RX clean baseline：60 B 饱和平台约 `130.6 kpps`；1514 B @ 8000 pps、200000 帧零丢包，估算 on-wire 约 `98.43 Mbit/s`。

当前 TX throughput 尚未建立 Measured 性能基线；已有结果只证明 async TX ownership / completion recycle 正确。
