#pragma once

#include <stddef.h>
#include <stdint.h>

/* Recorded replies, embedded in flash by EMBED_FILES in main/CMakeLists.txt.
 * No filesystem, no SD card - the clips are part of the application image. */

typedef enum {
    AUDIO_HELLO = 0,   /* answered a voice */
    AUDIO_PICKUP,      /* picked up */
    AUDIO_OUCH,        /* tapped */
    AUDIO_SHAKE_HARD,  /* shaken hard */
    AUDIO_UPSIDE_DOWN, /* turned over */
    AUDIO_ID_COUNT,
} audio_id_t;

typedef struct {
    const uint8_t *pcm; /* 16-bit signed little-endian mono */
    size_t frames;
} audio_clip_t;

/* Returns NULL for an unknown id or a clip that failed to parse. */
const audio_clip_t *audio_asset(audio_id_t id);
