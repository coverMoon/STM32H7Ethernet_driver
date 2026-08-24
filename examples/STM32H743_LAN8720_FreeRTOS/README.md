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

Example 顶层 `CMakeLists.txt` 通过 `../../Ethernet` 引用仓库根目录 Driver Package。

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

对 `Core/**` 的长期手工逻辑只放在 `USER CODE BEGIN / END` 区域。不要手工修改：

```text
cmake/stm32cubemx/CMakeLists.txt
```

长期维护的 Driver 源码在仓库根目录 `Ethernet/`；当前板级 Port 位于：

```text
BSP/stm32h743vit6_iot/ethernet_port.c
```

## 3. 当前 Ethernet DMA / Cache 基线

```text
RAM_ETH      = 0x30040000 / 32 KiB
RX Desc      = 0x30040000 / 8 × 24 B
TX Desc      = 0x30040100 / 4 × 24 B
RX Pool      = 0x30042000 / 0x3000 / 8 × 1536 B
TX Pool      = 0x30045000 / 0x1800 / 4 × 1536 B
```

MPU：

```text
SRAM3 whole region            : Normal Non-cacheable
0x30040000 ~ 0x300401FF       : 512 B Device overlay for descriptors
```

CPU Cache：

```text
I-Cache = Enabled
D-Cache = Disabled
```

`STM32H743xx_FLASH.ld` 是本 Example 的板级 linker，不属于 Driver Package 固定配置。

## 4. 构建

本 Example 使用 CMake + Ninja + GNU Arm Embedded Toolchain。其他 Driver 用户不需要采用相同工具链。

在本目录执行：

```bash
./build.sh Debug --fresh
./build.sh Release --fresh
```

或者：

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
DMATxDscrTab = 0x30040100
.eth_dma_rx  = 0x30042000 / 0x3000
.eth_dma_tx  = 0x30045000 / 0x1800
```

不要只以“成功编译”替代 map / ELF 地址检查。

## 6. 烧录

构建完成后：

```bash
./flash.sh Debug
```

脚本会优先使用 `STM32_Programmer_CLI`，也支持 `st-flash`。可以先检查环境和固件：

```bash
./flash.sh Debug --check
```

## 7. FreeRTOS Runtime Task

当前 CubeMX 配置：

```text
Task Name  : EthernetRuntime
Priority   : osPriorityAboveNormal
Stack      : 256 Words
Entry      : EthernetRtos_RuntimeTask
Generation : As weak
Allocation : Dynamic
```

CubeMX 负责 Task object、priority、stack、allocation、`osThreadNew()` 和 weak Task stub；Package 中：

```text
Ethernet/RTOS/CMSIS_RTOS2/Src/ethernet_rtos.c
```

提供同名强定义 `EthernetRtos_RuntimeTask()`，负责：

```text
RX deferred processing
TX completion reclaim
```

Driver Package 本身不创建 Task。

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

Runtime Task 自身不在高速路径打印日志。

## 9. Async RX 路径

当前 Demo 使用 EtherType `0x88B5` 做 Raw Frame RX 验证。

```text
ETH IRQ
→ HAL_ETH_RxCpltCallback()
→ RX Thread Flag
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_Receive()
→ RX Buffer recycle
→ EthernetDemo_RxFrameHandler()
```

基础 async RX 1000 / 1000 已通过。

## 10. Async TX 路径

当前 Demo 已验证 1000-frame async TX completion ownership：

```text
EthernetDriver_TransmitAsync()
→ HAL_ETH_Transmit_IT()
→ TX IRQ
→ HAL_ETH_TxCpltCallback()
→ TX Thread Flag
→ EthernetRtos_RuntimeTask()
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ TX Buffer recycle
```

临时没有 TX Buffer / Descriptor 时返回 `ETHERNET_TX_RETRY`。当前没有软件 TX Queue。

该 1000-frame 测试证明 completion ownership / Buffer recycle 正确，不代表 TX throughput 性能。

## 11. Driver stats

`EthernetDriver_GetStats()` 当前提供：

```text
rx_frames / rx_errors / rx_dropped / rx_buffer_unavailable
tx_queued / tx_retries / tx_errors / tx_completed
hal_error_events
last_hal_error_code / last_dma_error_code / last_mac_error_code
```

高负载测试可从任务上下文打印 `[ETH][STAT]`，不要在 ETH ISR 中 `printf`。

## 12. RX Stress / Performance

PC 侧脚本：

```text
test/send_eth_stress.py
```

示例：

```bash
sudo python3 test/send_eth_stress.py enp8s0 -n 200000 --size 60 --pps 120000
sudo python3 test/send_eth_stress.py enp8s0 -n 200000 --size 1514 --pps 8000
```

当前 clean baseline 使用 I-Cache Enabled、D-Cache Disabled、RX8/TX4，无临时 DWT/linker-wrap profiler。

### 60 B

```text
110 kpps × 200000 → 200000 / 200000, HAL=0
120 kpps × 1M     → 1000000 / 1000000, HAL=205
130 kpps × 200000 → 200000 / 200000, HAL=52777
140 kpps × 200000 → 186539 / 200000
148.59 kpps       → 175866 / 200000
```

当前饱和接收平台约 `130.6 kpps`，约为 100BASE-TX 最小帧理论 `148.8 kpps` 的 `87.8%`。

### 1514 B

```text
8000 pps × 200000
→ 200000 / 200000
→ HAL/DMA error = 0
→ script frame rate = 96.90 Mbit/s
→ estimated on-wire ≈ 98.43 Mbit/s
```

I-Cache Disabled 时历史 60 B clean saturation 约 `71.1 kpps`；Enabled 后约 `130.6 kpps`。因此 I-Cache Enabled 是当前 Reference Example 正式配置。

当前尚未建立 TX throughput 性能基线。

## 13. 当前验证等级

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
Driver stats
I-Cache Enabled
RX 60 B high-load baseline
RX 1514 B near-line-rate baseline
```

未完成：

```text
DMA fatal / RBU / timeout recovery
Link lifecycle
Task stack high-water mark
D-Cache-on
真正长时间 Stress
TX throughput baseline
LwIP / Ping / UDP / TCP
```

运行时 callback、Thread Flag、RX/TX Buffer ownership 的完整原理说明见：

```text
docs/ETHERNET_RUNTIME_FLOW.md
```
