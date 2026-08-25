#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "ethernet_driver.h"
#include "ethernet_port.h"

#ifndef ETHERNET_TX_BENCHMARK_FRAME_SIZE
#define ETHERNET_TX_BENCHMARK_FRAME_SIZE        60U
#endif

#ifndef ETHERNET_TX_BENCHMARK_FRAME_COUNT
#define ETHERNET_TX_BENCHMARK_FRAME_COUNT   200000U
#endif
#define ETHERNET_TX_BENCHMARK_START_DELAY_MS  2000U
#define ETHERNET_TX_BENCHMARK_TIMEOUT_MS     60000U
#define ETHERNET_TX_BENCHMARK_ETHERTYPE     0x88B5U

#define ETHERNET_HEADER_SIZE                    14U
#define ETHERNET_FCS_SIZE                        4U
#define ETHERNET_PREAMBLE_SFD_SIZE               8U
#define ETHERNET_IFG_SIZE                       12U

static const uint8_t g_tx_benchmark_marker[] = "STM32H7 TX BENCH";
static uint8_t g_tx_benchmark_frame[ETHERNET_FRAME_BUFFER_SIZE];

_Static_assert(ETHERNET_TX_BENCHMARK_FRAME_SIZE >= 60U,
               "TX benchmark frame must be at least 60 bytes");
_Static_assert(ETHERNET_TX_BENCHMARK_FRAME_SIZE <= ETHERNET_FRAME_BUFFER_SIZE,
               "TX benchmark frame exceeds Driver frame buffer size");
_Static_assert(
    ETHERNET_TX_BENCHMARK_FRAME_SIZE >=
        (ETHERNET_HEADER_SIZE + sizeof(g_tx_benchmark_marker) - 1U + 4U),
    "TX benchmark frame is too small for marker and sequence");

bool __real_EthernetDriver_Start(void);

static void EthernetTxBenchmark_BuildFrame(void)
{
    ETH_HandleTypeDef *eth_handle = EthernetPort_GetHandle();
    uint8_t *frame = g_tx_benchmark_frame;

    memset(frame, 0xA5, ETHERNET_TX_BENCHMARK_FRAME_SIZE);

    memset(frame, 0xFF, 6U);

    if ((eth_handle != NULL) && (eth_handle->Init.MACAddr != NULL))
    {
        memcpy(&frame[6], eth_handle->Init.MACAddr, 6U);
    }
    else
    {
        memset(&frame[6], 0, 6U);
    }

    frame[12] = (uint8_t)(ETHERNET_TX_BENCHMARK_ETHERTYPE >> 8);
    frame[13] = (uint8_t)ETHERNET_TX_BENCHMARK_ETHERTYPE;

    memcpy(
        &frame[ETHERNET_HEADER_SIZE],
        g_tx_benchmark_marker,
        sizeof(g_tx_benchmark_marker) - 1U);
}

static void EthernetTxBenchmark_SetSequence(uint32_t sequence)
{
    uint32_t offset = ETHERNET_TX_BENCHMARK_FRAME_SIZE - 4U;
    uint8_t *frame = g_tx_benchmark_frame;

    frame[offset] = (uint8_t)(sequence >> 24);
    frame[offset + 1U] = (uint8_t)(sequence >> 16);
    frame[offset + 2U] = (uint8_t)(sequence >> 8);
    frame[offset + 3U] = (uint8_t)sequence;
}

static uint32_t EthernetTxBenchmark_TimeoutTicks(uint32_t tick_frequency)
{
    uint64_t ticks =
        ((uint64_t)ETHERNET_TX_BENCHMARK_TIMEOUT_MS * tick_frequency + 999U) /
        1000U;

    if (ticks > UINT32_MAX)
    {
        return UINT32_MAX;
    }

    return (uint32_t)ticks;
}

static bool EthernetTxBenchmark_Run(void)
{
    EthernetDriverStats stats_before = {0};
    EthernetDriverStats stats_after = {0};

    uint32_t queued = 0U;
    uint32_t retry_checks = 0U;
    uint32_t start_tick;
    uint32_t end_tick;
    uint32_t elapsed_ticks;
    uint32_t tick_frequency;
    uint32_t timeout_ticks;

    uint32_t queued_delta;
    uint32_t completed_delta;
    uint32_t retry_delta;
    uint32_t error_delta;
    uint32_t hal_error_delta;

    uint32_t pps = 0U;
    uint32_t frame_kbps = 0U;
    uint32_t on_wire_kbps = 0U;

    if (!EthernetDriver_GetStats(&stats_before))
    {
        printf("[ETH][TXBENCH] failed to read stats baseline\r\n");
        return false;
    }

    tick_frequency = osKernelGetTickFreq();

    if (tick_frequency == 0U)
    {
        printf("[ETH][TXBENCH] invalid RTOS tick frequency\r\n");
        return false;
    }

    timeout_ticks = EthernetTxBenchmark_TimeoutTicks(tick_frequency);

    EthernetTxBenchmark_BuildFrame();

    printf(
        "[ETH][TXBENCH] size=%lu count=%lu start_delay=%lu ms\r\n",
        (unsigned long)ETHERNET_TX_BENCHMARK_FRAME_SIZE,
        (unsigned long)ETHERNET_TX_BENCHMARK_FRAME_COUNT,
        (unsigned long)ETHERNET_TX_BENCHMARK_START_DELAY_MS);

    osDelay(ETHERNET_TX_BENCHMARK_START_DELAY_MS);

    start_tick = osKernelGetTickCount();

    while (queued < ETHERNET_TX_BENCHMARK_FRAME_COUNT)
    {
        EthernetTxResult result;

        EthernetTxBenchmark_SetSequence(queued);

        result = EthernetDriver_TransmitAsync(
            g_tx_benchmark_frame,
            ETHERNET_TX_BENCHMARK_FRAME_SIZE);

        if (result == ETHERNET_TX_QUEUED)
        {
            queued++;
            continue;
        }

        if (result == ETHERNET_TX_ERROR)
        {
            printf(
                "[ETH][TXBENCH] driver error at sequence=%lu\r\n",
                (unsigned long)queued);
            return false;
        }

        /*
         * RETRY 是 benchmark 中预期的 backpressure 信号。
         * 不 sleep，保持 busy producer；只低频检查 timeout，避免计时逻辑
         * 本身成为小包热路径的主要开销。
         */
        retry_checks++;

        if ((retry_checks & 0x3FFU) == 0U)
        {
            uint32_t now_tick = osKernelGetTickCount();

            if ((uint32_t)(now_tick - start_tick) >= timeout_ticks)
            {
                printf(
                    "[ETH][TXBENCH] submit timeout at queued=%lu\r\n",
                    (unsigned long)queued);
                return false;
            }
        }
    }

    for (;;)
    {
        uint32_t completed;

        if (!EthernetDriver_GetStats(&stats_after))
        {
            printf("[ETH][TXBENCH] failed to read completion stats\r\n");
            return false;
        }

        completed = stats_after.tx_completed - stats_before.tx_completed;

        if (completed >= ETHERNET_TX_BENCHMARK_FRAME_COUNT)
        {
            break;
        }

        if ((uint32_t)(osKernelGetTickCount() - start_tick) >= timeout_ticks)
        {
            printf(
                "[ETH][TXBENCH] completion timeout at completed=%lu\r\n",
                (unsigned long)completed);
            return false;
        }

        osDelay(1U);
    }

    end_tick = osKernelGetTickCount();
    elapsed_ticks = end_tick - start_tick;

    if (!EthernetDriver_GetStats(&stats_after))
    {
        return false;
    }

    queued_delta = stats_after.tx_queued - stats_before.tx_queued;
    completed_delta = stats_after.tx_completed - stats_before.tx_completed;
    retry_delta = stats_after.tx_retries - stats_before.tx_retries;
    error_delta = stats_after.tx_errors - stats_before.tx_errors;
    hal_error_delta = stats_after.hal_error_events - stats_before.hal_error_events;

    if (elapsed_ticks > 0U)
    {
        uint64_t packets_per_second =
            ((uint64_t)completed_delta * tick_frequency) / elapsed_ticks;
        uint64_t frame_bits_per_second =
            packets_per_second * ETHERNET_TX_BENCHMARK_FRAME_SIZE * 8U;
        uint64_t on_wire_bits_per_second =
            packets_per_second *
            (ETHERNET_TX_BENCHMARK_FRAME_SIZE +
             ETHERNET_FCS_SIZE +
             ETHERNET_PREAMBLE_SFD_SIZE +
             ETHERNET_IFG_SIZE) *
            8U;

        pps = (uint32_t)packets_per_second;
        frame_kbps = (uint32_t)(frame_bits_per_second / 1000U);
        on_wire_kbps = (uint32_t)(on_wire_bits_per_second / 1000U);
    }

    printf(
        "[ETH][TXBENCH] queued=%lu completed=%lu retry=%lu "
        "error=%lu hal=%lu dma=0x%08lX mac=0x%08lX\r\n",
        (unsigned long)queued_delta,
        (unsigned long)completed_delta,
        (unsigned long)retry_delta,
        (unsigned long)error_delta,
        (unsigned long)hal_error_delta,
        (unsigned long)stats_after.last_dma_error_code,
        (unsigned long)stats_after.last_mac_error_code);

    printf(
        "[ETH][TXBENCH] elapsed_ticks=%lu tick_hz=%lu pps=%lu "
        "frame_kbps=%lu onwire_kbps=%lu\r\n",
        (unsigned long)elapsed_ticks,
        (unsigned long)tick_frequency,
        (unsigned long)pps,
        (unsigned long)frame_kbps,
        (unsigned long)on_wire_kbps);

    return (queued_delta == ETHERNET_TX_BENCHMARK_FRAME_COUNT) &&
           (completed_delta == ETHERNET_TX_BENCHMARK_FRAME_COUNT) &&
           (error_delta == 0U);
}

/**
 * @brief Test-only linker wrapper around EthernetDriver_Start().
 *
 * The normal Driver start runs first. Once MAC/DMA is active, this Reference
 * Example executes one TX throughput benchmark in BootstrapTask context.
 */
bool __wrap_EthernetDriver_Start(void)
{
    bool started = __real_EthernetDriver_Start();

    if (!started)
    {
        return false;
    }

    if (EthernetTxBenchmark_Run())
    {
        printf("[ETH][TXBENCH] PASS\r\n");
    }
    else
    {
        printf("[ETH][TXBENCH] FAIL\r\n");
    }

    return true;
}
