idf_component_register(
    SRCS
        "main.c"
        "emulator.c"
        "env_source.c"
        "display.c"
        "logger.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        bt
        nvs_flash
        esp_timer
        esp_driver_usb_serial_jtag
        esp_hw_support
        console
)

# Two apps share these sources, selected at configure time:
#   idf.py build                  -> emulator (default)
#   idf.py -DAPP=probe build      -> BLE central probe
# main.c is the probe (guarded by #ifndef APP_EMULATOR); emulator.c is the emulator
# (guarded by #ifdef APP_EMULATOR).
if(NOT DEFINED APP)
    set(APP "emulator")
endif()
if(APP STREQUAL "emulator")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_EMULATOR=1)
    message(STATUS "Delonghi app: EMULATOR")
else()
    message(STATUS "Delonghi app: PROBE (BLE central)")
endif()

if(CLONE_MAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CLONE_MAC=1)
    message(STATUS "Delonghi emulator: CLONE_MAC enabled")
endif()
