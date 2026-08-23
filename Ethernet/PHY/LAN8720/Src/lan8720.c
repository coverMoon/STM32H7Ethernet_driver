#include "lan8720.h"

#include <stddef.h>

#include "ethernet_mdio.h"

#define LAN8720_PHY_ADDRESS_MAX                  31U

#define LAN8720_REG_BMCR                          0U
#define LAN8720_REG_BMSR                          1U
#define LAN8720_REG_PHY_ID1                       2U
#define LAN8720_REG_PHY_ID2                       3U
#define LAN8720_REG_PHY_SPECIAL_CONTROL_STATUS   31U

#define LAN8720_BMCR_AUTO_NEGOTIATION_ENABLE \
    (1U << 12)

#define LAN8720_BMCR_RESTART_AUTO_NEGOTIATION \
    (1U << 9)

#define LAN8720_BMSR_AUTO_NEGOTIATION_COMPLETE \
    (1U << 5)

#define LAN8720_BMSR_LINK_STATUS \
    (1U << 2)

#define LAN8720_PHY_STATUS_AUTODONE \
    (1U << 12)

#define LAN8720_PHY_STATUS_HCDSPEED_SHIFT         2U
#define LAN8720_PHY_STATUS_HCDSPEED_MASK          0x07U

#define LAN8720_HCDSPEED_10M_HALF                 0x01U
#define LAN8720_HCDSPEED_100M_HALF                0x02U
#define LAN8720_HCDSPEED_10M_FULL                 0x05U
#define LAN8720_HCDSPEED_100M_FULL                0x06U

#define LAN8720_PHY_ID1_VALUE                     0x0007U
#define LAN8720_PHY_ID2_MASK                      0xFFF0U
#define LAN8720_PHY_ID2_VALUE                     0xC0F0U

#define LAN8720_INVALID_REGISTER_VALUE            0xFFFFU

/**
 * @brief 检查 PHY 地址是否有效。
 *
 * @param[in] phy_address PHY 地址。
 *
 * @retval true   地址位于 Clause 22 PHY 地址范围内。
 * @retval false  地址无效。
 */
static bool Lan8720_IsAddressValid(uint32_t phy_address)
{
    return phy_address <= LAN8720_PHY_ADDRESS_MAX;
}

/**
 * @brief 确保 LAN8720 自动协商功能已经启用。
 *
 * @details
 * 如果 BMCR 已经使能 Auto-negotiation，则保持当前协商状态，不主动
 * Restart。只有在 Auto-negotiation 原先关闭时，才同时置位 Enable 和
 * Restart Auto-negotiation。
 *
 * @param[in] phy_address PHY 地址。
 *
 * @retval LAN8720_RESULT_OK         自动协商已经启用。
 * @retval LAN8720_RESULT_NOT_READY  PHY 尚未返回有效 BMCR。
 * @retval LAN8720_RESULT_ERROR_MDIO MDIO 访问失败。
 */
static Lan8720Result Lan8720_EnsureAutoNegotiation(
    uint32_t phy_address)
{
    uint32_t bmcr = 0U;

    if (!EthernetMdio_Read(
            phy_address,
            LAN8720_REG_BMCR,
            &bmcr))
    {
        return LAN8720_RESULT_ERROR_MDIO;
    }

    if (bmcr == LAN8720_INVALID_REGISTER_VALUE)
    {
        return LAN8720_RESULT_NOT_READY;
    }

    if ((bmcr & LAN8720_BMCR_AUTO_NEGOTIATION_ENABLE) != 0U)
    {
        return LAN8720_RESULT_OK;
    }

    bmcr |= LAN8720_BMCR_AUTO_NEGOTIATION_ENABLE;
    bmcr |= LAN8720_BMCR_RESTART_AUTO_NEGOTIATION;

    if (!EthernetMdio_Write(
            phy_address,
            LAN8720_REG_BMCR,
            bmcr))
    {
        return LAN8720_RESULT_ERROR_MDIO;
    }

    return LAN8720_RESULT_OK;
}

/**
 * @brief 初始化 LAN8720 PHY。
 *
 * @details
 * 验证指定 PHY 地址上的器件 ID，并确保自动协商功能已经启用。
 *
 * LAN8720 在硬件 Reset 释放后，如果 Strap 配置使能 Auto-negotiation，
 * PHY 会自行开始自动协商。因此当 BMCR 已经启用 Auto-negotiation 时，
 * 本函数不会再次主动 Restart，避免无意义地重新打断当前协商过程。
 *
 * 如果检测到 Auto-negotiation 未启用，本函数会启用该功能并触发一次
 * Restart Auto-negotiation。
 *
 * 本函数不等待 Link Up，也不等待自动协商完成，不包含 delay 或 timeout。
 * 上层应通过 Lan8720_GetStatus() 轮询链路状态。
 *
 * @param[in] phy_address PHY 地址，范围为 0~31。
 *
 * @retval LAN8720_RESULT_OK              PHY 初始化成功。
 * @retval LAN8720_RESULT_NOT_READY       PHY 当前尚未返回有效寄存器值。
 * @retval LAN8720_RESULT_ERROR_ARGUMENT  PHY 地址无效。
 * @retval LAN8720_RESULT_ERROR_MDIO      MDIO 访问失败。
 * @retval LAN8720_RESULT_ERROR_ID        PHY ID 与 LAN8720 不匹配。
 */
Lan8720Result Lan8720_Init(uint32_t phy_address)
{
    uint32_t id1 = 0U;
    uint32_t id2 = 0U;

    if (!Lan8720_IsAddressValid(phy_address))
    {
        return LAN8720_RESULT_ERROR_ARGUMENT;
    }

    if (!EthernetMdio_Read(
            phy_address,
            LAN8720_REG_PHY_ID1,
            &id1))
    {
        return LAN8720_RESULT_ERROR_MDIO;
    }

    if (!EthernetMdio_Read(
            phy_address,
            LAN8720_REG_PHY_ID2,
            &id2))
    {
        return LAN8720_RESULT_ERROR_MDIO;
    }

    if ((id1 == LAN8720_INVALID_REGISTER_VALUE) ||
        (id2 == LAN8720_INVALID_REGISTER_VALUE))
    {
        return LAN8720_RESULT_NOT_READY;
    }

    if ((id1 != LAN8720_PHY_ID1_VALUE) ||
        ((id2 & LAN8720_PHY_ID2_MASK) !=
         LAN8720_PHY_ID2_VALUE))
    {
        return LAN8720_RESULT_ERROR_ID;
    }

    return Lan8720_EnsureAutoNegotiation(phy_address);
}

/**
 * @brief 获取 LAN8720 当前链路状态。
 *
 * @details
 * BMSR Link Status 为 latch-low，因此连续读取两次 BMSR，
 * 第一次读取清除历史锁存状态，第二次读取用于判断当前状态。
 *
 * 当 Link 尚未建立或 Auto-negotiation 尚未完成时，函数仍返回
 * LAN8720_RESULT_OK，但 speed 和 duplex 保持 UNKNOWN。
 *
 * @param[in]  phy_address PHY 地址，范围为 0~31。
 * @param[out] status      PHY 状态输出。
 *
 * @retval LAN8720_RESULT_OK              状态读取成功。
 * @retval LAN8720_RESULT_NOT_READY       PHY 尚未返回有效寄存器值。
 * @retval LAN8720_RESULT_ERROR_ARGUMENT  参数无效。
 * @retval LAN8720_RESULT_ERROR_MDIO      MDIO 访问失败。
 */
Lan8720Result Lan8720_GetStatus(uint32_t phy_address, Lan8720Status *status)
{
    uint32_t bmsr = 0U;
    uint32_t phy_status = 0U;
    uint32_t hcdspeed;

    if (!Lan8720_IsAddressValid(phy_address) || (status == NULL))
    {
        return LAN8720_RESULT_ERROR_ARGUMENT;
    }

    status->link_up = false;
    status->auto_negotiation_complete = false;
    status->speed = LAN8720_SPEED_UNKNOWN;
    status->duplex = LAN8720_DUPLEX_UNKNOWN;

    /*
     * BMSR Link Status 为 latch-low。
     * 第一次读取清除历史锁存状态，第二次读取获取当前状态。
     */
    if (!EthernetMdio_Read(
            phy_address,
            LAN8720_REG_BMSR,
            &bmsr))
    {
        return LAN8720_RESULT_ERROR_MDIO;
    }

    if (!EthernetMdio_Read(
            phy_address,
            LAN8720_REG_BMSR,
            &bmsr))
    {
        return LAN8720_RESULT_ERROR_MDIO;
    }

    if (bmsr == LAN8720_INVALID_REGISTER_VALUE)
    {
        return LAN8720_RESULT_NOT_READY;
    }

    status->link_up =
        (bmsr & LAN8720_BMSR_LINK_STATUS) != 0U;

    status->auto_negotiation_complete =
        (bmsr &
         LAN8720_BMSR_AUTO_NEGOTIATION_COMPLETE) != 0U;

    if (!status->link_up || !status->auto_negotiation_complete)
    {
        return LAN8720_RESULT_OK;
    }

    if (!EthernetMdio_Read(phy_address, LAN8720_REG_PHY_SPECIAL_CONTROL_STATUS, &phy_status))
    {
        return LAN8720_RESULT_ERROR_MDIO;
    }

    if (phy_status == LAN8720_INVALID_REGISTER_VALUE)
    {
        return LAN8720_RESULT_NOT_READY;
    }

    if ((phy_status & LAN8720_PHY_STATUS_AUTODONE) == 0U)
    {
        status->auto_negotiation_complete = false;
        return LAN8720_RESULT_OK;
    }

    hcdspeed =
        (phy_status >> LAN8720_PHY_STATUS_HCDSPEED_SHIFT) &
        LAN8720_PHY_STATUS_HCDSPEED_MASK;

    switch (hcdspeed)
    {
        case LAN8720_HCDSPEED_10M_HALF:
            status->speed = LAN8720_SPEED_10M;
            status->duplex = LAN8720_DUPLEX_HALF;
            break;

        case LAN8720_HCDSPEED_10M_FULL:
            status->speed = LAN8720_SPEED_10M;
            status->duplex = LAN8720_DUPLEX_FULL;
            break;

        case LAN8720_HCDSPEED_100M_HALF:
            status->speed = LAN8720_SPEED_100M;
            status->duplex = LAN8720_DUPLEX_HALF;
            break;

        case LAN8720_HCDSPEED_100M_FULL:
            status->speed = LAN8720_SPEED_100M;
            status->duplex = LAN8720_DUPLEX_FULL;
            break;

        default:
            break;
    }

    return LAN8720_RESULT_OK;
}