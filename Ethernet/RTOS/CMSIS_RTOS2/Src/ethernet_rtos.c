#include "ethernet_rtos.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cmsis_os2.h"
#include "ethernet_driver.h"
#include "stm32h7xx_hal.h"

#define ETHERNET_RX_EVENT_FLAG  (1UL << 0)
#define ETHERNET_TX_EVENT_FLAG  (1UL << 1)

#define ETHERNET_EVENT_FLAGS (ETHERNET_RX_EVENT_FLAG | ETHERNET_TX_EVENT_FLAG)

/*
 * 性能诊断专用：
 * 保持原 RX 行为不变，只在热路径中记录 DWT cycle 和事件计数。
 * 报告仅在 RX 连续空闲 250 ms 后打印，避免串口输出干扰压力测试。
 * 完成瓶颈定位后应移除或关闭这段 profiling。
 */
#define ETHERNET_RX_PROFILE_ENABLE          1
#define ETHERNET_RX_PROFILE_IDLE_REPORT_MS 250U

#if ETHERNET_RX_PROFILE_ENABLE
typedef struct
{
    volatile uint32_t rx_irq_events;
    uint32_t rx_runtime_wakeups;

    uint32_t rx_receive_calls;
    uint32_t rx_receive_frames;
    uint32_t rx_receive_none;
    uint32_t rx_receive_errors;
    uint64_t rx_receive_cycles_total;
    uint32_t rx_receive_cycles_max;

    uint32_t rx_handler_calls;
    uint64_t rx_handler_cycles_total;
    uint32_t rx_handler_cycles_max;

    uint32_t reported_rx_frames;
    bool dwt_enabled;
} EthernetRtosRxProfile;
#endif

static uint8_t g_rx_frame[ETHERNET_FRAME_BUFFER_SIZE];

static osThreadId_t g_runtime_task_handle;
static volatile bool g_ready;

static EthernetRtosRxFrameHandler g_rx_frame_handler;
static void *g_rx_frame_handler_context;

#if ETHERNET_RX_PROFILE_ENABLE
static EthernetRtosRxProfile g_rx_profile;

/**
 * @brief 启用 Cortex-M7 DWT cycle counter。
 *
 * @details
 * CYCCNT 只用于性能测量，不参与 Driver 功能逻辑。单次耗时使用 uint32_t
 * 差值计算，即使计数器回绕也能正确处理远小于一次完整回绕周期的测量区间。
 */
static void EthernetRtos_ProfileInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    g_rx_profile.dwt_enabled =
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U;
}

/**
 * @brief 在 RX 已空闲后输出一份累计性能快照。
 *
 * @details
 * 仅当自上次报告后新增了 RX Frame 才打印，因此不会在空闲状态周期刷屏。
 * printf 不在 RX 热路径执行，避免再次制造之前观察到的串口阻塞丢包。
 */
static void EthernetRtos_ProfileReportIfNeeded(void)
{
    uint32_t receive_avg = 0U;
    uint32_t handler_avg = 0U;

    if (g_rx_profile.rx_receive_frames == g_rx_profile.reported_rx_frames)
    {
        return;
    }

    if (g_rx_profile.rx_receive_calls != 0U)
    {
        receive_avg = (uint32_t)(
            g_rx_profile.rx_receive_cycles_total /
            g_rx_profile.rx_receive_calls);
    }

    if (g_rx_profile.rx_handler_calls != 0U)
    {
        handler_avg = (uint32_t)(
            g_rx_profile.rx_handler_cycles_total /
            g_rx_profile.rx_handler_calls);
    }

    printf(
        "[ETH][PROF] dwt=%u irq=%lu wake=%lu "
        "read=%lu frame=%lu none=%lu err=%lu "
        "read_avg=%lu read_max=%lu "
        "handler_avg=%lu handler_max=%lu\r\n",
        g_rx_profile.dwt_enabled ? 1U : 0U,
        (unsigned long)g_rx_profile.rx_irq_events,
        (unsigned long)g_rx_profile.rx_runtime_wakeups,
        (unsigned long)g_rx_profile.rx_receive_calls,
        (unsigned long)g_rx_profile.rx_receive_frames,
        (unsigned long)g_rx_profile.rx_receive_none,
        (unsigned long)g_rx_profile.rx_receive_errors,
        (unsigned long)receive_avg,
        (unsigned long)g_rx_profile.rx_receive_cycles_max,
        (unsigned long)handler_avg,
        (unsigned long)g_rx_profile.rx_handler_cycles_max);

    g_rx_profile.reported_rx_frames = g_rx_profile.rx_receive_frames;
}
#endif

/**
 * @brief  Ethernet Driver RX complete ISR 事件处理。
 */
static void EthernetRtos_OnRxEvent(void *context)
{
    osThreadId_t task_handle = g_runtime_task_handle;

    (void)context;

#if ETHERNET_RX_PROFILE_ENABLE
    /*
     * Driver 每次从 HAL RX complete callback 转发事件时递增一次。
     * 该字段可近似看作 RX complete IRQ 数，用于判断是否存在逐包中断。
     */
    g_rx_profile.rx_irq_events++;
#endif

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
 * 保持已验证的原始 RX 行为：持续调用 EthernetDriver_Receive()，直到当前
 * 无待处理 Frame 或读取失败。Profiling 只测量 Receive() 总耗时和上层
 * handler 耗时，不改变 Buffer ownership、IRQ 配置或 drain 策略。
 */
static void EthernetRtos_ProcessRxFrames(void)
{
    for (;;)
    {
        uint16_t frame_length = 0U;
        EthernetRxResult result;

#if ETHERNET_RX_PROFILE_ENABLE
        uint32_t receive_start = 0U;
        uint32_t receive_cycles = 0U;

        if (g_rx_profile.dwt_enabled)
        {
            receive_start = DWT->CYCCNT;
        }
#endif

        result = EthernetDriver_Receive(
            g_rx_frame,
            sizeof(g_rx_frame),
            &frame_length);

#if ETHERNET_RX_PROFILE_ENABLE
        if (g_rx_profile.dwt_enabled)
        {
            receive_cycles = DWT->CYCCNT - receive_start;
            g_rx_profile.rx_receive_cycles_total += receive_cycles;

            if (receive_cycles > g_rx_profile.rx_receive_cycles_max)
            {
                g_rx_profile.rx_receive_cycles_max = receive_cycles;
            }
        }

        g_rx_profile.rx_receive_calls++;

        if (result == ETHERNET_RX_FRAME)
        {
            g_rx_profile.rx_receive_frames++;
        }
        else if (result == ETHERNET_RX_NONE)
        {
            g_rx_profile.rx_receive_none++;
        }
        else
        {
            g_rx_profile.rx_receive_errors++;
        }
#endif

        if ((result == ETHERNET_RX_NONE) || (result == ETHERNET_RX_ERROR))
        {
            return;
        }

        if (g_rx_frame_handler != NULL)
        {
#if ETHERNET_RX_PROFILE_ENABLE
            uint32_t handler_start = 0U;
            uint32_t handler_cycles = 0U;

            if (g_rx_profile.dwt_enabled)
            {
                handler_start = DWT->CYCCNT;
            }
#endif

            g_rx_frame_handler(
                g_rx_frame,
                frame_length,
                g_rx_frame_handler_context);

#if ETHERNET_RX_PROFILE_ENABLE
            if (g_rx_profile.dwt_enabled)
            {
                handler_cycles = DWT->CYCCNT - handler_start;
                g_rx_profile.rx_handler_cycles_total += handler_cycles;

                if (handler_cycles > g_rx_profile.rx_handler_cycles_max)
                {
                    g_rx_profile.rx_handler_cycles_max = handler_cycles;
                }
            }

            g_rx_profile.rx_handler_calls++;
#endif
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
 *
 * Profiling 开启时，等待使用有限 timeout，仅用于在 RX 停止后从任务上下文
 * 打印统计；收到正常 RX/TX event 时的处理顺序和原始实现保持一致。
 */
void EthernetRtos_RuntimeTask(void *argument)
{
    (void)argument;

    g_runtime_task_handle = osThreadGetId();

#if ETHERNET_RX_PROFILE_ENABLE
    EthernetRtos_ProfileInit();
#endif

    EthernetDriver_SetRxEventHandler(
        EthernetRtos_OnRxEvent,
        NULL);

    EthernetDriver_SetTxEventHandler(
        EthernetRtos_OnTxEvent,
        NULL);

    g_ready = true;

    for (;;)
    {
#if ETHERNET_RX_PROFILE_ENABLE
        uint32_t flags = osThreadFlagsWait(
            ETHERNET_EVENT_FLAGS,
            osFlagsWaitAny,
            ETHERNET_RX_PROFILE_IDLE_REPORT_MS);

        if (flags == osFlagsErrorTimeout)
        {
            EthernetRtos_ProfileReportIfNeeded();
            continue;
        }
#else
        uint32_t flags = osThreadFlagsWait(
            ETHERNET_EVENT_FLAGS,
            osFlagsWaitAny,
            osWaitForever);
#endif

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
#if ETHERNET_RX_PROFILE_ENABLE
            g_rx_profile.rx_runtime_wakeups++;
#endif
            EthernetRtos_ProcessRxFrames();
        }
    }
}
