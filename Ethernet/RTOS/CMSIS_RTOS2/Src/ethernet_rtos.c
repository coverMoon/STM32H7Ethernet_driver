#include "ethernet_rtos.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "ethernet_driver.h"
#include "stm32h7xx_hal.h"

#define ETHERNET_RX_EVENT_FLAG  (1UL << 0)
#define ETHERNET_TX_EVENT_FLAG  (1UL << 1)

#define ETHERNET_EVENT_FLAGS (ETHERNET_RX_EVENT_FLAG | ETHERNET_TX_EVENT_FLAG)

/*
 * 性能诊断专用：
 * - 不改变 RX ownership、IRQ 使能状态或 drain 策略；
 * - 将 ETHERNET_RX_FRAME / ETHERNET_RX_NONE 的 Receive() 耗时分别统计；
 * - 通过 GNU ld --wrap=HAL_ETH_ReadData 单独测量 HAL_ETH_ReadData()；
 * - 仅在 RX 连续空闲 250 ms 后打印并清空本轮统计，避免串口干扰热路径。
 *
 * 当前计数面向 20 万帧量级的短时压力测试，cycle 累计使用 uint32_t；
 * 每次报告后都会清零，避免长时间累计溢出。
 */
#define ETHERNET_RX_PROFILE_ENABLE          1
#define ETHERNET_RX_PROFILE_IDLE_REPORT_MS 250U

#if ETHERNET_RX_PROFILE_ENABLE
typedef struct
{
    volatile uint32_t rx_irq_events;
    uint32_t rx_runtime_wakeups;

    uint32_t frame_read_calls;
    uint32_t frame_read_cycles_total;
    uint32_t frame_read_cycles_max;

    uint32_t none_read_calls;
    uint32_t none_read_cycles_total;
    uint32_t none_read_cycles_max;

    uint32_t error_read_calls;
    uint32_t error_read_cycles_total;
    uint32_t error_read_cycles_max;

    uint32_t handler_calls;
    uint32_t handler_cycles_total;
    uint32_t handler_cycles_max;

    uint32_t hal_frame_calls;
    uint32_t hal_frame_cycles_total;
    uint32_t hal_frame_cycles_max;

    uint32_t hal_none_calls;
    uint32_t hal_none_cycles_total;
    uint32_t hal_none_cycles_max;

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
static uint32_t g_reported_hal_error_events;

/**
 * @brief 累计一次 cycle 测量结果。
 *
 * @details
 * 使用 force-inline 避免在 Debug -O0 下为了 profiling 再引入一次函数调用。
 */
__STATIC_FORCEINLINE void EthernetRtos_ProfileAccumulate(
    uint32_t cycles,
    uint32_t *calls,
    uint32_t *total,
    uint32_t *maximum)
{
    (*calls)++;
    (*total) += cycles;

    if (cycles > *maximum)
    {
        *maximum = cycles;
    }
}

/**
 * @brief 启用 Cortex-M7 DWT cycle counter。
 *
 * @details
 * CYCCNT 只用于性能测量，不参与 Driver 功能逻辑。单次耗时使用 uint32_t
 * 差值计算，即使计数器回绕也能正确处理远小于一次完整回绕周期的测量区间。
 */
static void EthernetRtos_ProfileInit(void)
{
    memset(&g_rx_profile, 0, sizeof(g_rx_profile));

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    g_rx_profile.dwt_enabled =
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U;
}

/**
 * @brief 对本轮 profiling 做原子快照并清零热路径计数。
 *
 * @details
 * RX event 计数会在 ISR 中更新，因此快照期间短暂关闭中断。这里只复制几十字节
 * 软件状态，不执行 printf，也不做任何可能阻塞的操作。
 */
static bool EthernetRtos_ProfileTakeSnapshot(EthernetRtosRxProfile *snapshot)
{
    uint32_t primask;
    bool dwt_enabled;

    if (snapshot == NULL)
    {
        return false;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    if ((g_rx_profile.frame_read_calls == 0U) &&
        (g_rx_profile.none_read_calls == 0U) &&
        (g_rx_profile.error_read_calls == 0U))
    {
        __set_PRIMASK(primask);
        return false;
    }

    *snapshot = g_rx_profile;
    dwt_enabled = g_rx_profile.dwt_enabled;

    memset(&g_rx_profile, 0, sizeof(g_rx_profile));
    g_rx_profile.dwt_enabled = dwt_enabled;

    __set_PRIMASK(primask);

    return true;
}

/**
 * @brief 在 RX 空闲后输出一轮性能快照。
 *
 * @details
 * 第一行按 Receive() 最终结果区分 FRAME / NONE / ERROR；第二行只统计
 * HAL_ETH_ReadData() 本身。other_frame_avg / other_none_avg 是两层 wall-cycle
 * 均值之差，可近似表示 Driver 参数检查、返回路径、第二次 memcpy、统计更新以及
 * profiling wrapper 自身的少量开销，不应解释为某一个单独函数的精确耗时。
 */
static void EthernetRtos_ProfileReportIfNeeded(void)
{
    EthernetRtosRxProfile profile = {0};
    EthernetDriverStats driver_stats = {0};
    uint32_t frame_avg = 0U;
    uint32_t none_avg = 0U;
    uint32_t error_avg = 0U;
    uint32_t handler_avg = 0U;
    uint32_t hal_frame_avg = 0U;
    uint32_t hal_none_avg = 0U;
    uint32_t other_frame_avg = 0U;
    uint32_t other_none_avg = 0U;
    uint32_t hal_error_delta = 0U;

    if (!EthernetRtos_ProfileTakeSnapshot(&profile))
    {
        return;
    }

    if (profile.frame_read_calls != 0U)
    {
        frame_avg = profile.frame_read_cycles_total / profile.frame_read_calls;
    }

    if (profile.none_read_calls != 0U)
    {
        none_avg = profile.none_read_cycles_total / profile.none_read_calls;
    }

    if (profile.error_read_calls != 0U)
    {
        error_avg = profile.error_read_cycles_total / profile.error_read_calls;
    }

    if (profile.handler_calls != 0U)
    {
        handler_avg = profile.handler_cycles_total / profile.handler_calls;
    }

    if (profile.hal_frame_calls != 0U)
    {
        hal_frame_avg = profile.hal_frame_cycles_total / profile.hal_frame_calls;
    }

    if (profile.hal_none_calls != 0U)
    {
        hal_none_avg = profile.hal_none_cycles_total / profile.hal_none_calls;
    }

    if (frame_avg > hal_frame_avg)
    {
        other_frame_avg = frame_avg - hal_frame_avg;
    }

    if (none_avg > hal_none_avg)
    {
        other_none_avg = none_avg - hal_none_avg;
    }

    if (EthernetDriver_GetStats(&driver_stats))
    {
        hal_error_delta =
            driver_stats.hal_error_events - g_reported_hal_error_events;
        g_reported_hal_error_events = driver_stats.hal_error_events;
    }

    printf(
        "[ETH][PROF] dwt=%u irq=%lu wake=%lu "
        "frame=%lu frame_avg=%lu frame_max=%lu "
        "none=%lu none_avg=%lu none_max=%lu "
        "err=%lu err_avg=%lu err_max=%lu "
        "handler_avg=%lu handler_max=%lu\r\n",
        profile.dwt_enabled ? 1U : 0U,
        (unsigned long)profile.rx_irq_events,
        (unsigned long)profile.rx_runtime_wakeups,
        (unsigned long)profile.frame_read_calls,
        (unsigned long)frame_avg,
        (unsigned long)profile.frame_read_cycles_max,
        (unsigned long)profile.none_read_calls,
        (unsigned long)none_avg,
        (unsigned long)profile.none_read_cycles_max,
        (unsigned long)profile.error_read_calls,
        (unsigned long)error_avg,
        (unsigned long)profile.error_read_cycles_max,
        (unsigned long)handler_avg,
        (unsigned long)profile.handler_cycles_max);

    printf(
        "[ETH][PROFHAL] hal_frame=%lu hal_frame_avg=%lu hal_frame_max=%lu "
        "hal_none=%lu hal_none_avg=%lu hal_none_max=%lu "
        "other_frame_avg=%lu other_none_avg=%lu "
        "halerr=%lu dma=0x%08lX\r\n",
        (unsigned long)profile.hal_frame_calls,
        (unsigned long)hal_frame_avg,
        (unsigned long)profile.hal_frame_cycles_max,
        (unsigned long)profile.hal_none_calls,
        (unsigned long)hal_none_avg,
        (unsigned long)profile.hal_none_cycles_max,
        (unsigned long)other_frame_avg,
        (unsigned long)other_none_avg,
        (unsigned long)hal_error_delta,
        (unsigned long)driver_stats.last_dma_error_code);
}

/*
 * GNU ld --wrap 入口。
 * CMake 通过 --wrap=HAL_ETH_ReadData 将 Driver 对 HAL_ETH_ReadData() 的调用
 * 重定向到这里，再由 __real_HAL_ETH_ReadData() 调用 ST HAL 原实现。
 * 这样可以测量 HAL 层而不修改 ST HAL 或 Ethernet Driver 热路径源码。
 */
HAL_StatusTypeDef __real_HAL_ETH_ReadData(
    ETH_HandleTypeDef *heth,
    void **pAppBuff);

HAL_StatusTypeDef __wrap_HAL_ETH_ReadData(
    ETH_HandleTypeDef *heth,
    void **pAppBuff)
{
    HAL_StatusTypeDef status;
    uint32_t start = 0U;
    uint32_t cycles = 0U;

    if (g_rx_profile.dwt_enabled)
    {
        start = DWT->CYCCNT;
    }

    status = __real_HAL_ETH_ReadData(heth, pAppBuff);

    if (g_rx_profile.dwt_enabled)
    {
        cycles = DWT->CYCCNT - start;
    }

    if (status == HAL_OK)
    {
        EthernetRtos_ProfileAccumulate(
            cycles,
            &g_rx_profile.hal_frame_calls,
            &g_rx_profile.hal_frame_cycles_total,
            &g_rx_profile.hal_frame_cycles_max);
    }
    else
    {
        EthernetRtos_ProfileAccumulate(
            cycles,
            &g_rx_profile.hal_none_calls,
            &g_rx_profile.hal_none_cycles_total,
            &g_rx_profile.hal_none_cycles_max);
    }

    return status;
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
 * 无待处理 Frame 或读取失败。Profiling 按返回结果分别统计 Receive() 的
 * wall-cycle，并单独统计上层 handler，不改变正常收包流程。
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
        }

        if (result == ETHERNET_RX_FRAME)
        {
            EthernetRtos_ProfileAccumulate(
                receive_cycles,
                &g_rx_profile.frame_read_calls,
                &g_rx_profile.frame_read_cycles_total,
                &g_rx_profile.frame_read_cycles_max);
        }
        else if (result == ETHERNET_RX_NONE)
        {
            EthernetRtos_ProfileAccumulate(
                receive_cycles,
                &g_rx_profile.none_read_calls,
                &g_rx_profile.none_read_cycles_total,
                &g_rx_profile.none_read_cycles_max);
        }
        else
        {
            EthernetRtos_ProfileAccumulate(
                receive_cycles,
                &g_rx_profile.error_read_calls,
                &g_rx_profile.error_read_cycles_total,
                &g_rx_profile.error_read_cycles_max);
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
            }

            EthernetRtos_ProfileAccumulate(
                handler_cycles,
                &g_rx_profile.handler_calls,
                &g_rx_profile.handler_cycles_total,
                &g_rx_profile.handler_cycles_max);
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
