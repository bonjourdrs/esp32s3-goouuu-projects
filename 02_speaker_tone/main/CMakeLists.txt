idf_component_register(SRCS "main.c" "audio_tone.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver
                       PRIV_REQUIRES esp_driver_i2s)
