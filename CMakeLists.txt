cmake_minimum_required(VERSION 3.20)
project(nucleo_g071rb_security C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
    src/startup_stm32g071rb.s
    src/main.c
    src/system_stm32g0xx.c
    src/security.c
    src/boot_policy.c
    src/crypto_stub.c
    src/platform.c
)

target_include_directories(${PROJECT_NAME} PRIVATE include)

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
)

