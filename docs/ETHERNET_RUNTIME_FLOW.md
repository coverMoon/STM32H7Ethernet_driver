# Ethernet Runtime Flow：RX/TX 异步路径、回调与 Ownership

本文用于解释当前 STM32H7 Ethernet Driver Package 的**实际运行时逻辑**。重点不是重复 API 清单，而是回答几个最容易绕的问题：

- CubeMX `As weak` Task Entry 与 Package 强定义是什么关系；
- HAL callback、运行时 Handler 注册、Thread Flag 分别是什么机制；
- ETH IRQ 如何把 RX/TX 工作交给任务上下文；
- RX Descriptor、RX DMA Buffer、CPU Frame 如何流转；
- TX Frame 如何从调用者 Buffer 进入 DMA，并在发送完成后回收到 Driver Pool；
- `EthernetDriver_SetRxEventHandler()`、`EthernetDriver_SetTxEventHandler()`、`EthernetRtos_SetRxFrameHandler()` 各自解决什么问题；
- 为什么当前 RX/TX 共用一个 `EthernetRtos_RuntimeTask()`。

本文描述的是当前已经完成上板回归的第一版数据路径：

```text
RX：copy-based async RX
TX：copy-based async TX
RTOS：单 Ethernet Runtime Task
```

当前尚未覆盖：完整 DMA error recovery、Link Down/Up MAC lifecycle、D-Cache-on、高负载/长时间 Stress、LwIP `ethernetif`。

---

## 1. 先建立整体心智模型

当前稳定分层：

```text
Application / LwIP
        ↓
RTOS Adapter / ethernetif
        ↓
Ethernet Driver
        ↓
PHY / MDIO
        ↓
Ethernet Port
        ↓
STM32 HAL / Hardware
```

每层只回答一个问题：

```text
Application / LwIP
→ “收到完整 Frame 后拿去做什么？要发送什么 Frame？”

RTOS Adapter
→ “IRQ 事件来了，怎样把延后处理放到任务上下文？”

Ethernet Driver
→ “MAC/DMA、Descriptor、Buffer ownership 和 Frame 收发怎么做？”

PHY / MDIO
→ “PHY 是否就绪？Link、Speed、Duplex 是什么？”

Port
→ “当前板的 ETH Handle、PHY Reset、DMA SRAM 准备在哪里？”
```

理解当前代码时，最重要的一点是：**不要把所有 callback / handler / weak function 当成同一种机制。**

---

## 2. 四种容易混在一起的机制

### 2.1 `__weak`：链接阶段的实现替换

当前 CubeMX FreeRTOS Task 配置：

```text
Task Name  : EthernetRuntime
Entry      : EthernetRtos_RuntimeTask
Generation : As weak
```

CubeMX 生成：

```c
__weak void EthernetRtos_RuntimeTask(void *argument)
{
    for (;;)
    {
        osDelay(1);
    }
}
```

Package 中提供同名强定义：

```c
void EthernetRtos_RuntimeTask(void *argument)
{
    ...
}
```

链接时：

```text
weak EthernetRtos_RuntimeTask
        +
strong EthernetRtos_RuntimeTask
        ↓
最终使用 Package strong implementation
```

因此 `As weak` **不是运行时 callback 注册**。

它只解决构建边界：

```text
CubeMX
→ Task object
→ priority
→ stack
→ allocation
→ osThreadNew()
→ weak stub

Ethernet Package
→ Runtime Task 的真正实现
```

当前 Reference Example 已验证该方式可以 Generate Code、Build 并正常上板运行。

### 2.2 HAL 固定 callback：HAL 主动调用 Driver

当前 `USE_HAL_ETH_REGISTER_CALLBACKS` 路径下使用的是 HAL 固定 weak callback 机制，Driver Package 提供对应强定义，例如：

```c
HAL_ETH_RxCpltCallback()
HAL_ETH_RxAllocateCallback()
HAL_ETH_RxLinkCallback()
HAL_ETH_TxCpltCallback()
HAL_ETH_TxFreeCallback()
```

它们不是通过本项目的 `Set...Handler()` 运行时注册的。

可以理解为 HAL 预留了固定插槽：

```text
HAL
→ “RX DMA complete 时调用 HAL_ETH_RxCpltCallback()”

HAL
→ “需要新的 RX DMA Buffer 时调用 HAL_ETH_RxAllocateCallback()”

HAL
→ “读取 RX Descriptor 数据时调用 HAL_ETH_RxLinkCallback()”

HAL
→ “TX DMA complete 时调用 HAL_ETH_TxCpltCallback()”

HAL_ETH_ReleaseTxPacket()
→ “确认 Packet 已发送完成后调用 HAL_ETH_TxFreeCallback()”
```

这些 callback 的**执行上下文并不相同**，后文会分别说明。

### 2.3 `Set...Handler()`：真正的运行时函数指针注册

当前真正的运行时 Handler 注册有三处：

```c
EthernetDriver_SetRxEventHandler(...);
EthernetDriver_SetTxEventHandler(...);
EthernetRtos_SetRxFrameHandler(...);
```

它们本质都是：

```text
保存函数指针 + context
→ 事件发生时调用该函数指针
```

方向如下：

```text
                 RX complete event
Driver  ─────────────────────────────→ RTOS Adapter
        EthernetDriver_SetRxEventHandler()

                 TX complete event
Driver  ─────────────────────────────→ RTOS Adapter
        EthernetDriver_SetTxEventHandler()

                 complete RX Frame
RTOS Adapter ─────────────────────────→ Application / ethernetif
             EthernetRtos_SetRxFrameHandler()
```

前两层是 Package 内部绑定，普通使用者通常不直接调用；第三层是上层真正需要设置的 Frame 交付入口。

### 2.4 Thread Flag：不是 callback，而是 ISR → Task 的事件传递

RTOS Adapter 当前定义两个事件位：

```c
ETHERNET_RX_EVENT_FLAG
ETHERNET_TX_EVENT_FLAG
```

ISR callback 只执行：

```c
osThreadFlagsSet(...);
```

Runtime Task 阻塞等待：

```c
osThreadFlagsWait(...);
```

Thread Flag 是**事件位**，不是 Packet Counter。因此多个相同事件可能合并成一个 bit；这也是为什么 RX 必须 drain、TX release 必须一次扫描所有已完成 Packet。

---

## 3. 上电与初始化路径

Reference Example 主初始化顺序：

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

当前 STM32H743 Example 使用 SRAM3 作为 Ethernet DMA SRAM，因此 Port 在 `MX_ETH_Init()` 前显式使能对应 SRAM clock。

这是板级行为。换板后由目标工程自己的 `ethernet_port.c` 决定。

### 3.2 `MX_ETH_Init()`

CubeMX/HAL 负责建立：

```text
ETH peripheral
RMII hardware config
RX/TX Descriptor
MAC address
ETH_HandleTypeDef
```

通用 Driver 不直接依赖 Demo 全局变量 `heth`，而通过：

```c
EthernetPort_GetHandle();
```

取得目标工程的 ETH Handle。

### 3.3 `EthernetDriver_Init()`

它主要初始化 Driver 软件状态，而不是重新初始化 ETH 外设：

```text
RX Buffer ownership
TX Buffer ownership
CPU-side RX Frame state
RX event handler
TX event handler
```

所以：

```text
MX_ETH_Init()
→ HAL / peripheral 初始化

EthernetDriver_Init()
→ Driver 软件 ownership / callback 状态初始化
```

---

## 4. Scheduler 启动后的两个 Ethernet 角色

当前 Example 主要有两个相关 Task：

```text
BootstrapTask
→ PHY bring-up
→ Auto-negotiation
→ Link / Speed / Duplex
→ MAC config / start
→ Demo 测试触发

EthernetRuntime
→ EthernetRtos_RuntimeTask()
→ RX deferred processing
→ TX completion reclaim
```

`EthernetRuntime` 不负责 PHY polling，也不执行应用业务。

---

## 5. Runtime Task 启动时做什么

CubeMX 创建：

```c
osThreadNew(
    EthernetRtos_RuntimeTask,
    NULL,
    &EthernetRuntime_attributes);
```

最终执行的是 Package 强定义。

Task 启动后：

```text
osThreadGetId()
        ↓
保存 g_runtime_task_handle
        ↓
EthernetDriver_SetRxEventHandler(EthernetRtos_OnRxEvent)
        ↓
EthernetDriver_SetTxEventHandler(EthernetRtos_OnTxEvent)
        ↓
g_ready = true
        ↓
等待 RX/TX Thread Flags
```

因此当前：

```c
EthernetRtos_IsReady()
```

表示的不是“RX Task 已经启动”，而是：

> Ethernet Runtime Task 已拿到自己的 Task Handle，并完成 RX/TX ISR event 的绑定，可以安全启动中断模式 MAC/DMA。

Bootstrap 在 `EthernetDriver_Start()` 前检查该状态，避免：

```text
MAC 已启动
→ IRQ 发生
→ Runtime Task 还没有建立 notification 绑定
```

---

## 6. BootstrapTask 做什么

Reference Example 启动时先设置最终 RX Frame Handler：

```c
EthernetRtos_SetRxFrameHandler(
    EthernetDemo_RxFrameHandler,
    NULL);
```

它的含义是：

> Runtime Task 以后拿到完整 RX Frame 后，把 Frame 交给 Demo Handler。

未来进入 LwIP 后，这个位置会由 `ethernetif` 适配逻辑接管，而 Driver / Runtime 底层无需理解 IP、UDP、TCP。

随后 Bootstrap 执行：

```text
EthernetPort_PhyResetRelease()
        ↓
Lan8720_IsReady()
        ↓
Lan8720_RestartAutoNegotiation()
        ↓
Lan8720_GetStatus()
        ↓
EthernetDriver_ConfigureLink()
        ↓
EthernetDriver_Start()
        ↓
HAL_ETH_Start_IT()
```

MAC/DMA 启动后，RX 和 TX 都使用 ETH global interrupt。

---

# Part A：RX Runtime

## 7. 一个 RX Frame 到来后的完整路径

### 7.1 硬件阶段

```text
PC / Network
    ↓
LAN8720 PHY
    ↓ RMII
STM32 ETH MAC
    ↓
RX Descriptor
    ↓
RX DMA Buffer
```

DMA 接收完成后触发 ETH IRQ。

### 7.2 ISR 阶段

CubeMX IRQ：

```c
void ETH_IRQHandler(void)
{
    HAL_ETH_IRQHandler(&heth);
}
```

HAL 识别 RX complete 后调用：

```c
HAL_ETH_RxCpltCallback();
```

Driver 的实现只转发 generic event：

```text
HAL_ETH_RxCpltCallback()
        ↓
g_rx_event_handler
        ↓
EthernetRtos_OnRxEvent()
        ↓
osThreadFlagsSet(ETHERNET_RX_EVENT_FLAG)
        ↓
ISR return
```

ISR 不做：

```text
Frame memcpy
HAL_ETH_ReadData()
协议解析
业务处理
printf
阻塞等待
```

### 7.3 Runtime Task 被唤醒

Runtime Task 等待：

```c
osThreadFlagsWait(
    ETHERNET_RX_EVENT_FLAG |
    ETHERNET_TX_EVENT_FLAG,
    osFlagsWaitAny,
    osWaitForever);
```

若包含 RX flag，调用内部 RX drain：

```text
EthernetDriver_Receive()
→ Frame

EthernetDriver_Receive()
→ Frame

...

EthernetDriver_Receive()
→ ETHERNET_RX_NONE
```

为什么必须 drain？

因为三个 RX IRQ 在 Task 获得 CPU 前发生时，Thread Flag 仍可能只表现为一个 RX bit：

```text
IRQ
IRQ
IRQ
 ↓
one RX event bit
```

因此 Task 不能假设“一次 flag 等于一个 Frame”。

---

## 8. `HAL_ETH_ReadData()` 与三个 RX callback 的职责

`EthernetDriver_Receive()` 在任务上下文调用：

```c
HAL_ETH_ReadData();
```

HAL 在处理已完成 RX Descriptor 时会调用：

```c
HAL_ETH_RxLinkCallback();
HAL_ETH_RxAllocateCallback();
```

三个 RX callback 可以这样区分：

### 8.1 `HAL_ETH_RxCpltCallback()`

```text
含义：有 RX 数据完成
上下文：ISR
职责：只发事件
```

### 8.2 `HAL_ETH_RxLinkCallback()`

```text
含义：当前 Descriptor 对应的数据需要交给应用侧表示
上下文：Runtime Task / HAL_ETH_ReadData()
职责：DMA Buffer → Driver CPU-side Frame，随后释放 DMA Buffer
```

### 8.3 `HAL_ETH_RxAllocateCallback()`

```text
含义：HAL 要重新武装 RX Descriptor
上下文：Runtime Task / HAL descriptor rebuild
职责：从静态 RX Pool 找空闲 Buffer，标记 in-use 后交给 HAL/DMA
```

---

## 9. RX Buffer ownership

当前 Driver：

```text
RX Buffer Count = 4
Buffer Size     = 1536 B
Alignment       = 32 B
```

### 9.1 DMA 获取 RX Buffer

```text
free RX Pool
    ↓
HAL_ETH_RxAllocateCallback()
    ↓
g_rx_buffer_in_use[i] = true
    ↓
DMA owns RX Buffer
```

### 9.2 CPU 获取 Frame

```text
DMA RX Buffer
    ↓
HAL_ETH_ReadData()
    ↓
HAL_ETH_RxLinkCallback()
    ↓ memcpy #1
Driver CPU-side g_rx_frame
    ↓
ReleaseRxBuffer()
    ↓
RX Buffer 回到 Pool
```

上层不会直接持有 DMA RX Buffer。

### 9.3 第二次 copy

`EthernetDriver_Receive()` 再把 Driver 内部 Frame copy 到 RTOS Adapter 的 Frame Buffer：

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
Application / ethernetif Handler
```

当前明确接受两次 copy，以换取简单、可验证的 ownership。

---

## 10. RX Frame Handler 生命周期

Runtime Task 读取到完整 Frame 后：

```c
g_rx_frame_handler(
    g_rx_frame,
    frame_length,
    context);
```

当前约束：

```text
handler 在 Runtime Task 上下文执行
frame pointer 只在 handler 调用期间有效
handler 返回后不得继续持有 pointer
需要长期保存 → 上层自己复制
```

Reference Example 的 `EthernetDemo_RxFrameHandler()` 只负责 `0x88B5` 测试统计。

---

# Part B：TX Runtime

## 11. Async TX 提交路径

当前公共接口：

```c
EthernetTxResult EthernetDriver_TransmitAsync(
    const uint8_t *frame,
    uint16_t length);
```

返回值：

```text
ETHERNET_TX_QUEUED
→ Frame 已复制到 Driver TX DMA Buffer，并成功提交 HAL/DMA
→ caller 原始 frame 可以立即复用

ETHERNET_TX_RETRY
→ 当前临时没有可用 TX Buffer / Descriptor
→ caller 可以稍后重试

ETHERNET_TX_ERROR
→ 参数无效或 Driver/MAC 状态无效
```

注意：`QUEUED` 表示**异步提交成功**，不是“Frame 已经发到网线上”。

### 11.1 submit 前先做 completion backstop

`EthernetDriver_TransmitAsync()` 首先调用：

```c
EthernetDriver_ProcessTxCompletions();
```

目的：

> 如果 TX completion event 已经发生，但 Runtime Task 尚未来得及处理，新的 submit 也会顺手回收已经完成的 Descriptor / Buffer。

这不是第二套 ownership，而是同一 `HAL_ETH_ReleaseTxPacket()` 机制的额外触发点。

### 11.2 获取 Driver TX DMA Buffer

```text
free TX Pool
    ↓
EthernetDriver_AcquireTxBuffer()
    ↓
g_tx_buffer_in_use[i] = true
    ↓
Driver owns TX Buffer
```

当前：

```text
TX Buffer Count = 4
Buffer Size     = 1536 B
Alignment       = 32 B
```

没有额外软件 TX Queue。

### 11.3 copy caller Frame

```text
Caller Frame
    ↓ memcpy
Driver TX DMA Buffer
```

copy 完成后，caller 原始 Buffer 生命周期就与 DMA 解耦。

这也是当前第一版不追求 TX zero-copy 的核心好处。

### 11.4 交给 HAL

Driver 构造：

```c
ETH_BufferTypeDef
ETH_TxPacketConfigTypeDef
```

并关键地设置：

```c
tx_config.pData = dma_buffer;
```

当前 HAL `HAL_ETH_Transmit_IT()` 会把 `pData` 保存到自己的 TX bookkeeping：

```text
pData
→ TxDescList.CurrentPacketAddress
→ TxDescList.PacketAddress[descidx]
```

因此 HAL 后续能够知道：

> “这个已完成 TX Packet 对应 Driver Pool 中哪个 DMA Buffer？”

随后：

```c
HAL_ETH_Transmit_IT(...);
```

配置 Descriptor IOC/OWN 并启动 DMA，然后立即返回。

成功后 ownership 转移为：

```text
Driver TX Buffer
→ HAL / DMA owns Buffer
```

---

## 12. TX complete IRQ 路径

DMA 完成发送后：

```text
TX DMA complete
    ↓
ETH IRQ
    ↓
HAL_ETH_IRQHandler()
    ↓
HAL 检测 ETH_DMACSR_TI
    ↓
HAL_ETH_TxCpltCallback()
```

Driver 的 `HAL_ETH_TxCpltCallback()` 与 RX complete callback 一样，只转发事件：

```text
HAL_ETH_TxCpltCallback()
        ↓
g_tx_event_handler
        ↓
EthernetRtos_OnTxEvent()
        ↓
osThreadFlagsSet(ETHERNET_TX_EVENT_FLAG)
        ↓
ISR return
```

这里**不直接释放 Descriptor，也不直接释放 TX Buffer**。

这样 ISR 仍然保持短小。

---

## 13. Runtime Task 如何回收 TX Buffer

Runtime Task 收到 TX flag 后，优先执行：

```c
EthernetDriver_ProcessTxCompletions();
```

Driver 调用：

```c
HAL_ETH_ReleaseTxPacket();
```

HAL 从自己的：

```text
TxDescList.releaseIndex
TxDescList.BuffersInUse
TxDescList.PacketAddress[]
```

开始扫描。

对于已经由 DMA 释放 `OWN` 的 Packet：

```text
Descriptor OWN == 0
        ↓
HAL 确认 Packet 已发送完成
        ↓
HAL_ETH_TxFreeCallback(PacketAddress)
        ↓
PacketAddress = NULL
        ↓
更新 BuffersInUse / releaseIndex
```

Driver 的 `HAL_ETH_TxFreeCallback()` 收到的正是提交时存入 `pData` 的 TX DMA Buffer 地址：

```text
HAL_ETH_TxFreeCallback(dma_buffer)
        ↓
EthernetDriver_ReleaseTxBuffer()
        ↓
g_tx_buffer_in_use[i] = false
        ↓
Buffer 回到 Driver TX Pool
```

完整 ownership：

```text
Caller Frame
    ↓ copy
Driver TX DMA Buffer
    ↓ submit
HAL / DMA
    ↓ TX complete
HAL TxDesc bookkeeping
    ↓ HAL_ETH_ReleaseTxPacket()
HAL_ETH_TxFreeCallback()
    ↓
Driver TX Pool
```

---

## 14. 为什么 `HAL_ETH_TxCpltCallback()` 不直接 free

这两个动作含义不同：

```text
HAL_ETH_TxCpltCallback()
→ “发生了 TX complete interrupt”

HAL_ETH_ReleaseTxPacket()
→ “检查当前所有 HAL TX bookkeeping，真正释放已经完成的 Packet”
```

TX interrupt 是事件提示；Packet reclaim 是状态扫描。

因此当前设计：

```text
ISR
→ 只发 TX event

Runtime Task
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ Buffer recycle
```

与 RX 的设计原则一致：ISR 只通知，复杂 ownership 工作放任务上下文。

---

## 15. 为什么多个 TX complete event 可以只用一个 Thread Flag

Thread Flag 不是完成计数器。

如果多个 TX complete 在 Runtime Task 获得 CPU 前发生：

```text
TX IRQ
TX IRQ
TX IRQ
 ↓
one TX event bit
```

但这不会要求我们保留“三次 callback 计数”。

原因是 `HAL_ETH_ReleaseTxPacket()` 本身会从 `releaseIndex` 开始扫描所有当前可释放 Packet，直到遇到仍由 DMA `OWN` 的 Packet。

因此：

```text
一个 TX event
→ 一次 ReleaseTxPacket()
→ 可以回收多个已完成 Packet
```

这与 RX 的：

```text
一个 RX event
→ Receive() 循环 drain 到 NONE
```

是同一种“事件提示 + 状态 drain”的思路。

---

## 16. 为什么 RX/TX 共用一个 Runtime Task

当前没有建立独立 `EthernetRxTask` / `EthernetTxTask`。

而是：

```text
EthernetRtos_RuntimeTask()
        ├── RX event
        │    └── drain EthernetDriver_Receive()
        │
        └── TX event
             └── EthernetDriver_ProcessTxCompletions()
```

优点：

- 不为了 TX completion 额外创建 Task；
- RX/TX ISR 都只做 Thread Flag 通知；
- Ethernet deferred processing 集中在一个任务；
- Application/CubeMX 仍只管理一个 Ethernet Runtime Task 的资源；
- 后续接入 LwIP 时边界仍然清楚。

当前同时收到 RX/TX flag 时，Task **优先处理 TX completion**：

```text
TX reclaim first
→ 尽快把只有 4 个的 TX Buffer 归还 Pool

RX drain second
→ 读取所有当前可用 Frame
```

这只是合理的当前顺序，不意味着 TX 业务优先级高于网络接收业务。

---

## 17. TX submit / reclaim 的并发保护

`HAL_ETH_Transmit_IT()` 与 `HAL_ETH_ReleaseTxPacket()` 都会操作 HAL 的 TX Descriptor bookkeeping：

```text
CurTxDesc
PacketAddress[]
BuffersInUse
releaseIndex
Descriptor OWN / IOC
```

当前 Driver 使用**短 PRIMASK 临界区**序列化：

```text
HAL_ETH_Transmit_IT()
HAL_ETH_ReleaseTxPacket()
Driver TX Pool acquire/release
```

但不会把整段 Frame `memcpy()` 放入关中断区。

逻辑：

```text
memcpy caller → TX DMA Buffer
        ↓
进入短 critical section
        ↓
HAL_ETH_Transmit_IT()
        ↓
退出
```

completion：

```text
进入短 critical section
        ↓
HAL_ETH_ReleaseTxPacket()
        ↓
TxFreeCallback → Driver Pool release
        ↓
退出
```

目的是保证 TX submit 与 reclaim 不会并发修改同一套 HAL/Driver bookkeeping。

当前这不是通用 RTOS mutex 抽象；Driver Core 仍然不依赖 FreeRTOS/CMSIS-RTOS2。

---

## 18. TX backpressure：为什么没有软件 Queue

当前只有 4 个静态 TX DMA Buffer。

如果 4 个 Buffer 都处于 in-use：

```c
EthernetDriver_TransmitAsync(...)
→ ETHERNET_TX_RETRY
```

调用者决定何时重试。

当前 Driver 不隐藏：

```text
malloc
动态 TX Queue
阻塞等待网络对端
```

这符合第一版“接口窄、ownership 明确、先做最小可验证实现”的目标。

未来 LwIP `ethernetif` 接入时，再根据实际上层调用模型决定如何映射 `RETRY` / backpressure；当前不提前设计复杂队列。

---

## 19. RX 与 TX 的 copy 数量

当前：

### RX

```text
DMA RX Buffer
    ↓ memcpy #1
Driver CPU-side Frame
    ↓ memcpy #2
RTOS Adapter Frame
    ↓
Application / ethernetif
```

### TX

```text
Application / LwIP Frame
    ↓ memcpy #1
Driver TX DMA Buffer
    ↓
HAL / DMA
```

所以：

```text
RX：2 copies
TX：1 copy
```

第一版主动接受这些 copy，不在基础正确性、Stress、性能测量前引入 zero-copy ownership 复杂度。

---

## 20. 当前 Runtime API 应怎样理解

### Driver → RTOS 内部绑定

```c
EthernetDriver_SetRxEventHandler(...);
EthernetDriver_SetTxEventHandler(...);
```

普通使用者通常不直接调用。

`EthernetRtos_RuntimeTask()` 启动时自动绑定：

```text
RX complete → EthernetRtos_OnRxEvent
TX complete → EthernetRtos_OnTxEvent
```

### RTOS → 上层

```c
EthernetRtos_SetRxFrameHandler(...);
```

这是上层真正关心的接收交付接口。

### 上层 → Driver TX

```c
EthernetDriver_TransmitAsync(...);
```

当前直接提交完整 Ethernet Frame。

### TX completion

当前 TX completion **不向 Application 再暴露 per-frame completion callback**。

因为 copy-based API 的 caller ownership 在 `ETHERNET_TX_QUEUED` 返回时已经结束；后续 completion 主要用于 Driver 内部回收 DMA Buffer。

---

## 21. RX/TX 两条完整链路对照

### RX

```text
Network
 ↓
PHY / RMII
 ↓
MAC / DMA
 ↓
RX DMA Buffer
 ↓
ETH IRQ
 ↓
HAL_ETH_IRQHandler()
 ↓
HAL_ETH_RxCpltCallback()
 ↓
Driver RX event handler
 ↓
EthernetRtos_OnRxEvent()
 ↓
RX Thread Flag
 ↓
EthernetRtos_RuntimeTask()
 ↓
EthernetDriver_Receive()
 ↓
HAL_ETH_ReadData()
 ├─ HAL_ETH_RxLinkCallback()
 │    └─ DMA Buffer → Driver CPU Frame → release RX Buffer
 └─ HAL_ETH_RxAllocateCallback()
      └─ new RX Buffer → DMA
 ↓
RTOS Adapter Frame
 ↓
EthernetRtos_SetRxFrameHandler() 注册的上层 Handler
```

### TX

```text
Application / LwIP
 ↓
EthernetDriver_TransmitAsync()
 ↓ memcpy
Driver TX DMA Buffer
 ↓
HAL_ETH_Transmit_IT()
 ↓
MAC / DMA
 ↓
Network

TX complete
 ↓
ETH IRQ
 ↓
HAL_ETH_IRQHandler()
 ↓
HAL_ETH_TxCpltCallback()
 ↓
Driver TX event handler
 ↓
EthernetRtos_OnTxEvent()
 ↓
TX Thread Flag
 ↓
EthernetRtos_RuntimeTask()
 ↓
EthernetDriver_ProcessTxCompletions()
 ↓
HAL_ETH_ReleaseTxPacket()
 ↓
HAL_ETH_TxFreeCallback()
 ↓
Driver TX Buffer returned to pool
```

---

## 22. Reference Example 当前验证状态

当前 Reference Example 已完成：

```text
PHY ready / Auto-negotiation / 100M Full
MAC/DMA Start_IT
async RX 1000 / 1000
EthernetRuntime + EthernetRtos_RuntimeTask + As weak
async TX 1000-frame continuous submit / completion recycle
RuntimeTask 改名后的 RX 1000 / 1000 回归
```

这说明：

- RX IRQ → RuntimeTask → ReadData → RX Buffer recycle 可持续工作；
- TX submit → TX IRQ → RuntimeTask → ReleaseTxPacket → TxFreeCallback → TX Buffer recycle 可持续工作；
- RX/TX 共用 RuntimeTask 没有破坏已验证的 RX 基础路径。

这些结果仍然不是高负载、吞吐极限或长时间 Stress 的证明。

---

## 23. 当前仍未解决的问题

以下内容不要从本文当前链路外推为“已经完成”：

```text
RX/TX error/drop 统计
DMA fatal / RBU / timeout recovery
TX async error 后完整 recovery
Link Down → MAC stop
Link Up → speed/duplex reconfigure → restart
Task stack high-water mark
D-Cache-on
高负载 / 长时间 Stress
LwIP ethernetif / Ping / UDP / TCP
```

尤其当前 TX `ETHERNET_TX_RETRY` 只表示临时资源不足 / HAL 暂不可提交；完整错误分类和 recovery 仍属于后续工作单元。

---

## 24. 最简记忆模型

如果以后重新读代码，只记下面几句话：

```text
Port
→ 当前板硬件绑定

PHY / MDIO
→ 链路状态

Driver
→ DMA / Descriptor / Buffer ownership

Runtime Task
→ 把 RX/TX IRQ 的后续工作移到任务上下文

Application / LwIP
→ Frame 最终拿去做什么
```

回调机制：

```text
__weak
→ 链接阶段让 CubeMX Task 与 Package strong implementation 接起来

HAL callback
→ HAL 把硬件 / Descriptor 生命周期事件交给 Driver

Driver RX/TX event handler
→ Driver 把 ISR event 交给 RTOS Adapter

Thread Flag
→ ISR → Runtime Task 的事件传递

RX Frame Handler
→ Runtime Task 把完整 RX Frame 交给上层
```

Ownership：

```text
RX
DMA Buffer → copy → CPU Frame → 立即回 Pool

TX
Caller Frame → copy → TX DMA Buffer → DMA → completion → TxFreeCallback → 回 Pool
```

把这几层分开后，当前 Runtime 逻辑基本就不会再绕。
