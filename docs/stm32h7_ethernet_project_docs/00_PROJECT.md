# STM32H7 Ethernet 通用驱动项目

- 状态：Active / M2 进行中
- 仓库定位：STM32H7 Ethernet Driver Package + Reference Example
- 第一验证平台：STM32H743VIT6
- 第一验证 PHY：LAN8720AI
- 接口：RMII
- 软件环境：STM32 HAL + FreeRTOS + LwIP

## 1. 项目目标

开发可在不同 STM32H7 控制板复用的 Ethernet 基础组件。仓库产品是：

```text
Ethernet/
```

目标用户复制该目录后，通过目标工程自己的 CubeMX / linker / MPU / Port 完成集成。完整验证工程是 Reference Example：

```text
examples/STM32H743_LAN8720_FreeRTOS/
```

第一版覆盖：PHY、STM32H7 MAC/DMA、Descriptor/Buffer ownership、MPU/Cache/linker、FreeRTOS 异步 RX/TX、LwIP ethernetif、Static IPv4、Ping、UDP Echo、TCP Echo、错误与压力测试。

机器人 HostLink、机器人协议和控制业务不属于本 Driver。

## 2. 稳定分层

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

约束：Driver Core 不依赖 Demo `eth.h` / `heth`；PHY 不依赖 FreeRTOS/LwIP；Port 只处理目标板绑定；DMA/MPU/linker 显式设计；RTOS Adapter 不隐藏 Task 创建。

## 3. 仓库结构

```text
Ethernet/                                  ← Driver Package
examples/STM32H743_LAN8720_FreeRTOS/       ← 完整参考 Demo
README.md                                  ← Driver Integration Guide
docs/ETHERNET_RUNTIME_FLOW.md              ← RX/TX Runtime / callback / ownership 原理说明
docs/stm32h7_ethernet_project_docs/        ← 技术与项目状态文档
```

Reference Example 内部包含 CubeMX `.ioc`、Core、CMSIS/HAL、FreeRTOS、BSP/Port、linker、CMake 和脚本。根目录不再把这些 Demo 文件与 Driver Package 平铺。

## 4. 里程碑

### M0：项目基线

已完成 CubeMX/FreeRTOS/调试输出/基础上板环境。

### M1：PHY Bring-up

已上板验证：PHY Reset、MDIO Read/Write、PHY ID/Address/Strap、Auto-negotiation、Link Up/Down、100 Mbit/s、Full Duplex、单次网线拔插状态恢复。

### M2：MAC / DMA

已完成并验证：

- STM32H743 DMA SRAM / MPU / linker；
- RX/TX Descriptor 与静态 Buffer Pool；
- copy-based RX ownership；
- Raw TX / RX；
- polling RX 1000 / 1000；
- ETH IRQ；
- `HAL_ETH_Start_IT()`；
- CMSIS-RTOS2 Thread Flag；
- async RX 1000 / 1000；
- Driver Package Port / RTOS Adapter 重构；
- 完整 Reference Example 移入 `examples/` 后重新 Build / map / On-board 回归；
- CubeMX `EthernetRuntime + EthernetRtos_RuntimeTask + As weak` 集成；
- copy-based async TX；
- `HAL_ETH_Transmit_IT()` → TX complete event → Runtime Task → `HAL_ETH_ReleaseTxPacket()` → `HAL_ETH_TxFreeCallback()`；
- async TX 1000-frame 连续提交与 TX Buffer recycle 上板验证；
- RuntimeTask 改名后的 async RX 1000 / 1000 回归。

仍未完成：RX/TX error/drop 统计、DMA/MAC error recovery、完整 Link lifecycle、Task stack high-water mark、长时间/高负载、D-Cache-on 专项验证。

### M3 ～ M6

LwIP / Ping / UDP / TCP / Stress 尚未进入。

## 5. 当前 Runtime 形态

Reference Example 当前 CubeMX Task：

```text
Task Name  : EthernetRuntime
Entry      : EthernetRtos_RuntimeTask
Generation : As weak
```

CubeMX 管理 Task object / priority / stack / allocation / `osThreadNew()`；Package 提供同名强定义，并在同一 Runtime Task 中处理：

```text
RX complete event
→ drain EthernetDriver_Receive()

TX complete event
→ EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
```

完整原理见 `docs/ETHERNET_RUNTIME_FLOW.md`。

## 6. 当前产品化状态

已完成：

- `Ethernet/` 为仓库根目录产品；
- 完整 STM32H743 Reference Example 位于 `examples/STM32H743_LAN8720_FreeRTOS/`；
- Example CMake 通过 `../../Ethernet` 引用共享 Package；
- 根 README 作为完整集成指南；
- 原 `docs/BOARD_PORTING.md` 的迁移职责合并到 README，删除重复文档；
- CubeMX Task 使用 `EthernetRtos_RuntimeTask + As weak`，CubeMX 管 Task 资源，Package 提供强定义实现；
- `docs/ETHERNET_RUNTIME_FLOW.md` 统一解释 weak symbol、HAL callback、运行时 Handler、IRQ/Task、RX/TX Buffer ownership。

## 7. 多对话状态管理

项目真实状态以远程 `main` 为准。默认先读取：

1. `00_PROJECT.md`
2. `01_ARCHITECTURE.md`
3. `02_HARDWARE_BASELINE.md`
4. `06_DECISIONS.md`
5. `07_STATUS.md`
6. `08_HANDOFF.md`
7. 当前任务所需代码、`.ioc`、linker、HAL、Datasheet / Reference Manual

## 8. 文档边界

面向使用者：根 `README.md` 是首要 Integration Guide，可独立完成关键 CubeMX / DMA / MPU / linker / Port / RTOS 接入。

专题文档继续提供更深的架构、硬件、内存、RTOS 和 Runtime 原理说明；`05_TEST_PLAN.md`、`06_DECISIONS.md`、`07_STATUS.md`、`08_HANDOFF.md` 属于项目控制文档。

## 9. CubeMX / 手工代码边界

Reference Example 中 `.ioc`、`Core/**`、ST Drivers、FreeRTOS、`cmake/stm32cubemx/CMakeLists.txt` 由 CubeMX/ST 管理；Core 手工逻辑只放 USER CODE 区域。

通用手工维护代码位于 `Ethernet/**`。当前板 `ethernet_port.c` 位于 Example BSP。板级 linker 显式维护 DMA layout，不使用 CubeMX Memory Management Tool 自动接管 Ethernet section。
