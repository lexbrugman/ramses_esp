idf_component_register(
  SRCS "ramses-mqtt.c"
  INCLUDE_DIRS "."
  REQUIRES mqtt json
  PRIV_REQUIRES ramses-network gateway message command esp_timer
)

