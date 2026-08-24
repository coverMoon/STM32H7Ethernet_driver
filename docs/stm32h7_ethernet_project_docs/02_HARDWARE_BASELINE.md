# Hardware Baseline

本文记录当前 STM32H743VIT6 + LAN8720AI Reference Example 的 Ethernet 硬件事实与板级配置。它是已验证示例，不把 GPIO、SRAM 地址或时钟拓扑当作所有 STM32H7 的固定配置。

## 1. 平台

| 项目 | 当前值 |
| --- | --- |
| MCU | STM32H743VIT6 / LQFP100 |
| PHY | LAN8720AI |
| MAC-PHY | RMII |
| PHY 供电 | 3.3 V |
| PHY 本地时钟 | 25 MHz 晶振 |
| 网络能力 | 10/100 Mbit/s |
| RTOS | FreeRTOS / CMSIS-RTOS2 |

Reference Example：

```text
examples/STM32H743_LAN8720_FreeRTOS/
```

## 2. RMII / SMI 引脚

| MCU 引脚 | 信号 |
| --- | --- |
| PA1 | RMII_REF_CLK |
| PA2 | ETH_MDIO |
| PC1 | ETH_MDC |
| PA7 | RMII_CRS_DV |
| PC4 | RMII_RXD0 |
| PC5 | RMII_RXD1 |
| PB11 | RMII_TX_EN |
| PB12 | RMII_TXD0 |
| PB13 | RMII_TXD1 |
| PC0 | ETH_RESET / PHY nRST |

PHY `nINT/REFCLKO` 接 STM32 PA1 / RMII_REF_CLK。

## 3. PHY 时钟

```text
25 MHz Crystal
→ LAN8720AI XTAL1/XTAL2
→ PHY internal clock generation
→ nINT/REFCLKO
→ STM32 PA1 / RMII_REF_CLK
```

原理图、Datasheet 和实际 Ethernet 功能支持该路径有效。25 MHz 晶振和 RMII REF_CLK 尚未用仪器独立测量，因此不标记为 Measured。

## 4. PHY Reset / MDIO

PC0 控制 LAN8720 nRST。Reset 释放后通过 PHY ID polling + timeout 判断可管理状态。

```text
PA2  ↔ MDIO
PC1  → MDC
```

使用 STM32 Ethernet MAC/HAL Clause 22 Management Interface，不使用 GPIO bit-bang。HAL ETH 版本 1.11.6；MDIO Read/Write 已上板验证。

## 5. PHY Address / Strap / ID

上板结果：

```text
Reg18       = 0x60E0
MODE[2:0]   = 111
PHY Address = 0
PHY ID1     = 0x0007
PHY ID2     = 0xC0F1
```

原理图 + Datasheet 支持：`nINTSEL` 为 Low，对应 REF_CLK Out Mode；`REGOFF` 为 Low，对应内部 regulator 启用。PHY Address/MODE/ID 已 MDIO 实测，strap 电平未独立电气测量。

## 6. Link / Speed / Duplex

On-board Verified：

```text
Link Up / Down
100 Mbit/s
Full Duplex
单次网线拔出 / 插回 Link 状态恢复
```

PHY 协商结果已用于配置 STM32 MAC，并完成 Raw TX/RX、async RX 与高负载 RX 验证。

尚未专项验证：10 Mbit/s、Half Duplex、快速连续插拔、Link/Speed LED。

## 7. 调试串口

```text
USART1
PA9 TX
PA10 RX
115200 / 8N1
```

已上板验证。UART 用于低频 Bring-up/统计输出，不进入 ETH ISR 或高速 Frame 处理逻辑。

## 8. Ethernet DMA 内存与 Cache

当前 STM32H743 Example：

```text
RAM_ETH / SRAM3 = 0x30040000 ~ 0x30047FFF / 32 KiB
RAM_D2          = 0x30000000 ~ 0x3003FFFF / 256 KiB

RX Descriptor   = 0x30040000 / 8 × 24 B
TX Descriptor   = 0x30040100 / 4 × 24 B
RX Pool         = 0x30042000 / 0x3000 / 8 × 1536 B
TX Pool         = 0x30045000 / 0x1800 / 4 × 1536 B
```

MPU：

```text
0x30040000 ~ 0x30047FFF  Normal / Non-cacheable / Shareable
0x30040000 ~ 0x300401FF  Device overlay / 512 B
```

当前 CPU Cache：

```text
I-Cache = Enabled
D-Cache = Disabled
```

I-Cache 已作为 Reference Example 当前基线；D-Cache-on 尚未验证。Ethernet SRAM3 保持 Non-cacheable，因此当前 RX/TX DMA Buffer 不需要 D-Cache Clean/Invalidate。

当前板 Port：

```text
examples/STM32H743_LAN8720_FreeRTOS/BSP/stm32h743vit6_iot/ethernet_port.c
```

其 `EthernetPort_PrepareDmaMemory()` 在 `MX_ETH_Init()` 前使能 D2 SRAM3 clock。

板级 linker：

```text
examples/STM32H743_LAN8720_FreeRTOS/STM32H743xx_FLASH.ld
```

## 9. RX 性能实测基线

测试固件：I-Cache Enabled、D-Cache Disabled、RX8/TX4、clean Runtime，无 DWT/linker-wrap profiling。PC 侧使用 `test/send_eth_stress.py`，EtherType `0x88B5`。

Measured：

```text
60 B @ 110 kpps, 200000 frames
→ 200000 / 200000
→ HAL/RBU events = 0

60 B @ 120 kpps, 1000000 frames
→ 1000000 / 1000000
→ HAL/RBU events = 205

60 B saturation
→ ~130.6 kpps received platform
→ theoretical minimum-frame packet rate ~148.8 kpps
→ ~87.8% of theoretical packet-rate limit

1514 B @ 8000 pps, 200000 frames
→ 200000 / 200000
→ HAL/DMA error = 0
→ script frame rate = 96.90 Mbit/s
→ estimated on-wire rate ≈ 98.43 Mbit/s
```

这些结果只代表 RX。TX 当前仅完成 async 1000-frame completion ownership 验证，尚无 Measured throughput 基线。

## 10. 验证状态

On-board Verified / Measured：PHY Reset/MDIO/ID/Address/Auto-negotiation/Link/100M Full Duplex、USART1、MAC link sync、Raw TX、Raw RX、polling RX、ETH IRQ + CMSIS-RTOS2 async RX、async TX completion ownership、RX 高负载性能。

Build/Map Verified 的当前 DMA layout 以 `.ioc` + linker + map/ELF 为准，地址/大小如第 8 节。

尚未完成：完整 Link lifecycle、DMA fatal/RBU/timeout recovery、Task stack high-water、D-Cache-on、真正长时间 Stress、TX throughput baseline。

## 11. 资料依据

- 当前有效 STM32H743VIT6 原理图；
- STM32H743 Datasheet / Reference Manual；
- LAN8720A/LAN8720AI Datasheet；
- 仓库 HAL 1.11.6；
- Example `.ioc`、linker、Port；
- Raw Frame / async RX/TX / RX stress 上板结果。
