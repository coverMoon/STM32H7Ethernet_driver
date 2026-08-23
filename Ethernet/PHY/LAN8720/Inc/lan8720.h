#ifndef LAN8720_H
#define LAN8720_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LAN8720 操作结果。
 */
typedef enum
{
    LAN8720_RESULT_OK = 0,          /**< 操作成功。 */
    LAN8720_RESULT_NOT_READY,       /**< PHY 暂时尚未进入可用状态，可稍后重试。 */
    LAN8720_RESULT_ERROR_ARGUMENT,  /**< 输入参数无效。 */
    LAN8720_RESULT_ERROR_MDIO,      /**< MDIO 访问失败。 */
    LAN8720_RESULT_ERROR_ID         /**< MDIO 设备存在，但 PHY ID 与 LAN8720 不匹配。 */
} Lan8720Result;

/**
 * @brief LAN8720 链路速率。
 */
typedef enum
{
    LAN8720_SPEED_UNKNOWN = 0,
    LAN8720_SPEED_10M,
    LAN8720_SPEED_100M
} Lan8720Speed;

/**
 * @brief LAN8720 链路双工模式。
 */
typedef enum
{
    LAN8720_DUPLEX_UNKNOWN = 0,
    LAN8720_DUPLEX_HALF,
    LAN8720_DUPLEX_FULL
} Lan8720Duplex;

/**
 * @brief LAN8720 当前链路状态。
 *
 * @details
 * 包含 PHY Link 状态、自动协商状态以及当前协商得到的速率和双工模式。
 * 当链路未建立或自动协商尚未完成时，speed 和 duplex 为 UNKNOWN。
 */
typedef struct
{
    bool link_up;
    bool auto_negotiation_complete;
    Lan8720Speed speed;
    Lan8720Duplex duplex;
} Lan8720Status;


Lan8720Result Lan8720_Init(uint32_t phy_address);
Lan8720Result Lan8720_GetStatus(uint32_t phy_address, Lan8720Status *status);

#ifdef __cplusplus
}
#endif

#endif /* LAN8720_H */