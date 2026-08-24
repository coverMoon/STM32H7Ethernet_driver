#ifndef ETHERNET_DRIVER_H
#define ETHERNET_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETHERNET_FRAME_BUFFER_SIZE 1536U

/**
 * @brief Ethernet 接收结果。
 */
typedef enum
{
    ETHERNET_RX_NONE = 0,
    ETHERNET_RX_FRAME,
    ETHERNET_RX_ERROR
} EthernetRxResult;

/**
 * @brief Ethernet 异步发送提交结果。
 */
typedef enum
{
    ETHERNET_TX_QUEUED = 0, /**< Frame 已提交到 HAL 异步发送队列。 */
    ETHERNET_TX_RETRY,      /**< 临时无可用资源，调用者可稍后重试。 */
    ETHERNET_TX_ERROR       /**< 参数无效或 Driver 尚未启动。 */
} EthernetTxResult;

/**
 * @brief Ethernet Driver 运行统计。
 *
 * @details
 * 所有计数从 EthernetDriver_Init() 开始累计。
 * 本结构只提供运行状态快照，不用于控制 Driver 行为。
 */
typedef struct
{
    uint32_t rx_frames;               /**< 成功向上层交付的完整 RX Frame 数量。 */
    uint32_t rx_errors;               /**< EthernetDriver_Receive() 返回错误的次数。 */
    uint32_t rx_dropped;              /**< Driver 组装阶段明确丢弃的 RX Frame 数量。 */
    uint32_t rx_buffer_unavailable;   /**< RX Descriptor 无法获得空闲 DMA Buffer 的次数。 */

    uint32_t tx_queued;               /**< 成功提交到 HAL 异步发送路径的 Frame 数量。 */
    uint32_t tx_retries;              /**< 返回 ETHERNET_TX_RETRY 的次数。 */
    uint32_t tx_errors;               /**< 返回 ETHERNET_TX_ERROR 的次数。 */
    uint32_t tx_completed;            /**< 已发送完成并归还 TX Buffer 的 Frame 数量。 */

    uint32_t hal_error_events;        /**< HAL ETH Error callback 触发次数。 */
    uint32_t last_hal_error_code;     /**< 最近一次 HAL ETH ErrorCode。 */
    uint32_t last_dma_error_code;     /**< 最近一次 HAL ETH DMAErrorCode。 */
    uint32_t last_mac_error_code;     /**< 最近一次 HAL ETH MACErrorCode。 */
} EthernetDriverStats;

/**
 * @brief Ethernet 链路速率。
 */
typedef enum
{
    ETHERNET_LINK_SPEED_10M = 0,
    ETHERNET_LINK_SPEED_100M
} EthernetLinkSpeed;

/**
 * @brief Ethernet 双工模式。
 */
typedef enum
{
    ETHERNET_DUPLEX_HALF = 0,
    ETHERNET_DUPLEX_FULL
} EthernetDuplexMode;

/**
 * @brief RX complete 事件处理函数。
 *
 * @details
 * 在 ISR 上下文触发，只允许执行轻量事件转发。
 *
 * @param[in] context 注册处理函数时传入的用户上下文。
 */
typedef void (*EthernetDriverRxEventHandler)(void *context);

/**
 * @brief TX complete 事件处理函数。
 *
 * @details
 * 在 ISR 上下文触发，只允许执行轻量事件转发。
 *
 * @param[in] context 注册处理函数时传入的用户上下文。
 */
typedef void (*EthernetDriverTxEventHandler)(void *context);

void EthernetDriver_Init(void);
void EthernetDriver_SetRxEventHandler(EthernetDriverRxEventHandler handler, void *context);
void EthernetDriver_SetTxEventHandler(EthernetDriverTxEventHandler handler, void *context);
bool EthernetDriver_ConfigureLink(EthernetLinkSpeed speed, EthernetDuplexMode duplex);
bool EthernetDriver_Start(void);
EthernetTxResult EthernetDriver_TransmitAsync(const uint8_t *frame, uint16_t length);
void EthernetDriver_ProcessTxCompletions(void);
EthernetRxResult EthernetDriver_Receive(uint8_t *frame, uint16_t capacity, uint16_t *length);
void EthernetDriver_RearmRxInterrupt(void);
bool EthernetDriver_GetStats(EthernetDriverStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_DRIVER_H */