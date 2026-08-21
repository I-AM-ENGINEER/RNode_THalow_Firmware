idf_component_register(
    SRCS "test_main.c"
         "../../../src/rns_framing.c"
         "../../../src/kiss.c"
         "../../../src/gzip_inflate.c"
         "../../../src/switch.c"
    INCLUDE_DIRS "." "../../../inc"
    REQUIRES unity heap esp_timer)

# Unity in IDF needs this for the runner macros
target_compile_definitions(${COMPONENT_LIB} PRIVATE UNITY_INCLUDE_CONFIG_H=0)
