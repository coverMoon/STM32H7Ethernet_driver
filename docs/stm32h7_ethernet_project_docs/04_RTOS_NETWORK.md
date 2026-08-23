# FreeRTOS / LwIP Runtime Design

本文描述 Driver Package 与 FreeRTOS / CMSIS-RTOS2 / LwIP 的运行时边界。当前 async RX 与 async TX completion ownership 均已实现并完成上板回归；ethernetif 和 LwIP 尚未实现。

如果需要理解 weak symbol、HAL callback、运行时 Handler 注册、IRQ/Task 交接以及 RX/TX Buffer ownership 的完整调用过程，见：[`docs/ETHERNET_RUNTIME_FLOW.md`](../ETHERNET_RUNTIME_FLOW.md)。

## 1. 运行时分层

```text
Application / LwIP
        ↓
Frame Handler / ethernetif
        ↓
CMSIS-RTOS2 Adapter
        ↓
Ethernet Driver
        ↓
HAL ETH / DMA
```

Driver Core 不 include FreeRTOS/CMSIS-RTOS2；RTOS Adapter 是可选层。

## 2. Task ownership

Adapter 不创建 Task，也不隐藏 heap 使用。

```text
Application / CubeMX
→ Task object
→ priority
→ stack
→ static/dynamic allocation

Ethernet RTOS Adapter
→ EthernetRtos_RuntimeTask()
→ task handle
→ RX/TX Thread Flags
→ RX drain
→ TX completion reclaim
```

当前 API：

```text
EthernetRtos_SetRxFrameHandler()
EthernetRtos_IsReady()
EthernetRtos_RuntimeTask()
```

Task priority / stack 当前参考值只是 bring-up 参数，不作为 Driver 固定值；最终应根据实际 stack high-water mark 和系统调度测量调整。

## 3. Runtime Task 启动

Task 启动时：

```text
osThreadGetId()
→ 保存自身 handle
→ EthernetDriver_SetRxEventHandler(EthernetRtos_OnRxEvent)
→ EthernetDriver_SetTxEventHandler(EthernetRtos_OnTxEvent)
→ ready = true
```

MAC/DMA Start 前应确认：

```c
EthernetRtos_IsReady() == true
```

当前 `ready` 表示 Runtime Task 已完成 RX/TX 两类 ISR event 的绑定。

## 4. RX IRQ 路径

当前已验证：

```text
ETH_IRQHandler()
→ HAL_ETH_IRQHandler()
→ HAL_ETH_RxCpltCallback()
→ Ethernet Driver generic RX event
→ EthernetRtos_OnRxEvent()
→ osThreadFlagsSet(RX)
→ EthernetRtos_RuntimeTask()
```

ISR 只做必要 HAL 处理和事件通知，不执行 Frame copy、协议解析、应用业务或 `printf`。

当前 ETH IRQ priority = 5，与当前 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` 配合。这个数字属于 Reference Example，不冻结为通用配置。

## 5. RX deferred processing

Runtime Task 收到 RX flag 后必须 drain：

```text
EthernetDriver_Receive()
→ frame
EthernetDriver_Receive()
→ frame
...
EthernetDriver_Receive()
→ ETHERNET_RX_NONE
```

Thread Flag 是事件位，不是 Packet Counter；多个 IRQ 可以合并成一次任务唤醒。

`EthernetDriver_Receive()` 在任务上下文调用 `HAL_ETH_ReadData()`，由 HAL 继续触发：

```text
HAL_ETH_RxLinkCallback()
→ DMA Buffer 数据 copy 到 Driver CPU Frame
→ RX DMA Buffer release

HAL_ETH_RxAllocateCallback()
→ 重新给 RX Descriptor 分配静态 DMA Buffer
```

## 6. RX Frame Handler

应用通过：

```c
EthernetRtos_SetRxFrameHandler(handler, context);
```

在 Runtime Task 上下文接收完整 Frame。frame pointer 只在 Handler 调用期间有效，返回后不得继续持有；需要长期保存时由上层复制。

Reference Example 的 `0x88B5` / 1000 Frame 计数属于 Demo 测试逻辑，不进入 Package。

未来 ethernetif 可以在该任务上下文把 Frame 转换为 LwIP pbuf；具体 pbuf ownership 在 LwIP 工作单元再冻结。

## 7. TX submit

当前 TX 使用：

```c
EthernetDriver_TransmitAsync(frame, length);
```

返回：

```text
ETHERNET_TX_QUEUED
→ 已 copy 到 Driver TX DMA Buffer 并提交 HAL

ETHERNET_TX_RETRY
→ 当前临时资源不足，可稍后重试

ETHERNET_TX_ERROR
→ 参数或 Driver 状态错误
```

当前没有软件 TX Queue，也没有阻塞等待 TX completion。

提交链路：

```text
caller frame
→ Driver TX DMA Buffer
→ HAL_ETH_Transmit_IT()
→ DMA
```

`ETHERNET_TX_QUEUED` 返回后 caller 原始 Frame 即可复用。

## 8. TX IRQ 与 completion reclaim

TX DMA complete：

```text
ETH_IRQHandler()
→ HAL_ETH_IRQHandler()
→ HAL_ETH_TxCpltCallback()
→ Driver generic TX event
→ EthernetRtos_OnTxEvent()
→ osThreadFlagsSet(TX)
→ EthernetRtos_RuntimeTask()
```

Runtime Task 收到 TX flag 后：

```text
EthernetDriver_ProcessTxCompletions()
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ Driver TX Buffer recycle
```

`HAL_ETH_TxCpltCallback()` 只通知，不在 ISR 里做 Descriptor reclaim。

当前 `EthernetDriver_TransmitAsync()` 在提交前也会调用一次 `EthernetDriver_ProcessTxCompletions()` 作为 backstop，以回收已经完成但尚未被 Runtime Task 处理的 Packet。

## 9. 单 Runtime Task 的 RX/TX 调度

当前事件位：

```text
RX flag = 1 << 0
TX flag = 1 << 1
```

Task 等待两者任意一个：

```c
osThreadFlagsWait(
    ETHERNET_RX_EVENT_FLAG | ETHERNET_TX_EVENT_FLAG,
    osFlagsWaitAny,
    osWaitForever);
```

同一次唤醒同时存在 RX/TX flag 时，当前先处理 TX completion，再处理 RX drain：

```text
TX reclaim
→ 尽快归还只有 4 个的 TX Buffer

RX drain
→ 读到 ETHERNET_RX_NONE
```

Thread Flag 可能合并多次同类事件，因此：

```text
RX：靠 drain Receive() 消化状态
TX：靠 ReleaseTxPacket() 扫描所有已完成 Packet
```

不依赖“一个 IRQ 对应一次 Task wakeup”。

## 10. TX 并发保护

`HAL_ETH_Transmit_IT()` 与 `HAL_ETH_ReleaseTxPacket()` 会共同操作 HAL TX Descriptor bookkeeping。

当前 Driver 用短 PRIMASK critical section 序列化：

```text
TX Pool acquire/release
HAL_ETH_Transmit_IT()
HAL_ETH_ReleaseTxPacket()
```

Frame copy 不放在整个关中断区。

这样 Driver Core 不需要引入 FreeRTOS mutex，同时避免 TX submit 与 reclaim 并发修改同一套 Descriptor 状态。

## 11. CubeMX Task generation

当前 Reference Example 已采用并验证：

```text
Task Name  : EthernetRuntime
Task Entry : EthernetRtos_RuntimeTask
Generation : As weak
```

CubeMX 6.18.1 的实际生成结果：

- CubeMX 继续生成 Task attributes 和 `osThreadNew()`；
- generated `freertos.c` 提供 `__weak void EthernetRtos_RuntimeTask(void *argument)` stub；
- `Ethernet/RTOS/CMSIS_RTOS2/Src/ethernet_rtos.c` 提供同名强定义；
- 链接时由 Package 强定义承担实际 Runtime Task 逻辑。

该方式已在当前 Reference Example 上完成 Generate Code、Build、async RX 与 async TX 上板回归。

非 CubeMX 用户可以直接使用 CMSIS-RTOS2 / FreeRTOS API 创建 Task，入口同样指向 `EthernetRtos_RuntimeTask()`。

## 12. Runtime registration 边界

当前真正的运行时 Handler 注册有三层接口：

```text
EthernetDriver_SetRxEventHandler()
→ Driver → RTOS Adapter

EthernetDriver_SetTxEventHandler()
→ Driver → RTOS Adapter

EthernetRtos_SetRxFrameHandler()
→ RTOS Adapter → Application / ethernetif
```

前两者由 `EthernetRtos_RuntimeTask()` 内部自动完成，普通使用者通常不需要直接调用；后者是上层决定完整 RX Frame 最终交给谁的接口。

HAL 的 RX/TX callback 属于 HAL 固定 callback，不是通过上述注册接口绑定的函数。

## 13. Error / Link lifecycle

尚未完成：

- DMA fatal / RBU / timeout recovery；
- RX/TX drop/error 统计；
- Link Down 时 MAC stop；
- Link Up 后 speed/duplex reconfigure/start；
- 初始 Auto-negotiation 超时后晚到 Link Up 的完整启动路径；
- async TX 异常路径下的完整 Descriptor/Buffer recovery。

当前 PHY Link 继续轮询，200 ms 只是 bring-up 值。

## 14. LwIP

尚未实现：ethernetif、netif state、ARP、IPv4、Ping、UDP、TCP。

进入 LwIP 后仍保持：

```text
LwIP
→ ethernetif
→ Ethernet Driver / RTOS runtime
```

Driver Core 不引入 LwIP API。

## 15. 当前验证

On-board Verified：

```text
RX:
ETH IRQ
→ Driver RX event
→ CMSIS-RTOS2 Thread Flag
→ Runtime Task RX drain
→ Frame Handler
→ RX Buffer recycle
→ async RX 1000 / 1000

TX:
EthernetDriver_TransmitAsync()
→ HAL_ETH_Transmit_IT()
→ TX IRQ
→ Driver TX event
→ Runtime Task reclaim
→ HAL_ETH_ReleaseTxPacket()
→ HAL_ETH_TxFreeCallback()
→ TX Buffer recycle
→ async TX 1000-frame test PASS
```

RuntimeTask 改名后 RX 1000 / 1000 回归也已通过。

这些结果不代表高负载、吞吐极限或长时间压力测试。
