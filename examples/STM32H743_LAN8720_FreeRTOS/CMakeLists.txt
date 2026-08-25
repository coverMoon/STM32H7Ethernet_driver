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
set(ETHERNET_TEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../../test")

# Temporary M2 TX throughput benchmark harness. Keep it outside Driver Package.
option(ETHERNET_TX_BENCHMARK_ENABLE "Enable raw Ethernet TX throughput benchmark" ON)
set(ETHERNET_TX_BENCHMARK_FRAME_SIZE "60" CACHE STRING
    "TX benchmark Ethernet frame size in bytes")
set(ETHERNET_TX_BENCHMARK_FRAME_COUNT "200000" CACHE STRING
    "TX benchmark frame count")

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

if(ETHERNET_TX_BENCHMARK_ENABLE)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${ETHERNET_TEST_DIR}/ethernet_tx_benchmark.c
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ./BSP/stm32h743vit6_iot
    ${ETHERNET_PACKAGE_DIR}/Inc
    ${ETHERNET_PACKAGE_DIR}/PHY/LAN8720/Inc
    ${ETHERNET_PACKAGE_DIR}/Port/Inc
    ${ETHERNET_PACKAGE_DIR}/RTOS/CMSIS_RTOS2/Inc
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
)

if(ETHERNET_TX_BENCHMARK_ENABLE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        ETHERNET_TX_BENCHMARK_FRAME_SIZE=${ETHERNET_TX_BENCHMARK_FRAME_SIZE}
        ETHERNET_TX_BENCHMARK_FRAME_COUNT=${ETHERNET_TX_BENCHMARK_FRAME_COUNT}
    )

    # Run the test harness immediately after the normal MAC/DMA start succeeds.
    # The Driver Package implementation itself is not modified.
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        -Wl,--wrap=EthernetDriver_Start
    )
endif()

list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
)
