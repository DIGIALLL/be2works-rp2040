cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2040_zero CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(cp2112_emu C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(cp2112_emu
    src/main.c
    src/cp2112.c
    src/smbus.c
    src/usb_descriptors.c
)

target_include_directories(cp2112_emu PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_libraries(cp2112_emu PRIVATE
    pico_stdlib
    pico_unique_id
    pico_bootrom
    hardware_i2c
    hardware_watchdog
    tinyusb_device
    tinyusb_board
)

# USB занят HID-интерфейсом CP2112, stdio по USB не нужен
pico_enable_stdio_usb(cp2112_emu 0)
pico_enable_stdio_uart(cp2112_emu 0)

pico_add_extra_outputs(cp2112_emu)
