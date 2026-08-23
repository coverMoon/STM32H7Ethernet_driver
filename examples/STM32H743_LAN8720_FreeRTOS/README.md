# STM32H743 + LAN8720 FreeRTOS Reference Example

本目录是 `Ethernet/` Driver Package 的完整参考工程，不是 Driver Package 本身。

验证硬件：

```text
MCU       : STM32H743VIT6
PHY       : LAN8720AI
Interface : RMII
RTOS      : FreeRTOS / CMSIS-RTOS2
Debug UART: USART1, PA9/PA10, 115200 8N1
```

当前工具版本：

```text
STM32CubeMX : 6.18.1
STM32CubeH7 : V1.13.0
HAL ETH     : 1.11.6
```

## 1. 目录关系

```text
repository root
├── Ethernet/                         ← 可复用 Driver Package
└── examples/
    └── STM32H743_LAN8720_FreeRTOS/   ← 本参考工程
        ├── BSP/
        ├── Core/
        ├── Drivers/
        ├── Middlewares/
        ├── cmake/
        ├── CMakeLists.txt
        ├── CMakePresets.json
        ├── STM32H743xx_FLASH.ld
        ├── stm32H7ethernet_demo.ioc
        ├── startup_stm32h743xx.s
        ├── build.sh
        └── flash.sh
```

Example 的顶层 `CMakeLists.txt` 通过 `../../Ethernet` 引用仓库根目录 Driver Package，因此不要把一份 Driver 副本复制进 Example。

## 2. CubeMX / 手工代码边界

CubeMX / ST 管理：

```text
stm32H7ethernet_demo.ioc
Core/**
Drivers/CMSIS/**
Drivers/STM32H7xx_HAL_Driver/**
Middlewares/Third_Party/FreeRTOS/**
cmake/stm32cubemx/CMakeLists.txt
startup / system 等生成内容
```

对 `Core/**` 的长期手工逻辑只放在 `USER CODE BEGIN / END` 区域。

不要手工修改：

```text
cmake/stm32cubemx/CMakeLists.txt
```

长期维护的 Driver 源码在仓库根目录 `Ethernet/`；当前板级 Port 位于：

```text
BSP/stm32h743vit6_iot/ethernet_port.c
```

## 3. 当前 Ethernet DMA 内存

```text
RAM_ETH      = 0x30040000 / 32 KiB
RX Desc      = 0x30040000
TX Desc      = 0x30040080
RX Pool      = 0x30042000 / 0x1800 / 4 × 1536 B
TX Pool      = 0x30044000 / 0x1800 / 4 × 1536 B
```

MPU：

```text
SRAM3 whole region : Normal Non-cacheable
first 256 B        : Device overlay for descriptors
```

当前 I-Cache / D-Cache 均关闭。

`STM32H743xx_FLASH.ld` 是本 Example 的板级 linker，不属于 Driver Package 固定配置。

## 4. 构建

本 Example 使用 CMake + Ninja + GNU Arm Embedded Toolchain。其他 Driver 用户不需要采用相同工具链。

在本目录执行：

```bash
./build.sh Debug --fresh
./build.sh Release --fresh
```

或者使用 CMake Preset：

```bash
cmake --preset Debug
cmake --build --preset Debug
```

## 5. 检查 DMA map

Debug build 后：

```bash
grep -E "RxDescripSection|TxDescripSection|eth_dma_rx|eth_dma_tx|DMARxDscrTab|DMATxDscrTab" \
  build/Debug/stm32H7ethernet_demo.map
```

也可检查 ELF：

```bash
arm-none-eabi-nm -n build/Debug/stm32H7ethernet_demo.elf | \
  grep -E "DMARxDscrTab|DMATxDscrTab"
```

预期：

```text
DMARxDscrTab = 0x30040000
DMATxDscrTab = 0x30040080
.eth_dma_rx  = 0x30042000 / 0x1800
.eth_dma_tx  = 0x30044000 / 0x1800
```

不要只以“成功编译”替代 map / ELF 地址检查。

## 6. 烧录

构建完成后：

```bash
./flash.sh Debug
```

脚本会优先使用 `STM32_Programmer_CLI`，也支持 `st-flash`。可以先只检查环境和固件：

```bash
./flash.sh Debug --check
```

## 7. FreeRTOS Runtime Task

当前 CubeMX 配置已经冻结并验证：

```text
Task Name  : EthernetRuntime
Priority   : osPriorityAboveNormal
Stack      : 256 Words
Entry      : EthernetRtos_RuntimeTask
Generation : As weak
Allocation : Dynamic
```

CubeMX 负责：

```text
Task object
priority
stack
allocation
osThreadNew()
weak Task stub
```

Package 中：

```text
Ethernet/RTOS/CMSIS_RTOS2/Src/ethernet_rtos.c
```

提供同名强定义 `EthernetRtos_RuntimeTask()`，最终负责：

```text
RX deferred processing
TX completion reclaim
```

Driver Package 本身仍不创建 Task。

## 8. 正常启动日志

基础启动参考输出：

```text
[ETH] BootstrapTask started
[ETH] PHY ready
[ETH] Auto-negotiation started
[ETH] Link up
[ETH] Speed=100M
[ETH] Duplex=Full
[ETH] MAC/DMA started
```

Runtime Task 自身不在高速路径打印启动日志。

## 9. Async RX 测试

当前 Demo 使用 EtherType `0x88B5` 做 Raw Frame RX 验证。

PC 连续发送 1000 帧时，已验证：

```text
[ETH] Async RX test 1000/1000 PASS, total=1000
```

当前 RX 路径：

```text
ETH IRQ
→ HAL_ETH_RxCpltCallback()
→ RX Thread Flag
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_Receive()
→ RX Buffer recycle
→ EthernetDemo_RxFrameHandler()
```

该测试验证基础连续 ownership，不是吞吐极限或长时间 Stress Test。

## 10. Async TX 测试

当前 Demo 默认启用：

```text
ETHERNET_ASYNC_TX_TEST_ENABLE = 1
ETHERNET_TX_TEST_TARGET_COUNT = 1000
```

MAC/DMA 启动成功后，BootstrapTask 连续调用：

```c
EthernetDriver_TransmitAsync();
```

测试 Frame：

```text
Destination MAC : ff:ff:ff:ff:ff:ff
EtherType       : 0x88B5
Frame Length    : 60 B
尾部 4 B        : 发送序号
```

临时没有 TX Buffer / Descriptor 时：

```text
ETHERNET_TX_RETRY
→ osDelay(1)
→ retry
```

每 100 帧输出一次：

```text
[ETH] Async TX queued=100
...
[ETH] Async TX queued=1000
[ETH] Async TX queued 1000/1000
```

当前 TX completion 路径：

```text
HAL_ETH_Transmit_IT()
→ TX IRQ
→ HAL_ETH_TxCpltCallback()
→ TX Thread Flag
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ TX Buffer recycle
```

该 1000-frame 测试已上板通过，并且完成后重新执行 async RX 1000 / 1000 回归也通过。

## 11. 当前验证等级

已确认：

```text
Static Review
Debug Build
Release Build
map / ELF DMA layout
PHY Reset / MDIO / ID / Address
Auto-negotiation
Link Up / Down
100M Full Duplex
Raw TX / RX
Polling RX 1000 / 1000
Async RX 1000 / 1000
EthernetRuntime / As weak Task integration
Async TX 1000-frame completion recycle
RuntimeTask 改名后的 RX 1000 / 1000 regression
```

未完成：

```text
RX/TX error/drop 统计
DMA fatal / timeout recovery
Link lifecycle
Task stack high-water mark
D-Cache-on
高负载 / 长时间 Stress
LwIP / Ping / UDP / TCP
```

运行时 callback、Thread Flag、RX/TX Buffer ownership 的完整原理说明见仓库根目录：

```text
docs/ETHERNET_RUNTIME_FLOW.md
```
