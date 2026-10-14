set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Pinned to the GNU Tools for STM32 14.3.1 bundle (independent of PATH)
set(ARM_TOOLCHAIN_BIN "$ENV{HOME}/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin")

set(CMAKE_C_COMPILER ${ARM_TOOLCHAIN_BIN}/arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER ${ARM_TOOLCHAIN_BIN}/arm-none-eabi-gcc)
set(CMAKE_OBJCOPY ${ARM_TOOLCHAIN_BIN}/arm-none-eabi-objcopy)
set(CMAKE_SIZE ${ARM_TOOLCHAIN_BIN}/arm-none-eabi-size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

