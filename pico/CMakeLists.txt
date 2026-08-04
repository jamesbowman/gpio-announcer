cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED PICO_SDK_PATH)
    if (DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
    else()
        message(FATAL_ERROR "Set PICO_SDK_PATH to a Pico SDK 2.2.0 checkout")
    endif()
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(gpio_announcer C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.2.0")
    message(FATAL_ERROR
        "Pico SDK 2.2.0 or later is required; found ${PICO_SDK_VERSION_STRING}")
endif()

pico_sdk_init()

add_executable(gpio_announcer gpio_announcer.c)

target_compile_options(gpio_announcer PRIVATE -Wall -Wextra)

target_link_libraries(gpio_announcer
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_pio
)

# Keep every announced GPIO free; diagnostics use USB CDC only.
pico_enable_stdio_usb(gpio_announcer 1)
pico_enable_stdio_uart(gpio_announcer 0)
pico_add_extra_outputs(gpio_announcer)
