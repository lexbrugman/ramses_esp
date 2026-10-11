idf_component_register(
  SRCS ramses_network.c network_cmd.c ramses_wifi.c wifi_cmd.c ramses_ota.c ota_cmd.c ramses_sntp.c
  INCLUDE_DIRS "."
  REQUIRES nvs_flash esp_wifi esp_netif console mbedtls app_update esp_partition esp_https_ota esp_http_client
  PRIV_REQUIRES command
)

# The OTA channel: CI sets the repo it runs in and the "<branch>-latest"
# release it publishes the build to (see firmware.yml), so a binary keeps
# following the branch it was built from. Local builds and forks follow
# upstream main.
if(DEFINED ENV{FW_OTA_REPO})
  set(FW_OTA_REPO "$ENV{FW_OTA_REPO}")
else()
  set(FW_OTA_REPO "lexbrugman/ramses_esp")
endif()
if(DEFINED ENV{FW_OTA_CHANNEL})
  set(FW_OTA_CHANNEL "$ENV{FW_OTA_CHANNEL}")
else()
  set(FW_OTA_CHANNEL "main-latest")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  FW_OTA_REPO="${FW_OTA_REPO}"
  FW_OTA_CHANNEL="${FW_OTA_CHANNEL}"
)
