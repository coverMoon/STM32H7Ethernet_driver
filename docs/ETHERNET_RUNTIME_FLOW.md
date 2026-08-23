# Ethernet Runtime Flow 与回调机制说明

本文用于解释当前 STM32H7 Ethernet Driver Package 的运行时逻辑，重点说明以下几个容易混淆的部分：

- CubeMX `As weak` Task Entry 到底做了什么；
- HAL callback 与运行时注册 callback 有什么区别；
- ETH IRQ 如何唤醒 RX Task；
- RX Descriptor / DMA Buffer / CPU Frame 如何流转；
- `EthernetDriver_SetRxEventHandler()` 与 `EthernetRtos_SetRxFrameHandler()` 分别解决什么问题。

本文描述的是当前已验证的 **copy-based async RX + polling TX** 路径。异步 TX、LwIP `ethernetif`、完整 Link lifecycle 和 DMA error recovery 不在本文范围内。

## 1. 先建立整体心智模型

当前代码可以按五层理解：

```text
Application / LwIP
        ↓
RTOS Adapter
        ↓
Ethernet Driver
        ↓
PHY / MDIO
        ↓
Ethernet Port / STM32 HAL / Hardware
```

各层只回答一个问题：

```text
Port
→ “这块板上的 ETH Handle、PHY Reset、DMA SRAM 准备在哪里？”

PHY / MDIO
→ “PHY 是否就绪？Link、Speed、Duplex 是什么？”

Ethernet Driver
→ “MAC/DMA、Descriptor、Buffer ownership 和 Frame 收发怎么做？”

RTOS Adapter
→ “RX 中断来了，怎样把处理工作交给任务？”

Application / LwIP
→ “收到完整 Ethernet Frame 以后拿去做什么？”
```

理解当前代码时，不要把所有 callback 当成同一种机制。实际上共有三类完全不同的机制。

## 2.  callback / 函数替换机制

### 2.1 `__weak`：链接阶段的实现替换

CubeMX 中 Ethernet RX Task 当前配置为：

```text
Task Entry : EthernetRtos_RxTask
Generation : As weak
```

CubeMX 会生成一个 weak stub：

```c
__weak void EthernetRtos_RxTask(void *argument)
{
    for (;;)
    {
        osDelay(1);
    }
}
```

Driver Package 中则提供同名强定义：

```c
void EthernetRtos_RxTask(void *argument)
{
    ...
}
```

链接时：

```text
weak EthernetRtos_RxTask
        +
strong EthernetRtos_RxTask
        ↓
最终使用 strong implementation
```

其解决一个构建边界问题：

```text
CubeMX
→ 管理 Task object / priority / stack / allocation / osThreadNew()

Ethernet Package
→ 提供 Task 的真正实现
```

CubeMX 生成的 weak 函数在当前最终固件中不会承担 RX 逻辑。

### 2.2 HAL 固定 callback：HAL 主动调用 Driver

当前 HAL 配置使用固定名称 callback，例如：

```c
HAL_ETH_RxCpltCallback()
HAL_ETH_RxAllocateCallback()
HAL_ETH_RxLinkCallback()
```

可以理解为：

```text
HAL
→ “发生 RX complete 时，我会找 HAL_ETH_RxCpltCallback()”

HAL
→ “需要新的 RX DMA Buffer 时，我会找 HAL_ETH_RxAllocateCallback()”

HAL
→ “读取 Descriptor 数据时，我会找 HAL_ETH_RxLinkCallback()”
```

当前 Driver Package 提供这些 callback 的强定义。

### 2.3 `Set...Handler()`：运行时注册

当前真正的运行时函数指针注册只有两层：

```c
EthernetDriver_SetRxEventHandler(...);
EthernetRtos_SetRxFrameHandler(...);
```

它们本质都是：

```text
保存一个函数指针
→ 事件发生时再调用这个函数指针
```

两者方向完全不同：

```text
Driver
  │
  │ EthernetDriver_SetRxEventHandler()
  ▼
RTOS Adapter
  │
  │ EthernetRtos_SetRxFrameHandler()
  ▼
Application / LwIP
```

第一层用于 **Driver → RTOS**，第二层用于 **RTOS → 上层**。

## 3. 上电与初始化路径

当前 Reference Example 的主初始化顺序为：

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
MX_USART1_UART_Init()
MX_ETH_Init()
        ↓
EthernetDriver_Init()
        ↓
osKernelInitialize()
MX_FREERTOS_Init()
        ↓
osKernelStart()
```

### 3.1 `EthernetPort_PrepareDmaMemory()`

这是板级 Port 行为。

当前 STM32H743 Reference Example 使用 SRAM3 作为 Ethernet DMA SRAM，因此该函数负责在 `MX_ETH_Init()` 之前使能对应 SRAM clock。

换板后，这部分由目标工程自己的 `ethernet_port.c` 实现。

### 3.2 `MX_ETH_Init()`

这是 CubeMX/HAL 负责的 ETH 外设初始化。

它建立：

```text
ETH peripheral
RMII/MII hardware config
Descriptor configuration
MAC address
ETH_HandleTypeDef
```

通用 Driver 不直接依赖 CubeMX 全局变量名 `heth`，而是通过：

```c
EthernetPort_GetHandle();
```

取得当前目标工程的 HAL ETH Handle。

### 3.3 `EthernetDriver_Init()`

`EthernetDriver_Init()` 当前主要初始化 Driver 的**软件状态**，而不是重新初始化 ETH 硬件。

它会清理：

```text
RX Buffer ownership
TX Buffer ownership
CPU-side RX frame state
RX event handler
```

因此：

```text
MX_ETH_Init()
→ HAL / peripheral 初始化

EthernetDriver_Init()
→ Driver 软件 ownership / callback 状态初始化
```

## 4. Scheduler 启动后的两个角色

当前 Reference Example 主要有两个 Ethernet 相关 Task：

```text
BootstrapTask
→ 负责 PHY bring-up、Link/Speed/Duplex、MAC/DMA Start

EthernetRxTask
→ 负责等待 RX event 并在任务上下文读取 Frame
```

两者职责需要分开理解。

## 5. RX Task 启动时做什么

CubeMX 创建 Task 时直接使用：

```c
osThreadNew(EthernetRtos_RxTask, ...);
```

最终执行的是 Driver Package 中的强定义 `EthernetRtos_RxTask()`。

Task 启动后首先执行：

```text
osThreadGetId()
        ↓
保存自己的 task handle
        ↓
EthernetDriver_SetRxEventHandler(EthernetRtos_OnRxEvent, NULL)
        ↓
g_ready = true
```

这一步中的：

```c
EthernetDriver_SetRxEventHandler(
    EthernetRtos_OnRxEvent,
    NULL);
```

表示：

> Driver 以后收到 RX complete ISR event 时，把这个事件转交给 RTOS Adapter 的 `EthernetRtos_OnRxEvent()`。

这是 Driver 与 RTOS 解耦的关键。

Driver Core 自己不 include `cmsis_os2.h`，也不知道 Thread Flag、Task Handle 或 FreeRTOS 的存在。

### 5.1 为什么需要 `EthernetRtos_IsReady()`

Task 必须先拿到自身 handle，并完成 Driver RX event 绑定，才能安全启动 MAC/DMA async RX。

因此 Bootstrap 在 `EthernetDriver_Start()` 前检查：

```c
EthernetRtos_IsReady();
```

这样避免：

```text
MAC 已启动
→ Frame 到达
→ IRQ 发生
→ RX Task 还没有建立 notification 绑定
```

## 6. BootstrapTask 的启动路径

BootstrapTask 首先为 Reference Example 注册最终 Frame Handler：

```c
EthernetRtos_SetRxFrameHandler(
    EthernetDemo_RxFrameHandler,
    NULL);
```

它的含义是：

> RX Task 以后拿到完整 Frame 后，把 Frame 交给 `EthernetDemo_RxFrameHandler()`。

这是当前普通使用者真正需要关心的那一层运行时注册。

Reference Example 的 Handler 只负责 `0x88B5` 测试和 1000 Frame 计数；未来接入 LwIP 后，这一层可以替换成 `ethernetif` 的 Frame 入口。

随后 BootstrapTask 执行 PHY bring-up：

```text
等待上电稳定
        ↓
EthernetPort_PhyResetRelease()
        ↓
Lan8720_IsReady()
        ↓
Lan8720_RestartAutoNegotiation()
        ↓
轮询 Lan8720_GetStatus()
        ↓
获得 Link / Speed / Duplex
        ↓
EthernetDriver_ConfigureLink()
        ↓
EthernetDriver_Start()
```

### 6.1 PHY / MDIO 调用链

以 `Lan8720_IsReady()` 为例：

```text
Lan8720_IsReady()
        ↓
EthernetMdio_Read()
        ↓
EthernetPort_GetHandle()
        ↓
HAL_ETH_ReadPHYRegister()
```

因此 LAN8720 Driver 不知道：

```text
CubeMX
FreeRTOS
具体板子的 heth 变量名
具体 STM32H7 板型
```

它只依赖 MDIO Wrapper。

## 7. 一个 RX Frame 到来后的完整路径

### 7.1 硬件阶段

```text
PC / Network
    ↓
LAN8720 PHY
    ↓ RMII
STM32 ETH MAC
    ↓
DMA Descriptor
    ↓
RX DMA Buffer
```

DMA 收包完成后触发 ETH IRQ。

### 7.2 ISR 阶段：只做事件转发

CubeMX 生成：

```c
void ETH_IRQHandler(void)
{
    HAL_ETH_IRQHandler(&heth);
}
```

HAL 解析 DMA interrupt 状态后调用固定 callback：

```c
HAL_ETH_RxCpltCallback();
```

Driver 中的实现只做：

```text
读取 g_rx_event_handler
        ↓
调用已注册的 handler
```

而 RX Task 启动时已经注册：

```text
g_rx_event_handler
        =
EthernetRtos_OnRxEvent
```

因此实际继续执行：

```c
EthernetRtos_OnRxEvent()
{
    osThreadFlagsSet(...);
}
```

然后 ISR 返回。

这一阶段不做：

```text
Frame memcpy
协议解析
应用业务
printf
阻塞等待
```

所以 ISR 路径可以压缩成：

```text
ETH IRQ
→ HAL_ETH_IRQHandler()
→ HAL_ETH_RxCpltCallback()
→ Driver generic RX event
→ EthernetRtos_OnRxEvent()
→ osThreadFlagsSet()
```

## 8. RX Task 被唤醒以后

RX Task 平时阻塞在：

```c
osThreadFlagsWait(
    ETHERNET_RX_EVENT_FLAG,
    osFlagsWaitAny,
    osWaitForever);
```

收到通知后，Task 不只读取一次，而是持续 drain：

```text
EthernetDriver_Receive()
→ Frame

EthernetDriver_Receive()
→ Frame

...

EthernetDriver_Receive()
→ ETHERNET_RX_NONE
```

原因是 Thread Flag 是**事件位**，不是 Packet Counter。

例如三个 IRQ 在 Task 获得 CPU 之前发生：

```text
IRQ
IRQ
IRQ
 ↓
一个 RX EVENT bit
```

因此一次任务唤醒必须把当前所有可读 Frame 尽量读完。

## 9. `HAL_ETH_ReadData()` 与另外两个 HAL callback

`EthernetDriver_Receive()` 内部调用：

```c
HAL_ETH_ReadData();
```

这时 HAL 会在**任务上下文**处理已经完成的 RX Descriptor，并调用：

```c
HAL_ETH_RxLinkCallback();
HAL_ETH_RxAllocateCallback();
```

这两个函数与 ISR 中的 `HAL_ETH_RxCpltCallback()` 职责不同。

### 9.1 `HAL_ETH_RxCpltCallback()`

职责：

```text
“RX 有新数据了”
→ 只发事件
```

执行上下文：ISR。

### 9.2 `HAL_ETH_RxLinkCallback()`

职责：

```text
读取当前 Descriptor 对应数据
→ DMA Buffer 内容复制到 Driver CPU-side Frame
→ DMA Buffer 立即归还 RX Pool
```

执行上下文：当前设计下由 `HAL_ETH_ReadData()` 触发，因此在 RX Task 上下文。

### 9.3 `HAL_ETH_RxAllocateCallback()`

职责：

```text
HAL 需要重新武装 RX Descriptor
→ Driver 从 RX Pool 找一个空闲 DMA Buffer
→ 标记为 in-use
→ 返回给 HAL / DMA
```

因此三个 HAL callback 可以用一句话区分：

```text
RxCpltCallback
→ 通知“有包”

RxLinkCallback
→ 处理“这个 Descriptor 里的数据”

RxAllocateCallback
→ 给 DMA “新的接收 Buffer”
```

## 10. RX Buffer ownership

当前 Driver 有 4 个静态 RX DMA Buffer。

### 10.1 DMA 获取 Buffer

```text
free RX Pool
    ↓
HAL_ETH_RxAllocateCallback()
    ↓
标记 in-use
    ↓
DMA owns buffer
```

### 10.2 CPU 读取 Frame

Task 调用：

```c
HAL_ETH_ReadData();
```

HAL 调用 `HAL_ETH_RxLinkCallback()`：

```text
DMA RX Buffer
    ↓ memcpy
Driver CPU-side g_rx_frame
    ↓
ReleaseRxBuffer()
    ↓
Buffer 回到 RX Pool
```

上层永远不直接持有 DMA RX Buffer。

### 10.3 当前存在两次 RX copy

当前 copy-first 路径实际是：

```text
DMA RX Buffer
    │
    │ memcpy #1
    ▼
Driver CPU-side Frame
    │
    │ memcpy #2
    ▼
RTOS Adapter g_rx_frame
    │
    ▼
Application Frame Handler
```

第一次 copy 用于尽快归还 DMA Buffer；第二次 copy 把 Driver 内部 Frame 交给调用者 Buffer。

这不是性能最优方案，但 ownership 简单、边界清楚，符合当前“不主动追求 Zero Copy”的设计。

## 11. Frame Handler 与上层 ownership

RX Task 成功获得完整 Frame 后调用：

```c
g_rx_frame_handler(
    g_rx_frame,
    frame_length,
    g_rx_frame_handler_context);
```

该 Handler 由应用通过：

```c
EthernetRtos_SetRxFrameHandler(handler, context);
```

注册。

当前 Frame pointer 的生命周期是：

```text
只在 Handler 调用期间有效
```

Handler 返回后，上层不能继续持有该指针。

如果需要长期保存 Frame，上层必须自行复制。

## 12. 两个真正的运行时注册分别是谁

把 HAL 固定 callback 和 weak symbol 排除以后，真正的运行时注册只有两个：

| 注册接口 | 谁调用 | 保存什么 | 方向 | 普通用户是否需要直接调用 |
| --- | --- | --- | --- | --- |
| `EthernetDriver_SetRxEventHandler()` | RTOS Adapter | ISR event handler | Driver → RTOS | 否 |
| `EthernetRtos_SetRxFrameHandler()` | Application / ethernetif | 完整 Frame handler | RTOS → 上层 | 是 |

因此普通 CMSIS-RTOS2 使用者不需要理解或手动调用 `EthernetDriver_SetRxEventHandler()`。

`EthernetRtos_RxTask()` 启动时会自动完成 Driver → RTOS 的绑定。

用户真正需要做的是：

```c
EthernetRtos_SetRxFrameHandler(
    MyRxHandler,
    my_context);
```

也就是告诉 Adapter：

> “拿到完整 Frame 后交给谁。”

## 13. 当前 TX 路径为什么简单很多

当前 TX 仍是 polling：

```text
Caller Frame
    ↓ memcpy
TX DMA Buffer
    ↓
HAL_ETH_Transmit(timeout)
    ↓
等待 DMA 完成
    ↓
HAL_OK
    ↓
释放 TX DMA Buffer
```

因此当前 TX 没有 Task notification 和 completion handler 链。

未来切换到 `HAL_ETH_Transmit_IT()` 后，需要单独设计 TX completion ownership；不能直接把 RX callback 机制机械复制过去。

## 14. 未来接入 LwIP 时哪里会变化

当前 Reference Example 注册的是：

```c
EthernetDemo_RxFrameHandler();
```

它只做测试 EtherType 和计数。

未来可以变成：

```text
EthernetRtos_RxTask()
        ↓
LwIP / ethernetif Frame Handler
        ↓
pbuf
        ↓
LwIP input
```

因此正常情况下，下面这些层不需要因为接入 LwIP 而推翻：

```text
ETH IRQ
Driver generic RX event
CMSIS-RTOS2 Thread Flag
RX DMA Buffer ownership
EthernetDriver_Receive()
```

真正变化的是“完整 Frame 最后交给谁”。

## 15. 一张图串起全部 RX 逻辑

```text
                         Hardware
                            │
PC / Network → LAN8720 → RMII → STM32 MAC
                            │
                            ▼
                    DMA Descriptor / Buffer
                            │
                      RX complete IRQ
                            ▼
────────────────────────── ISR ──────────────────────────

ETH_IRQHandler()
    ↓
HAL_ETH_IRQHandler()
    ↓
HAL_ETH_RxCpltCallback()          ← HAL 固定 callback
    ↓
g_rx_event_handler
    ↓
EthernetRtos_OnRxEvent()          ← RTOS Adapter 注册给 Driver
    ↓
osThreadFlagsSet()

───────────────────────── Task ──────────────────────────

EthernetRtos_RxTask()
    ↓
EthernetDriver_Receive()
    ↓
HAL_ETH_ReadData()
    │
    ├─→ HAL_ETH_RxLinkCallback()      ← HAL 固定 callback
    │       ↓
    │   DMA Buffer → Driver CPU Frame
    │       ↓
    │   release RX Buffer
    │
    └─→ HAL_ETH_RxAllocateCallback()  ← HAL 固定 callback
            ↓
        free Buffer → DMA
    ↓
RTOS Adapter Frame Buffer
    ↓
g_rx_frame_handler
    ↓
Application / ethernetif Handler  ← 上层注册给 RTOS Adapter
```

## 16. 最简记忆方式

如果只保留几个关键概念，可以记成：

```text
weak function
→ 解决 CubeMX Task 与 Package 实现的链接边界

HAL callback
→ HAL 把硬件/DMA事件交给 Driver

Driver event handler
→ Driver 把 RX 中断事件交给 RTOS

Thread Flag
→ ISR 唤醒 RX Task

Frame handler
→ RX Task 把完整 Frame 交给 Application / LwIP
```

再压缩成一条主线：

```text
Hardware
→ HAL
→ Driver
→ RTOS Task
→ Application / LwIP
```

每一层只向上一层交付自己已经处理到合适粒度的信息：

```text
HAL       → RX complete / Descriptor
Driver    → generic RX event / complete Frame
RTOS      → task-context complete Frame
Application / LwIP → 协议和业务
```

这就是当前运行时设计中各种 callback 和注册机制存在的原因。