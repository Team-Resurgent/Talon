# Talon — XID gamepad: descriptors, custom TinyUSB class driver, input state,
# WiFi station, and the web UI (embedded index.html).
idf_component_register(
    SRCS "talon_main.c"
         "xid_descriptors.c"
         "xid_class.c"
         "talon_input.c"
         "talon_igr.c"
         "wifi_net.c"
         "webui.c"
         "bt_host.c"
         "bt_hidmap.c"
         "esp_hid_gap.c"
         "led_status.c"
         "ota.c"
         "usb_owner.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html" "setup.html" "ota.html"
    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_http_server
                  esp_hw_support esp_driver_gpio esp_timer wpa_supplicant
                  bt esp_hid app_update
)

# Byte-faithful EP0 size 8 for the Duke (esp_tinyusb defaults to 64). Inject the
# setting the TinyUSB library must share with our sources directly onto its
# target (esp_tinyusb passes its own tusb_config.h to the library privately).
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(talon_tusb_name tinyusb)              # local component
else()
    set(talon_tusb_name espressif__tinyusb)   # managed component
endif()
idf_component_get_property(talon_tusb_lib ${talon_tusb_name} COMPONENT_LIB)
target_compile_definitions(${talon_tusb_lib} PRIVATE CFG_TUD_ENDPOINT0_SIZE=8)
