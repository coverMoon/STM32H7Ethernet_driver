/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <stdio.h>

#include "ethernet_driver.h"
#include "ethernet_port.h"
#include "ethernet_rtos.h"
#include "lan8720.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define LAN8720_PHY_ADDRESS                 0U

#define PHY_INIT_TIMEOUT_MS              100U
#define PHY_INIT_POLL_PERIOD_MS            5U

#define AUTO_NEGOTIATION_TIMEOUT_MS      5000U
#define AUTO_NEGOTIATION_POLL_PERIOD_MS   100U

#define PHY_LINK_POLL_PERIOD_MS           200U

#define ETHERNET_RX_TEST_ETHERTYPE      0x88B5U
#define ETHERNET_RX_TEST_TARGET_COUNT   1000U

#define ETHERNET_ASYNC_TX_TEST_ENABLE       1
#define ETHERNET_TX_TEST_TARGET_COUNT    1000U
#define ETHERNET_TX_TEST_RETRY_TIMEOUT_MS 5000U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
static uint32_t g_ethernet_rx_frame_count;
static uint32_t g_ethernet_rx_test_frame_count;
/* USER CODE END Variables */
/* Definitions for BootstrapTask */
osThreadId_t BootstrapTaskHandle;
const osThreadAttr_t BootstrapTask_attributes = {
  .name = "BootstrapTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for EthernetRuntime */
osThreadId_t EthernetRuntimeHandle;
const osThreadAttr_t EthernetRuntime_attributes = {
  .name = "EthernetRuntime",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static bool EthernetBootstrap_StartMac(const Lan8720Status *phy_status);
static void EthernetDemo_RxFrameHandler(const uint8_t *frame, uint16_t length, void *context);

#if ETHERNET_ASYNC_TX_TEST_ENABLE
static bool EthernetDemo_RunAsyncTxTest(void);
#endif
/* USER CODE END FunctionPrototypes */

void StartBootstrapTask(void *argument);
void EthernetRtos_RuntimeTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of BootstrapTask */
  BootstrapTaskHandle = osThreadNew(StartBootstrapTask, NULL, &BootstrapTask_attributes);

  /* creation of EthernetRuntime */
  EthernetRuntimeHandle = osThreadNew(EthernetRtos_RuntimeTask, NULL, &EthernetRuntime_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartBootstrapTask */
/**
  * @brief  Function implementing the BootstrapTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartBootstrapTask */
void StartBootstrapTask(void *argument)
{
  /* USER CODE BEGIN StartBootstrapTask */
  Lan8720Status phy_status = {0};
  Lan8720Status last_phy_status = {0};
  Lan8720Result phy_result = LAN8720_RESULT_NOT_READY;

  bool last_status_valid = false;
  bool negotiation_ready = false;

  uint32_t elapsed_ms = 0U;

  (void)argument;

  printf("[ETH] BootstrapTask started\r\n");

  EthernetRtos_SetRxFrameHandler(EthernetDemo_RxFrameHandler, NULL);

  /*
   * MX_GPIO_Init() 已将 PHY nRST 拉低。
   * 等待上电稳定后，由板级 Port 释放 PHY Hardware Reset。
   */
  osDelay(25U);
  EthernetPort_PhyResetRelease();

  /*
   * 初始化 LAN8720。
   *
   * Lan8720_Init() 只验证 PHY 并确保 Auto-negotiation 已启用，
   * 不等待 Link Up，因此 timeout / retry 仍由 BootstrapTask 管理。
   */
  elapsed_ms = 0U;

  while (elapsed_ms < PHY_INIT_TIMEOUT_MS)
  {
    phy_result = Lan8720_Init(LAN8720_PHY_ADDRESS);

    if (phy_result == LAN8720_RESULT_OK)
    {
      break;
    }

    /*
     * 参数错误和 PHY ID 不匹配属于永久配置错误，
     * 继续等待不会改变结果，因此立即结束初始化。
     */
    if ((phy_result == LAN8720_RESULT_ERROR_ARGUMENT) ||
        (phy_result == LAN8720_RESULT_ERROR_ID))
    {
      break;
    }

    /*
     * NOT_READY 和暂时的 MDIO 访问失败允许在 Reset 释放后短时间重试。
     */
    osDelay(PHY_INIT_POLL_PERIOD_MS);
    elapsed_ms += PHY_INIT_POLL_PERIOD_MS;
  }

  if (phy_result != LAN8720_RESULT_OK)
  {
    printf(
      "[ETH] PHY init failed, result=%d\r\n",
      (int)phy_result);
  }
  else
  {
    printf("[ETH] PHY initialized\r\n");
    printf("[ETH] Waiting for auto-negotiation\r\n");

    /*
     * Hardware Reset 已经让 LAN8720 自行开始 Auto-negotiation。
     * 此处只等待 PHY 报告协商完成，不再主动 Restart。
     */
    elapsed_ms = 0U;

    while (elapsed_ms < AUTO_NEGOTIATION_TIMEOUT_MS)
    {
      phy_result = Lan8720_GetStatus(
        LAN8720_PHY_ADDRESS,
        &phy_status);

      if ((phy_result == LAN8720_RESULT_OK) &&
          phy_status.link_up &&
          phy_status.auto_negotiation_complete)
      {
        negotiation_ready = true;
        break;
      }

      /*
       * 状态读取期间允许临时 NOT_READY / MDIO failure。
       * timeout 仍由调用层统一控制。
       */
      osDelay(AUTO_NEGOTIATION_POLL_PERIOD_MS);
      elapsed_ms += AUTO_NEGOTIATION_POLL_PERIOD_MS;
    }

    if (negotiation_ready)
    {
      printf("[ETH] Link up\r\n");

      if (phy_status.speed == LAN8720_SPEED_100M)
      {
        printf("[ETH] Speed=100M\r\n");
      }
      else if (phy_status.speed == LAN8720_SPEED_10M)
      {
        printf("[ETH] Speed=10M\r\n");
      }
      else
      {
        printf("[ETH] Speed=Unknown\r\n");
      }

      if (phy_status.duplex == LAN8720_DUPLEX_FULL)
      {
        printf("[ETH] Duplex=Full\r\n");
      }
      else if (phy_status.duplex == LAN8720_DUPLEX_HALF)
      {
        printf("[ETH] Duplex=Half\r\n");
      }
      else
      {
        printf("[ETH] Duplex=Unknown\r\n");
      }

      if (EthernetBootstrap_StartMac(&phy_status))
      {
        printf("[ETH] MAC/DMA started\r\n");

        #if ETHERNET_ASYNC_TX_TEST_ENABLE
        if (EthernetDemo_RunAsyncTxTest())
        {
          printf("[ETH] Async TX queued 1000/1000\r\n");
        }
        #endif
      }

      last_phy_status = phy_status;
      last_status_valid = true;
    }
    else
    {
      printf(
        "[ETH] Auto-negotiation timeout or link down\r\n");
    }
  }

  /*
   * 周期轮询 PHY Link。
   *
   * 当前工作单元只收敛 PHY API，不在这里实现完整 MAC
   * Link Down / Up lifecycle。此循环仍只负责状态观测和日志。
   */
  for (;;)
  {
    Lan8720Status current_phy_status = {0};

    phy_result = Lan8720_GetStatus(
      LAN8720_PHY_ADDRESS,
      &current_phy_status);

    if (phy_result == LAN8720_RESULT_OK)
    {
      if (!last_status_valid ||
          (current_phy_status.link_up !=
           last_phy_status.link_up))
      {
        if (current_phy_status.link_up)
        {
          printf("[PHY] Link up\r\n");

          if (current_phy_status.speed ==
              LAN8720_SPEED_100M)
          {
            printf("[PHY] Speed=100M\r\n");
          }
          else if (current_phy_status.speed ==
                   LAN8720_SPEED_10M)
          {
            printf("[PHY] Speed=10M\r\n");
          }

          if (current_phy_status.duplex ==
              LAN8720_DUPLEX_FULL)
          {
            printf("[PHY] Duplex=Full\r\n");
          }
          else if (current_phy_status.duplex ==
                   LAN8720_DUPLEX_HALF)
          {
            printf("[PHY] Duplex=Half\r\n");
          }
        }
        else
        {
          printf("[PHY] Link down\r\n");
        }

        last_phy_status = current_phy_status;
        last_status_valid = true;
      }
    }

    osDelay(PHY_LINK_POLL_PERIOD_MS);
  }

  /* USER CODE END StartBootstrapTask */
}

/* USER CODE BEGIN Header_EthernetRtos_RuntimeTask */
/**
* @brief Function implementing the EthernetRuntime thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_EthernetRtos_RuntimeTask */
__weak void EthernetRtos_RuntimeTask(void *argument)
{
  /* USER CODE BEGIN EthernetRtos_RuntimeTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END EthernetRtos_RuntimeTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
/**
 * @brief  根据 PHY 协商结果配置并启动 Ethernet MAC/DMA。
 *
 * @param[in] phy_status PHY 当前链路状态。
 *
 * @retval true   MAC 配置并启动成功。
 * @retval false  PHY 状态无效、RX Runtime 未就绪或 Ethernet 操作失败。
 */
static bool EthernetBootstrap_StartMac(const Lan8720Status *phy_status)
{
  EthernetLinkSpeed speed;
  EthernetDuplexMode duplex;

  if ((phy_status == NULL) ||
      !phy_status->link_up ||
      !phy_status->auto_negotiation_complete)
  {
    return false;
  }

  if (!EthernetRtos_IsReady())
  {
    printf("[ETH] Ethernet runtime not ready\r\n");
    return false;
  }

  if (phy_status->speed == LAN8720_SPEED_100M)
  {
    speed = ETHERNET_LINK_SPEED_100M;
  }
  else if (phy_status->speed == LAN8720_SPEED_10M)
  {
    speed = ETHERNET_LINK_SPEED_10M;
  }
  else
  {
    printf("[ETH] Unsupported PHY speed\r\n");
    return false;
  }

  if (phy_status->duplex == LAN8720_DUPLEX_FULL)
  {
    duplex = ETHERNET_DUPLEX_FULL;
  }
  else if (phy_status->duplex == LAN8720_DUPLEX_HALF)
  {
    duplex = ETHERNET_DUPLEX_HALF;
  }
  else
  {
    printf("[ETH] Unsupported PHY duplex mode\r\n");
    return false;
  }

  if (!EthernetDriver_ConfigureLink(speed, duplex))
  {
    printf("[ETH] MAC link config failed\r\n");
    return false;
  }

  if (!EthernetDriver_Start())
  {
    printf("[ETH] MAC/DMA start failed\r\n");
    return false;
  }

  return true;
}

/**
 * @brief  Demo RX Frame 处理函数。
 */
static void EthernetDemo_RxFrameHandler(const uint8_t *frame, uint16_t length, void *context)
{
  uint16_t ether_type;

  (void)context;

  if (frame == NULL)
  {
    return;
  }

  g_ethernet_rx_frame_count++;

  if (length < 14U)
  {
    return;
  }

  ether_type = ((uint16_t)frame[12] << 8) | (uint16_t)frame[13];

  if (ether_type != ETHERNET_RX_TEST_ETHERTYPE)
  {
    return;
  }

  g_ethernet_rx_test_frame_count++;

  if (g_ethernet_rx_test_frame_count == ETHERNET_RX_TEST_TARGET_COUNT)
  {
    printf("[ETH] Async RX test 1000/1000 PASS, total=%lu\r\n",
           (unsigned long)g_ethernet_rx_frame_count);
  }
}

#if ETHERNET_ASYNC_TX_TEST_ENABLE

/**
 * @brief  连续异步提交测试 Frame，验证 TX Buffer 回收和重试流程。
 *
 * @details
 * Frame 尾部四字节写入发送序号；临时无可用资源时延时后重试，
 * 直到达到目标数量或超过重试等待时间。
 *
 * @retval true   所有测试 Frame 均已成功提交。
 * @retval false  Driver 返回错误或发送流程超时。
 */
static bool EthernetDemo_RunAsyncTxTest(void)
{
    static uint8_t test_frame[60] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x88, 0xB5,
        'S', 'T', 'M', '3', '2', 'H', '7', ' ',
        'a', 's', 'y', 'n', 'c', ' ', 'T', 'X'
    };

    uint32_t queued = 0U;
    uint32_t retry_wait_ms = 0U;

    while (queued < ETHERNET_TX_TEST_TARGET_COUNT)
    {
        EthernetTxResult result;

        test_frame[56] = (uint8_t)(queued >> 24);
        test_frame[57] = (uint8_t)(queued >> 16);
        test_frame[58] = (uint8_t)(queued >> 8);
        test_frame[59] = (uint8_t)queued;

        result = EthernetDriver_TransmitAsync(
            test_frame,
            sizeof(test_frame));

        if (result == ETHERNET_TX_QUEUED)
        {
            queued++;
            retry_wait_ms = 0U;

            if ((queued % 100U) == 0U)
            {
                printf(
                    "[ETH] Async TX queued=%lu\r\n",
                    (unsigned long)queued);
            }

            continue;
        }

        if (result == ETHERNET_TX_ERROR)
        {
            printf("[ETH] Async TX driver error\r\n");
            return false;
        }

        if (retry_wait_ms >= ETHERNET_TX_TEST_RETRY_TIMEOUT_MS)
        {
            printf(
                "[ETH] Async TX stalled at %lu\r\n",
                (unsigned long)queued);

            return false;
        }

        osDelay(1U);
        retry_wait_ms++;
    }

    return true;
}

#endif
/* USER CODE END Application */

