#include "audio_assets.h"

#include "esp_log.h"

#define TAG "ASSET"

/* Symbols produced by EMBED_FILES. The IDF build only keeps the file's base
 * name, so "assets/wav/hello.wav" becomes _binary_hello_wav_start. */
extern const uint8_t hello_wav_start[]       asm("_binary_hello_wav_start");
extern const uint8_t hello_wav_end[]         asm("_binary_hello_wav_end");
extern const uint8_t pickup_wav_start[]      asm("_binary_pickup_wav_start");
extern const uint8_t pickup_wav_end[]        asm("_binary_pickup_wav_end");
extern const uint8_t ouch_wav_start[]        asm("_binary_ouch_wav_start");
extern const uint8_t ouch_wav_end[]          asm("_binary_ouch_wav_end");
extern const uint8_t shake_hard_wav_start[]  asm("_binary_shake_hard_wav_start");
extern const uint8_t shake_hard_wav_end[]    asm("_binary_shake_hard_wav_end");
extern const uint8_t upside_down_wav_start[] asm("_binary_upside_down_wav_start");
extern const uint8_t upside_down_wav_end[]   asm("_binary_upside_down_wav_end");

/* Walk the chunk list instead of assuming a 44-byte header, so the clips keep
 * working if the encoder ever emits extra chunks. */
static const uint8_t *find_pcm(const uint8_t *start, const uint8_t *end, size_t *frames)
{
    *frames = 0;

    if (end - start < 12) {
        return NULL;
    }

    const uint8_t *p = start + 12; /* skip "RIFF"<size>"WAVE" */
    while (p + 8 <= end) {
        uint32_t size = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                        ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        if (p[0] == 'd' && p[1] == 'a' && p[2] == 't' && p[3] == 'a') {
            *frames = size / 2;
            return p + 8;
        }
        p += 8 + size + (size & 1);
    }
    return NULL;
}

static audio_clip_t s_clips[AUDIO_ID_COUNT] = {
    [AUDIO_HELLO]       = { .pcm = hello_wav_start,       .frames = 0 },
    [AUDIO_PICKUP]      = { .pcm = pickup_wav_start,      .frames = 0 },
    [AUDIO_OUCH]        = { .pcm = ouch_wav_start,        .frames = 0 },
    [AUDIO_SHAKE_HARD]  = { .pcm = shake_hard_wav_start,  .frames = 0 },
    [AUDIO_UPSIDE_DOWN] = { .pcm = upside_down_wav_start, .frames = 0 },
};

static const uint8_t *const s_ends[AUDIO_ID_COUNT] = {
    [AUDIO_HELLO]       = hello_wav_end,
    [AUDIO_PICKUP]      = pickup_wav_end,
    [AUDIO_OUCH]        = ouch_wav_end,
    [AUDIO_SHAKE_HARD]  = shake_hard_wav_end,
    [AUDIO_UPSIDE_DOWN] = upside_down_wav_end,
};

static bool s_parsed;

static void parse_once(void)
{
    if (s_parsed) {
        return;
    }
    s_parsed = true;

    for (int i = 0; i < AUDIO_ID_COUNT; ++i) {
        size_t frames = 0;
        const uint8_t *pcm = find_pcm(s_clips[i].pcm, s_ends[i], &frames);
        if (pcm == NULL || frames == 0) {
            ESP_LOGW(TAG, "clip %d has no usable data chunk", i);
            continue;
        }
        s_clips[i].pcm = pcm;
        s_clips[i].frames = frames;
    }
}

const audio_clip_t *audio_asset(audio_id_t id)
{
    if (id < 0 || id >= AUDIO_ID_COUNT) {
        return NULL;
    }

    parse_once();

    const audio_clip_t *clip = &s_clips[id];
    return clip->frames > 0 ? clip : NULL;
}
