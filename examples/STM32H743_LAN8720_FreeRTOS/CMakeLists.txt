cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME stm32H7ethernet_demo)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

enable_language(C ASM)

add_executable(${CMAKE_PROJECT_NAME})

# STM32CubeMX-generated project lives entirely inside this example directory.
add_subdirectory(cmake/stm32cubemx)

# The reusable Driver Package stays at repository root.
set(ETHERNET_PACKAGE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../Ethernet")

target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ./BSP/stm32h743vit6_iot/board_debug.c
    ./BSP/stm32h743vit6_iot/ethernet_port.c
    ${ETHERNET_PACKAGE_DIR}/Src/ethernet_mdio.c
    ${ETHERNET_PACKAGE_DIR}/Src/ethernet_driver.c
    ${ETHERNET_PACKAGE_DIR}/PHY/LAN8720/Src/lan8720.c
    ${ETHERNET_PACKAGE_DIR}/RTOS/CMSIS_RTOS2/Src/ethernet_rtos.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ./BSP/stm32h743vit6_iot
    ${ETHERNET_PACKAGE_DIR}/Inc
    ${ETHERNET_PACKAGE_DIR}/PHY/LAN8720/Inc
    ${ETHERNET_PACKAGE_DIR}/Port/Inc
    ${ETHERNET_PACKAGE_DIR}/RTOS/CMSIS_RTOS2/Inc
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
)

#
# RX 性能诊断专用：
# 通过 GNU ld --wrap 拦截 HAL_ETH_ReadData() 与 HAL_ETH_IRQHandler()，
# 在不修改 ST HAL 的前提下分别测量 RX task-side HAL 路径和 ETH IRQ handler。
# 完成瓶颈定位后应删除这些 wrap 选项。
#
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,--wrap=HAL_ETH_ReadData
    -Wl,--wrap=HAL_ETH_IRQHandler
)

list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
)
