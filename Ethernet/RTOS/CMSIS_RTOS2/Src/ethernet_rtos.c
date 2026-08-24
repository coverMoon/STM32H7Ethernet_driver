#include "ethernet_rtos.h"

#include <stddef.h>

#include "cmsis_os2.h"
#include "ethernet_driver.h"

#define ETHERNET_RX_EVENT_FLAG  (1UL << 0)
#define ETHERNET_TX_EVENT_FLAG  (1UL << 1)

#define ETHERNET_EVENT_FLAGS (ETHERNET_RX_EVENT_FLAG | ETHERNET_TX_EVENT_FLAG)

static osThreadId_t g_runtime_task_handle;
static volatile bool g_ready;

static EthernetRtosRxFrameHandler g_rx_frame_handler;
static void *g_rx_frame_handler_context;

/**
 * @brief  Ethernet Driver RX complete ISR 事件处理。
 */
static void EthernetRtos_OnRxEvent(void *context)
{
    osThreadId_t task_handle = g_runtime_task_handle;

    (void)context;

    if (task_handle != NULL)
    {
        (void)osThreadFlagsSet(
            task_handle,
            ETHERNET_RX_EVENT_FLAG);
    }
}

/**
 * @brief  Ethernet Driver TX complete ISR 事件处理。
 */
static void EthernetRtos_OnTxEvent(void *context)
{
    osThreadId_t task_handle = g_runtime_task_handle;

    (void)context;

    if (task_handle != NULL)
    {
        (void)osThreadFlagsSet(
            task_handle,
            ETHERNET_TX_EVENT_FLAG);
    }
}

/**
 * @brief  处理当前所有可读取的 RX Frame。
 *
 * @details
 * 直接取得 Driver CPU 侧 Frame 视图并同步交给上层，避免此前
 * Driver CPU Frame -> RTOS Adapter Frame 的第二次 memcpy。
 *
 * Frame view 只在下一次 Driver RX 读取前有效，因此 Handler 必须在本次
 * 调用内完成同步消费，不能缓存 frame 指针供其他任务稍后使用。
 */
static void EthernetRtos_ProcessRxFrames(void)
{
    for (;;)
    {
        const uint8_t *frame = NULL;
        uint16_t frame_length = 0U;
        EthernetRxResult result = EthernetDriver_ReceiveView(&frame, &frame_length);

        if ((result == ETHERNET_RX_NONE) || (result == ETHERNET_RX_ERROR))
        {
            return;
        }

        if (g_rx_frame_handler != NULL)
        {
            g_rx_frame_handler(frame, frame_length, g_rx_frame_handler_context);
        }
    }
}

/**
 * @brief  设置任务上下文中的完整 Frame 处理函数。
 *
 * @details
 * 建议在 MAC/DMA 启动前完成设置。传入 NULL 可停止向上层转交 Frame。
 *
 * @param[in] handler Frame 处理函数。
 * @param[in] context 调用处理函数时传入的用户上下文。
 */
void EthernetRtos_SetRxFrameHandler(EthernetRtosRxFrameHandler handler, void *context)
{
    g_rx_frame_handler_context = context;
    g_rx_frame_handler = handler;
}

/**
 * @brief  查询 Ethernet Runtime Task 是否已完成事件绑定。
 *
 * @retval true   Runtime Task 已就绪。
 * @retval false  Runtime Task 尚未就绪。
 */
bool EthernetRtos_IsReady(void)
{
    return g_ready;
}

/**
 * @brief  CMSIS-RTOS2 Ethernet Runtime Task 入口。
 *
 * @details
 * Task 创建、priority、stack 和 allocation 仍由应用/CubeMX 管理。
 * 本任务负责 RX deferred processing 和 TX completion reclaim。
 */
void EthernetRtos_RuntimeTask(void *argument)
{
    (void)argument;

    g_runtime_task_handle = osThreadGetId();

    EthernetDriver_SetRxEventHandler(
        EthernetRtos_OnRxEvent,
        NULL);

    EthernetDriver_SetTxEventHandler(
        EthernetRtos_OnTxEvent,
        NULL);

    g_ready = true;

    for (;;)
    {
        uint32_t flags = osThreadFlagsWait(ETHERNET_EVENT_FLAGS, osFlagsWaitAny, osWaitForever);

        if ((flags & osFlagsError) != 0U)
        {
            continue;
        }

        /*
         * 优先回收 TX Buffer，缩短 Buffer 占用时间。
         */
        if ((flags & ETHERNET_TX_EVENT_FLAG) != 0U)
        {
            EthernetDriver_ProcessTxCompletions();
        }

        if ((flags & ETHERNET_RX_EVENT_FLAG) != 0U)
        {
            EthernetRtos_ProcessRxFrames();
        }
    }
}