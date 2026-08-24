#include "ethernet_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ethernet_port.h"
#include "stm32h7xx_hal.h"

#define ETHERNET_DMA_BUFFER_SIZE  ETHERNET_FRAME_BUFFER_SIZE
#define ETHERNET_DMA_ALIGNMENT    32U

/**
 * @brief CPU 侧单帧接收暂存区。
 *
 * DMA RX Buffer 中的数据在 HAL_ETH_RxLinkCallback() 中复制到这里，
 * 因此上层不会持有 DMA Buffer。
 */
typedef struct
{
    uint32_t length;
    bool valid;
    uint8_t data[ETHERNET_DMA_BUFFER_SIZE];
} EthernetRxFrameStorage;

static uint8_t g_rx_dma_buffers[ETH_RX_DESC_CNT][ETHERNET_DMA_BUFFER_SIZE]
    __attribute__((section(".eth_dma_buffer.rx"),
                   aligned(ETHERNET_DMA_ALIGNMENT),
                   used));

static uint8_t g_tx_dma_buffers[ETH_TX_DESC_CNT][ETHERNET_DMA_BUFFER_SIZE]
    __attribute__((section(".eth_dma_buffer.tx"),
                   aligned(ETHERNET_DMA_ALIGNMENT),
                   used));

static bool g_rx_buffer_in_use[ETH_RX_DESC_CNT];
static bool g_tx_buffer_in_use[ETH_TX_DESC_CNT];

static EthernetRxFrameStorage g_rx_frame;

static EthernetDriverRxEventHandler g_rx_event_handler;
static void *g_rx_event_context;
static EthernetDriverTxEventHandler g_tx_event_handler;
static void *g_tx_event_context;

static EthernetDriverStats g_stats;

_Static_assert((ETHERNET_DMA_BUFFER_SIZE % ETHERNET_DMA_ALIGNMENT) == 0U,
               "Ethernet DMA buffer size must be cache-line aligned");

/**
 * @brief 进入 Ethernet Driver 内部短临界区。
 *
 * @details
 * 用于保护 Driver 中会被 Task / ISR 并发访问的短时软件状态。
 * 临界区内不得执行等待、日志输出或大块数据复制。
 *
 * @return 进入临界区前的 PRIMASK，用于恢复原中断状态。
 */
static uint32_t EthernetDriver_EnterCritical(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    return primask;
}

/**
 * @brief 离开 Ethernet Driver 内部短临界区。
 *
 * @param[in] primask 进入临界区前保存的 PRIMASK。
 */
static void EthernetDriver_ExitCritical(uint32_t primask)
{
    __set_PRIMASK(primask);
}

/**
 * @brief 原子增加一个 Driver 统计计数器。
 *
 * @param[in,out] counter 待增加的统计字段。
 */
static void EthernetDriver_IncrementCounter(uint32_t *counter)
{
    uint32_t primask;

    if (counter == NULL)
    {
        return;
    }

    primask = EthernetDriver_EnterCritical();

    (*counter)++;

    EthernetDriver_ExitCritical(primask);
}

/**
 * @brief 将当前正在组装的 RX Frame 标记为无效。
 *
 * @details
 * 一个 Frame 即使跨多个 Descriptor 出现多个异常，也只统计一次 drop。
 */
static void EthernetDriver_InvalidateRxFrame(void)
{
    if (!g_rx_frame.valid)
    {
        return;
    }

    g_rx_frame.valid = false;

    EthernetDriver_IncrementCounter(&g_stats.rx_dropped);
}

/**
 * @brief 判断 HAL 当前 RX Descriptor 是否已经交还给 CPU。
 *
 * @details
 * HAL_ETH_IRQHandler() 在调用 HAL_ETH_RxCpltCallback() 前会清 RI。
 * Runtime Task 完成一轮 drain 后重新打开 RIE 时，之前被屏蔽期间留下的
 * RI 可能立即触发一次“旧中断”。此时通过当前 Descriptor 的 OWN 位判断
 * 是否真的还有新 Frame 等待处理，避免无意义地再次唤醒 Runtime Task。
 *
 * @param[in] eth_handle Ethernet HAL Handle。
 *
 * @retval true   当前 Descriptor 已由 DMA 完成，CPU 可以继续读取。
 * @retval false  当前 Descriptor 仍归 DMA 所有，暂时没有待处理数据。
 */
static bool EthernetDriver_IsRxDescriptorReady(const ETH_HandleTypeDef *eth_handle)
{
    uint32_t desc_index;
    ETH_DMADescTypeDef *descriptor;

    if (eth_handle == NULL)
    {
        return false;
    }

    desc_index = eth_handle->RxDescList.RxDescIdx;

    if ((desc_index >= ETH_RX_DESC_CNT) || (eth_handle->RxDescList.RxDesc[desc_index] == 0U))
    {
        return false;
    }

    descriptor = (ETH_DMADescTypeDef *)eth_handle->RxDescList.RxDesc[desc_index];

    /*
     * RX Descriptor 位于 non-cacheable DMA SRAM。DMB 只保证这里的读取
     * 不越过前面的外设/内存访问，不承担 cache maintenance 职责。
     */
    __DMB();

    return READ_BIT(descriptor->DESC3, ETH_DMARXNDESCWBF_OWN) == 0U;
}

/**
 * @brief  释放一个 RX DMA Buffer。
 *
 * @param[in] buffer RX DMA Buffer 首地址。
 *
 * @retval true   Buffer 属于 RX Pool，已释放。
 * @retval false  Buffer 不属于 RX Pool。
 */
static bool EthernetDriver_ReleaseRxBuffer(uint8_t *buffer)
{
    for (uint32_t i = 0U; i < ETH_RX_DESC_CNT; i++)
    {
        if (buffer == g_rx_dma_buffers[i])
        {
            g_rx_buffer_in_use[i] = false;
            return true;
        }
    }

    return false;
}

/**
 * @brief  获取一个空闲 TX DMA Buffer。
 *
 * @retval 非 NULL  空闲 TX Buffer 地址。
 * @retval NULL      当前没有可用 TX Buffer。
 */
static uint8_t *EthernetDriver_AcquireTxBuffer(void)
{
    uint8_t *buffer = NULL;
    uint32_t primask = EthernetDriver_EnterCritical();

    for (uint32_t i = 0U; i < ETH_TX_DESC_CNT; i++)
    {
        if (!g_tx_buffer_in_use[i])
        {
            g_tx_buffer_in_use[i] = true;
            buffer = g_tx_dma_buffers[i];
            break;
        }
    }

    EthernetDriver_ExitCritical(primask);

    return buffer;
}

/**
 * @brief 释放一个 TX DMA Buffer。
 *
 * @param[in] buffer TX DMA Buffer 地址。
 *
 * @retval true   Buffer 属于 TX Pool，已成功释放。
 * @retval false  Buffer 不属于 TX Pool。
 */
static bool EthernetDriver_ReleaseTxBuffer(uint8_t *buffer)
{
    bool released = false;
    uint32_t primask = EthernetDriver_EnterCritical();

    for (uint32_t i = 0U; i < ETH_TX_DESC_CNT; i++)
    {
        if (buffer == g_tx_dma_buffers[i])
        {
            g_tx_buffer_in_use[i] = false;
            released = true;
            break;
        }
    }

    EthernetDriver_ExitCritical(primask);

    return released;
}

/**
 * @brief  将一个 RX DMA Buffer 片段复制到 CPU 侧帧暂存区。
 *
 * @param[in] buffer RX 数据地址。
 * @param[in] length 数据长度。
 *
 * @retval true   数据复制成功。
 * @retval false  参数无效或帧长度超过暂存区容量。
 */
static bool EthernetDriver_AppendRxData(const uint8_t *buffer, uint16_t length)
{
    if (buffer == NULL)
    {
        return false;
    }

    if ((g_rx_frame.length + length) > sizeof(g_rx_frame.data))
    {
        return false;
    }

    memcpy(&g_rx_frame.data[g_rx_frame.length], buffer, length);
    g_rx_frame.length += length;

    return true;
}

/**
 * @brief  初始化 Ethernet Driver 的软件 Buffer ownership 状态。
 *
 * @details
 * 不初始化 DMA 硬件，只清理 RX/TX Buffer Pool 和 RX event handler 的软件状态。
 * 必须在 Ethernet MAC/DMA Start 前调用。
 */
void EthernetDriver_Init(void)
{
    memset(g_rx_buffer_in_use, 0, sizeof(g_rx_buffer_in_use));
    memset(g_tx_buffer_in_use, 0, sizeof(g_tx_buffer_in_use));
    memset(&g_stats, 0, sizeof(g_stats));

    g_rx_frame.length = 0U;
    g_rx_frame.valid = false;

    g_rx_event_handler = NULL;
    g_rx_event_context = NULL;

    g_tx_event_handler = NULL;
    g_tx_event_context = NULL;
}

/**
 * @brief 获取 Ethernet Driver 当前统计快照。
 *
 * @details
 * 使用短临界区保证读取过程中不会被 ETH ISR 或其他 Task 更新一半。
 *
 * @param[out] stats 统计结果输出。
 *
 * @retval true   获取成功。
 * @retval false  参数无效。
 */
bool EthernetDriver_GetStats(EthernetDriverStats *stats)
{
    uint32_t primask;

    if (stats == NULL)
    {
        return false;
    }

    primask = EthernetDriver_EnterCritical();

    *stats = g_stats;

    EthernetDriver_ExitCritical(primask);

    return true;
}

/**
 * @brief  注册 RX complete ISR 事件处理函数。
 *
 * @details
 * 建议在 MAC/DMA Start 前完成注册。handler 在 ISR 上下文执行，
 * 只能进行轻量事件转发或 RTOS FromISR-safe 通知。
 */
void EthernetDriver_SetRxEventHandler(EthernetDriverRxEventHandler handler, void *context)
{
    if (handler == NULL)
    {
        g_rx_event_handler = NULL;
        g_rx_event_context = NULL;
        return;
    }

    g_rx_event_context = context;
    g_rx_event_handler = handler;
}

/**
 * @brief  注册 TX complete ISR 事件处理函数。
 *
 * @details
 * 建议在 MAC/DMA 启动前完成注册。handler 在 ISR 上下文执行，
 * 只能进行轻量事件转发或 RTOS FromISR-safe 通知。传入 NULL 可取消注册。
 *
 * @param[in] handler TX complete 事件处理函数。
 * @param[in] context 调用处理函数时传入的用户上下文。
 */
void EthernetDriver_SetTxEventHandler(EthernetDriverTxEventHandler handler, void *context)
{
    if (handler == NULL)
    {
        g_tx_event_handler = NULL;
        g_tx_event_context = NULL;
        return;
    }

    g_tx_event_context = context;
    g_tx_event_handler = handler;
}

/**
 * @brief  根据 PHY 协商结果配置 Ethernet MAC。
 *
 * @param[in] speed   链路速率。
 * @param[in] duplex  双工模式。
 *
 * @retval true   MAC 配置成功。
 * @retval false  Port、参数或 HAL 状态无效。
 */
bool EthernetDriver_ConfigureLink(EthernetLinkSpeed speed, EthernetDuplexMode duplex)
{
    ETH_HandleTypeDef *eth_handle = EthernetPort_GetHandle();
    ETH_MACConfigTypeDef mac_config = {0};

    if ((eth_handle == NULL) || (eth_handle->gState != HAL_ETH_STATE_READY))
    {
        return false;
    }

    if (HAL_ETH_GetMACConfig(eth_handle, &mac_config) != HAL_OK)
    {
        return false;
    }

    switch (speed)
    {
        case ETHERNET_LINK_SPEED_10M:
            mac_config.Speed = ETH_SPEED_10M;
            break;

        case ETHERNET_LINK_SPEED_100M:
            mac_config.Speed = ETH_SPEED_100M;
            break;

        default:
            return false;
    }

    switch (duplex)
    {
        case ETHERNET_DUPLEX_HALF:
            mac_config.DuplexMode = ETH_HALFDUPLEX_MODE;
            break;

        case ETHERNET_DUPLEX_FULL:
            mac_config.DuplexMode = ETH_FULLDUPLEX_MODE;
            break;

        default:
            return false;
    }

    return HAL_ETH_SetMACConfig(eth_handle, &mac_config) == HAL_OK;
}

/**
 * @brief  以中断模式启动 Ethernet MAC 和 DMA。
 *
 * @details
 * HAL_ETH_Start_IT() 仍负责建立 IT 模式 RX Descriptor，并保留 TX complete、
 * RX complete、fatal bus error 等 HAL 行为。启动成功后单独屏蔽 RBUE：
 * RX Buffer Unavailable 对当前 copy-first Driver 是可恢复的背压状态，
 * Descriptor 会在 HAL_ETH_ReadData() 中重建，不需要为每次 RBU 再进入
 * Error IRQ / callback 热路径。
 *
 * @retval true   启动成功。
 * @retval false  Port、HAL 状态错误或启动失败。
 */
bool EthernetDriver_Start(void)
{
    ETH_HandleTypeDef *eth_handle = EthernetPort_GetHandle();

    if ((eth_handle == NULL) || (eth_handle->gState != HAL_ETH_STATE_READY))
    {
        return false;
    }

    if (HAL_ETH_Start_IT(eth_handle) != HAL_OK)
    {
        return false;
    }

    /*
     * 仅关闭 RBU source。AIE / FBEE 继续保持 HAL Start_IT() 的配置，
     * 因而 Fatal Bus Error 等真正的 DMA 异常仍可正常进入 HAL error path。
     */
    __HAL_ETH_DMA_DISABLE_IT(eth_handle, ETH_DMACIER_RBUE);

    return true;
}

/**
 * @brief  重新使能 RX complete 中断。
 *
 * @details
 * HAL_ETH_RxCpltCallback() 在第一次 RX 中断后会关闭 RIE，让 Runtime Task
 * 连续 drain 当前 RX ring。任务处理到暂时无 Frame 后调用本函数恢复 RIE。
 *
 * 被屏蔽期间 RI 状态可能保持置位；重新打开 RIE 后可能立即产生一次 IRQ。
 * HAL callback 会再次检查当前 Descriptor OWN 位，并过滤没有实际待处理
 * Descriptor 的旧 RI，因此这里不主动清 DMACSR，避免和其他 DMA 状态位竞态。
 */
void EthernetDriver_RearmRxInterrupt(void)
{
    ETH_HandleTypeDef *eth_handle = EthernetPort_GetHandle();
    uint32_t primask;

    if (eth_handle == NULL)
    {
        return;
    }

    primask = EthernetDriver_EnterCritical();

    if (eth_handle->gState == HAL_ETH_STATE_STARTED)
    {
        /* Descriptor rebuild / tail pointer update 必须先对 DMA 可见。 */
        __DMB();
        __HAL_ETH_DMA_ENABLE_IT(eth_handle, ETH_DMACIER_RIE);
    }

    EthernetDriver_ExitCritical(primask);
}

/**
 * @brief  异步提交一个完整 Ethernet Frame。
 *
 * @details
 * Frame 会先复制到 Driver 管理的 TX DMA Buffer，再提交给 HAL 中断发送接口。
 * 函数返回后，调用者即可释放或复用原始 frame。已提交 Buffer 在 TX complete
 * 事件到达后由 EthernetDriver_ProcessTxCompletions() 回收。
 *
 * @param[in] frame  Ethernet Frame，包含目的 MAC、源 MAC、EtherType 和 Payload，
 *                   不包含 FCS。
 * @param[in] length Frame 长度。
 *
 * @retval ETHERNET_TX_QUEUED Frame 已成功提交。
 * @retval ETHERNET_TX_RETRY  当前无可用 Buffer 或 HAL 暂时无法接收，可稍后重试。
 * @retval ETHERNET_TX_ERROR  参数无效或 Driver 尚未启动。
 */
EthernetTxResult EthernetDriver_TransmitAsync(const uint8_t *frame, uint16_t length)
{
    ETH_HandleTypeDef *eth_handle = EthernetPort_GetHandle();
    ETH_BufferTypeDef hal_buffer = {0};
    ETH_TxPacketConfigTypeDef tx_config = {0};
    uint8_t *dma_buffer;
    HAL_StatusTypeDef hal_status;
    uint32_t primask;

    if ((eth_handle == NULL) ||
        (frame == NULL) ||
        (length < 14U) ||
        (length > ETHERNET_DMA_BUFFER_SIZE) ||
        (eth_handle->gState != HAL_ETH_STATE_STARTED))
    {
        EthernetDriver_IncrementCounter(&g_stats.tx_errors);
        return ETHERNET_TX_ERROR;
    }

    /*
     * Backstop：
     * 即使 TX complete Task 尚未来得及运行，也先尝试回收已经完成的包。
     */
    EthernetDriver_ProcessTxCompletions();

    dma_buffer = EthernetDriver_AcquireTxBuffer();

    if (dma_buffer == NULL)
    {
        EthernetDriver_IncrementCounter(&g_stats.tx_retries);
        return ETHERNET_TX_RETRY;
    }

    /*
     * copy 完成后，caller 原始 frame 生命周期即可结束。
     */
    memcpy(dma_buffer, frame, length);

    hal_buffer.buffer = dma_buffer;
    hal_buffer.len = length;
    hal_buffer.next = NULL;

    tx_config.Attributes =ETH_TX_PACKETS_FEATURES_CRCPAD | ETH_TX_PACKETS_FEATURES_SAIC;
    tx_config.Length = length;
    tx_config.TxBuffer = &hal_buffer;
    tx_config.SrcAddrCtrl = ETH_SRC_ADDR_REPLACE;
    tx_config.CRCPadCtrl = ETH_CRC_PAD_INSERT;

    /*
     * HAL 会保存该地址，并在 HAL_ETH_ReleaseTxPacket()
     * 中把它传给 HAL_ETH_TxFreeCallback()。
     */
    tx_config.pData = dma_buffer;

    primask = EthernetDriver_EnterCritical();

    hal_status = HAL_ETH_Transmit_IT(
        eth_handle,
        &tx_config);

    EthernetDriver_ExitCritical(primask);

    if (hal_status == HAL_OK)
    {
        return ETHERNET_TX_QUEUED;
    }

    /*
     * 当前单 Buffer、无 VLAN/TSO 路径下，未成功提交意味着
     * dma_buffer 尚未进入正常 DMA ownership，可归还 Driver Pool。
     */
    (void)EthernetDriver_ReleaseTxBuffer(dma_buffer);
    EthernetDriver_IncrementCounter(&g_stats.tx_retries);

    return ETHERNET_TX_RETRY;
}

/**
 * @brief  回收所有已完成发送的 TX Packet 和 DMA Buffer。
 *
 * @details
 * 调用 HAL 释放已完成的 TX Descriptor，并通过 HAL_ETH_TxFreeCallback()
 * 将对应 DMA Buffer 归还 Driver Pool。本函数应在任务上下文调用。
 */
void EthernetDriver_ProcessTxCompletions(void)
{
    ETH_HandleTypeDef *eth_handle = EthernetPort_GetHandle();
    uint32_t primask;

    if ((eth_handle == NULL) || (eth_handle->gState != HAL_ETH_STATE_STARTED))
    {
        return;
    }

    primask = EthernetDriver_EnterCritical();

    (void)HAL_ETH_ReleaseTxPacket(eth_handle);

    EthernetDriver_ExitCritical(primask);
}

/**
 * @brief  读取一个完整 Ethernet Frame。
 *
 * @param[out] frame     接收 Frame 的调用者 Buffer。
 * @param[in]  capacity  调用者 Buffer 容量。
 * @param[out] length    实际接收 Frame 长度。
 *
 * @retval ETHERNET_RX_FRAME  成功读取一个完整 Frame。
 * @retval ETHERNET_RX_NONE   当前没有完整 Frame。
 * @retval ETHERNET_RX_ERROR  Port、参数、状态或 RX Frame 无效。
 */
EthernetRxResult EthernetDriver_Receive(uint8_t *frame, uint16_t capacity, uint16_t *length)
{
    ETH_HandleTypeDef *eth_handle = EthernetPort_GetHandle();
    void *app_buffer = NULL;
    HAL_StatusTypeDef hal_status;

    if ((eth_handle == NULL) ||
        (frame == NULL) ||
        (length == NULL) ||
        (capacity == 0U) ||
        (eth_handle->gState != HAL_ETH_STATE_STARTED))
    {
        EthernetDriver_IncrementCounter(&g_stats.rx_errors);
        return ETHERNET_RX_ERROR;
    }

    *length = 0U;

    hal_status = HAL_ETH_ReadData(eth_handle, &app_buffer);

    if (hal_status != HAL_OK)
    {
        return ETHERNET_RX_NONE;
    }

    if ((app_buffer != &g_rx_frame) ||
        !g_rx_frame.valid ||
        (g_rx_frame.length == 0U) ||
        (g_rx_frame.length > capacity))
    {
        g_rx_frame.length = 0U;
        g_rx_frame.valid = false;

        EthernetDriver_IncrementCounter(&g_stats.rx_errors);

        return ETHERNET_RX_ERROR;
    }

    memcpy(frame, g_rx_frame.data, g_rx_frame.length);

    *length = (uint16_t)g_rx_frame.length;

    g_rx_frame.length = 0U;
    g_rx_frame.valid = false;

    EthernetDriver_IncrementCounter(&g_stats.rx_frames);

    return ETHERNET_RX_FRAME;
}

/****************************************************************************

========================== HAL callbacks 区域 ================================

*****************************************************************************/

/**
 * @brief  为 HAL RX Descriptor 提供空闲 DMA Buffer。
 *
 * @param[out] buffer 返回 DMA Buffer 地址；没有空闲 Buffer 时返回 NULL。
 */
void HAL_ETH_RxAllocateCallback(uint8_t **buffer)
{
    if (buffer == NULL)
    {
        return;
    }

    *buffer = NULL;

    for (uint32_t i = 0U; i < ETH_RX_DESC_CNT; i++)
    {
        if (!g_rx_buffer_in_use[i])
        {
            g_rx_buffer_in_use[i] = true;
            *buffer = g_rx_dma_buffers[i];
            return;
        }
    }

    EthernetDriver_IncrementCounter(&g_stats.rx_buffer_unavailable);
}

/**
 * @brief  将 HAL 收到的 DMA Buffer 数据复制到 CPU 侧 Frame。
 *
 * @details
 * HAL_ETH_ReadData() 每处理一个 RX Descriptor 都会调用本函数。
 * DMA Buffer 完成复制后立即归还 RX Pool，使 Descriptor 可以重新获取 Buffer。
 */
void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buffer, uint16_t length)
{
    if ((pStart == NULL) || (pEnd == NULL))
    {
        if (buffer != NULL)
        {
            (void)EthernetDriver_ReleaseRxBuffer(buffer);

            EthernetDriver_IncrementCounter(&g_stats.rx_dropped);
        }

        return;
    }

    if (*pStart == NULL)
    {
        g_rx_frame.length = 0U;
        g_rx_frame.valid = true;

        *pStart = &g_rx_frame;
        *pEnd = &g_rx_frame;
    }

    if (!EthernetDriver_AppendRxData(buffer, length))
    {
        EthernetDriver_InvalidateRxFrame();
    }

    if ((buffer == NULL) || !EthernetDriver_ReleaseRxBuffer(buffer))
    {
        EthernetDriver_InvalidateRxFrame();
    }

    *pEnd = &g_rx_frame;
}

/**
 * @brief  HAL RX complete callback。
 *
 * @details
 * 第一次 RX complete interrupt 只负责把 Runtime Task 唤醒，并立即屏蔽 RIE。
 * Runtime Task 会在中断保持屏蔽期间批量 drain RX ring，处理完当前积压后再通过
 * EthernetDriver_RearmRxInterrupt() 恢复 RIE。
 *
 * 重新打开 RIE 时，屏蔽期间遗留的 RI 状态可能触发一次旧中断。这里检查 HAL
 * 当前 RxDescIdx 对应 Descriptor 的 OWN 位；若 Descriptor 仍归 DMA 所有，说明
 * 实际没有新 Frame，无需再次唤醒 Runtime Task，直接恢复 RIE 即可。
 */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
    EthernetDriverRxEventHandler handler = g_rx_event_handler;
    void *context = g_rx_event_context;

    if ((heth == NULL) || (handler == NULL))
    {
        return;
    }

    /*
     * 从这里开始把 RX complete interrupt 当作“启动一轮 RX batch”的门铃。
     * TX / Error interrupt 保持原状态，不受影响。
     */
    __HAL_ETH_DMA_DISABLE_IT(heth, ETH_DMACIER_RIE);

    if (!EthernetDriver_IsRxDescriptorReady(heth))
    {
        /*
         * 这是 rearm 后由旧 RI 造成的空中断。HAL 已在进入 callback 前清 RI，
         * 因此此处直接重新打开 RIE，不需要碰 DMACSR 其他状态位。
         */
        __DMB();
        __HAL_ETH_DMA_ENABLE_IT(heth, ETH_DMACIER_RIE);
        return;
    }

    handler(context);
}

/**
 * @brief  HAL TX complete callback。
 *
 * @details
 * ISR 中只发送 completion event，不直接回收 TX Descriptor。
 */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)
{
    EthernetDriverTxEventHandler handler = g_tx_event_handler;
    void *context = g_tx_event_context;

    (void)heth;

    if (handler != NULL)
    {
        handler(context);
    }
}

/**
 * @brief HAL TX packet free callback。
 *
 * @details
 * 由 HAL_ETH_ReleaseTxPacket() 在任务上下文调用。
 * 只有 Driver TX Buffer 被实际识别并归还 Pool 后，才记为一次 TX completion。
 */
void HAL_ETH_TxFreeCallback(uint32_t *buffer)
{
    if ((buffer != NULL) && EthernetDriver_ReleaseTxBuffer((uint8_t *)buffer))
    {
        EthernetDriver_IncrementCounter(&g_stats.tx_completed);
    }
}

/**
 * @brief HAL Ethernet Error callback。
 *
 * @details
 * ISR 中只保存错误统计和最近一次 HAL / DMA / MAC ErrorCode，
 * 不执行 printf、DMA recovery、MAC restart 或协议处理。
 *
 * @param[in] heth Ethernet HAL Handle。
 */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
    uint32_t primask;

    if (heth == NULL)
    {
        return;
    }

    primask = EthernetDriver_EnterCritical();

    g_stats.hal_error_events++;
    g_stats.last_hal_error_code = HAL_ETH_GetError(heth);
    g_stats.last_dma_error_code = HAL_ETH_GetDMAError(heth);
    g_stats.last_mac_error_code = HAL_ETH_GetMACError(heth);

    EthernetDriver_ExitCritical(primask);
}