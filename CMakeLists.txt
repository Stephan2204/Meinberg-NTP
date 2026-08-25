cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meinberg_ntp_wt32_eth01)

idf_component_get_property(lwip_lib lwip COMPONENT_LIB)

# Let lwIP find our project hook header.
target_compile_options(${lwip_lib} PRIVATE "-I${PROJECT_DIR}/src")

# 1) Add DHCP Option 7 (Log Server) to Option 55 Parameter Request List.
# 2) Pre-include our small ESP-IDF lwIP project hook header.
target_compile_definitions(${lwip_lib} PRIVATE
    "DHCP_ADD_EXTRA_REQUEST_OPTIONS=,7"
    "ESP_IDF_LWIP_HOOK_FILENAME=\"meinberg_lwip_hook.h\""
)
