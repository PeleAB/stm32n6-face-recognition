cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# n6-platform: reusable STM32N6 static library
#
# Expected cache variables (set by the consumer before add_subdirectory):
#   N6PLAT_CUBE_FW   — absolute path to STM32Cube_FW_N6
#   N6PLAT_LIB_ROOT  — absolute path to Lib/
#   N6PLAT_INC_EXTRA — extra include directory (app's Inc/ — contains
#                      FreeRTOSConfig.h, stm32n6xx_hal_conf.h, app_config.h …)
# ---------------------------------------------------------------------------

# Source files that live in the consuming app's tree (suppressed there via
# HEADER_FILE_ONLY to avoid duplicate symbol errors with mx-generated.cmake).
set(N6PLAT_APP_SOURCES
    ${CMAKE_SOURCE_DIR}/Src/bsp/platform.c
    ${CMAKE_SOURCE_DIR}/Src/bsp/freertos_platform.c
    ${CMAKE_SOURCE_DIR}/Src/bsp/fuse_programming.c
    ${CMAKE_SOURCE_DIR}/Src/svc/buffer_queue.c
    ${CMAKE_SOURCE_DIR}/Src/svc/draw.c
    ${CMAKE_SOURCE_DIR}/Src/svc/nn_service.c
    ${CMAKE_SOURCE_DIR}/Src/svc/app_stats.c
    ${CMAKE_SOURCE_DIR}/Src/svc/utils.c
    ${CMAKE_SOURCE_DIR}/Src/sysobj/src/sysobj_cache.c
    ${CMAKE_SOURCE_DIR}/Src/sysobj/src/sysobj_dma2d.c
    ${CMAKE_SOURCE_DIR}/Src/sysobj/src/sysobj_encoder.c
    ${CMAKE_SOURCE_DIR}/Src/sysobj/src/sysobj_uart.c
)

# Source files that live in n6-platform itself (new platform modules).
set(N6PLAT_OWN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/sysobj/src/sysobj_params.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/sysobj/src/sysobj_timer.c
)

add_library(n6platform STATIC ${N6PLAT_APP_SOURCES} ${N6PLAT_OWN_SOURCES})

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
set(UVC_LIB_ROOT "${N6PLAT_LIB_ROOT}/uvcl")
set(USBX_ROOT    "${N6PLAT_CUBE_FW}/Middlewares/ST/usbx")

target_include_directories(n6platform
    PUBLIC
        # Sysobj headers (sysobj_params.h, sysobj_timer.h, etc.) must be
        # visible to consumers (main.c, app_pipeline.c …).
        ${CMAKE_SOURCE_DIR}/Src/sysobj/inc
    PRIVATE
        # App-specific config headers
        ${N6PLAT_INC_EXTRA}
        ${N6PLAT_INC_EXTRA}/app
        ${N6PLAT_INC_EXTRA}/svc
        ${N6PLAT_INC_EXTRA}/bsp
        # HAL / CMSIS
        ${N6PLAT_CUBE_FW}/Drivers/STM32N6xx_HAL_Driver/Inc
        ${N6PLAT_CUBE_FW}/Drivers/STM32N6xx_HAL_Driver/Inc/Legacy
        ${N6PLAT_CUBE_FW}/Drivers/CMSIS/Device/ST/STM32N6xx/Include
        ${N6PLAT_CUBE_FW}/Drivers/CMSIS/Include
        ${N6PLAT_CUBE_FW}/Drivers/CMSIS/DSP/Include
        # BSP
        ${N6PLAT_CUBE_FW}/Drivers/BSP/STM32N6570-DK
        ${N6PLAT_CUBE_FW}/Drivers/BSP/Components/Common
        ${N6PLAT_CUBE_FW}/Drivers/BSP/Components/aps256xx
        ${N6PLAT_CUBE_FW}/Drivers/BSP/Components/mx66uw1g45g
        ${N6PLAT_CUBE_FW}/Utilities/lcd
        ${N6PLAT_CUBE_FW}/Utilities/Fonts
        # Middleware — Video encoder
        ${N6PLAT_CUBE_FW}/Middlewares/Third_Party/VideoEncoder/inc
        ${N6PLAT_CUBE_FW}/Middlewares/Third_Party/VideoEncoder/source/common
        ${N6PLAT_CUBE_FW}/Middlewares/ST/VideoEncoder_EWL
        # FreeRTOS
        ${N6PLAT_LIB_ROOT}/FreeRTOS/Source/include
        ${N6PLAT_LIB_ROOT}/FreeRTOS/Source/portable/GCC/ARM_CM55_NTZ/non_secure
        # AI Runtime
        ${N6PLAT_LIB_ROOT}/AI_Runtime/Inc
        ${N6PLAT_LIB_ROOT}/AI_Runtime/Npu/ll_aton
        ${N6PLAT_LIB_ROOT}/AI_Runtime/Npu/Devices/STM32N6XX
        # Camera Middleware
        ${N6PLAT_LIB_ROOT}/Camera_Middleware
        ${N6PLAT_LIB_ROOT}/Camera_Middleware/sensors
        ${N6PLAT_LIB_ROOT}/Camera_Middleware/sensors/imx335
        ${N6PLAT_LIB_ROOT}/Camera_Middleware/sensors/vd55g1
        ${N6PLAT_LIB_ROOT}/Camera_Middleware/sensors/vd6g
        ${N6PLAT_LIB_ROOT}/Camera_Middleware/ISP_Library/isp/Inc
        ${N6PLAT_LIB_ROOT}/Camera_Middleware/ISP_Library/evision/Inc
        # Post-processing / AI wrappers
        ${N6PLAT_LIB_ROOT}/lib_vision_models_pp/lib_vision_models_pp/Inc
        ${N6PLAT_LIB_ROOT}/ai-postprocessing-wrapper
        # UVC / USBX
        ${UVC_LIB_ROOT}/Inc
        ${UVC_LIB_ROOT}/Src
        ${UVC_LIB_ROOT}/Inc/usbx
        ${UVC_LIB_ROOT}/Src/usbx
        ${USBX_ROOT}/common/core/inc
        ${USBX_ROOT}/ports/generic/inc
        ${USBX_ROOT}/common/usbx_device_classes/inc
        ${USBX_ROOT}/common/usbx_stm32_device_controllers
        # Model
        ${CMAKE_SOURCE_DIR}/Model
)

# ---------------------------------------------------------------------------
# Compile definitions (mirror mx-generated.cmake; keep in sync)
# ---------------------------------------------------------------------------
target_compile_definitions(n6platform PRIVATE
    ${APP_DISPLAY_COMPILE_DEFINITION}
    STM32N657xx
    USE_FULL_ASSERT
    USE_FULL_LL_DRIVER
    VECT_TAB_SRAM
    USE_IMX335_SENSOR
    USE_VD66GY_SENSOR
    USE_VD55G1_SENSOR
    TX_MAX_PARALLEL_NETWORKS=1
    FEAT_FREERTOS
    APP_HAS_PARALLEL_NETWORKS=0
    LL_ATON_PLATFORM=LL_ATON_PLAT_STM32N6
    LL_ATON_OSAL=LL_ATON_OSAL_FREERTOS
    LL_ATON_RT_MODE=LL_ATON_RT_ASYNC
    LL_ATON_SW_FALLBACK
    LL_ATON_DBG_BUFFER_INFO_EXCLUDED=1
    UVC_LIB_USE_DMA
    UVC_LIB_USE_USBX
    UX_INCLUDE_USER_DEFINE_FILE
    USBL_PACKET_PER_MICRO_FRAME=3
    UX_STANDALONE
    UVCL_USBX_USE_FREERTOS
    USE_DCACHE
)

# ---------------------------------------------------------------------------
# Compile options (same as the app target)
# ---------------------------------------------------------------------------
target_compile_options(n6platform PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wall
        -fstack-usage
        -fdata-sections
        -ffunction-sections
    >
)
