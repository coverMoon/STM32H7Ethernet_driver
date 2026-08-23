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

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_DRIVER_H */