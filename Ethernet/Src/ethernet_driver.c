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
 * 保留 HAL_ETH_Start_IT() 的正常 RX complete / TX / fatal DMA interrupt 行为。
 * 启动成功后只关闭 Receive Buffer Unavailable interrupt source（RBUE），用于
 * 隔离高负载下 RBU abnormal interrupt storm 的 CPU 开销。RBU 本身仍可能在
 * DMACSR 中出现；当前 copy-first RX 仍通过 HAL_ETH_ReadData() 消费 Frame、
 * 重建 Descriptor 并更新 tail pointer，不依赖 ErrorCallback 才能恢复接收。
 *
 * 这是性能诊断阶段的单变量实验，不代表最终 recovery 策略已经冻结。
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
     * 仅屏蔽专用 RBU interrupt；RIE / TIE / AIE / FBEE 等保持 HAL 配置。
     * 这样可以单独测量 RBU Error IRQ storm 对 RX 热路径的影响。
     */
    __HAL_ETH_DMA_DISABLE_IT(eth_handle, ETH_DMACIER_RBUE);

    return true;
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
 * 仅把中断事件转交给注册的上层事件处理函数，不读取 Frame。
 */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
    EthernetDriverRxEventHandler handler = g_rx_event_handler;
    void *context = g_rx_event_context;

    (void)heth;

    if (handler != NULL)
    {
        handler(context);
    }
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
