#ifndef ETHERNET_RTOS_H
#define ETHERNET_RTOS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 任务上下文中的完整 Ethernet Frame 处理函数。
 *
 * @details
 * frame 仅在处理函数调用期间有效，返回后不得继续持有该指针。
 *
 * @param[in] frame   完整 Ethernet Frame，不包含 FCS。
 * @param[in] length  Frame 长度。
 * @param[in] context 注册处理函数时传入的用户上下文。
 */
typedef void (*EthernetRtosRxFrameHandler)(const uint8_t *frame, uint16_t length, void *context);

void EthernetRtos_SetRxFrameHandler(EthernetRtosRxFrameHandler handler, void *context);
bool EthernetRtos_IsReady(void);
void EthernetRtos_RuntimeTask(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_RTOS_H */