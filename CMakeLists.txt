cmake_minimum_required(VERSION 3.20)
project(pic_programmer_debugger C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The device table: data/pic_devices.csv -> pic_devices.c in the build directory.
set(PIC_DEVICES_C ${CMAKE_BINARY_DIR}/pic_devices.c)
add_custom_command(
    OUTPUT ${PIC_DEVICES_C}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_devices.py
            ${CMAKE_SOURCE_DIR}/data/pic_devices.csv ${PIC_DEVICES_C}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_devices.py ${CMAKE_SOURCE_DIR}/data/pic_devices.csv
    COMMENT "Generating the PIC device table"
)

add_executable(${PROJECT_NAME}
    src/startup_stm32g071rb.s
    src/main.c
    src/clock.c
    src/cmd.c
    src/i2c.c
    src/icsp.c
    src/oled.c
    src/oled_port.c
    src/pic.c
    src/proto/icsp8.c
    src/proto/p16_8bit.c
    src/proto/p18_8bit.c
    ${PIC_DEVICES_C}
    src/system_stm32g0xx.c
    src/platform.c
    src/uart.c
    src/ui.c
)

target_include_directories(${PROJECT_NAME} PRIVATE include)

# For an OLED module that never acknowledges (SSD1309 with SDAOUT not connected): drive the
# I2C pins in software and ignore acknowledges. See include/i2c.h.
option(I2C_IGNORE_ACK "Software I2C that ignores acknowledges" OFF)
if(I2C_IGNORE_ACK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE I2C_IGNORE_ACK=1)
endif()

target_compile_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wconversion
    -ffunction-sections
    -fdata-sections
)

target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -T${CMAKE_SOURCE_DIR}/linker/STM32G071RB_FLASH.ld
    -Wl,--gc-sections
    -Wl,-Map=${PROJECT_NAME}.map
    -Wl,--print-memory-usage
    -nostartfiles
)

set_target_properties(${PROJECT_NAME} PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/linker/STM32G071RB_FLASH.ld)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
)

