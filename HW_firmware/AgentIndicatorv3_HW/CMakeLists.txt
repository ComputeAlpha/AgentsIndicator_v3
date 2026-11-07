# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD seeed_xiao_rp2350 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(AgentIndicatorv3_HW C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# ---- AgentIndicator v3 targets -------------------------------------------
# Shared WS2812 driver (PIO + DMA)
add_library(led_strip INTERFACE)
target_sources(led_strip INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/led_strip.cpp)
target_include_directories(led_strip INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(led_strip INTERFACE pico_stdlib hardware_pio hardware_dma)

function(agentind_usb_identity target product)
    target_compile_definitions(${target} PRIVATE
        USBD_MANUFACTURER="AgentIndicator"
        USBD_PRODUCT="${product}")
    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 0)
endfunction()

# Layout-discovery probe
add_executable(agentind_probe src/probe_main.cpp)
pico_generate_pio_header(agentind_probe ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)
target_link_libraries(agentind_probe led_strip pico_multicore)
agentind_usb_identity(agentind_probe "AgentInd v3 probe")
pico_add_extra_outputs(agentind_probe)

# Main firmware (target keeps the project name so the Pico VS Code extension's
# Run/Flash tasks pick it up)
add_executable(AgentIndicatorv3_HW
    src/main.cpp
    src/protocol.cpp
    src/renderer.cpp)
pico_generate_pio_header(AgentIndicatorv3_HW ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)
target_link_libraries(AgentIndicatorv3_HW led_strip pico_multicore pico_util hardware_watchdog)
pico_set_program_name(AgentIndicatorv3_HW "AgentIndicatorv3_HW")
pico_set_program_version(AgentIndicatorv3_HW "1.1.0")
agentind_usb_identity(AgentIndicatorv3_HW "AgentIndicator v3")
target_compile_options(AgentIndicatorv3_HW PRIVATE -Wall -Wextra)
pico_add_extra_outputs(AgentIndicatorv3_HW)
