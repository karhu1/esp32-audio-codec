# Two separate firmware modes share this component:
#   idf.py -DAPP_MODE=benchmark build   (default) -> opus_benchmark_main.c, unchanged
#   idf.py -DAPP_MODE=stream build                -> stream_main.c + stream_codec.c
# CMake remembers APP_MODE in its cache, so pass it again when switching back.
if(NOT DEFINED APP_MODE)
    set(APP_MODE "benchmark")
endif()

if(APP_MODE STREQUAL "stream")
    set(MAIN_SRCS "stream_main.c" "stream_codec.c")
else()
    set(MAIN_SRCS "opus_benchmark_main.c")
endif()

idf_component_register(SRCS ${MAIN_SRCS}
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES opus esp_timer esp_driver_uart log)