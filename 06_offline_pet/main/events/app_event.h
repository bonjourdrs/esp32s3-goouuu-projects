#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Everything the sensors decide gets funnelled through here. Detectors post
 * events, the pet state machine consumes them, and neither knows about the
 * other. */

typedef enum {
    EVENT_NONE = 0,

    /* audio detector */
    EVENT_VOICE_END,

    /* motion detector */
    EVENT_PICK_UP,
    EVENT_TAP,
    EVENT_SHAKE_HARD,
    EVENT_UPSIDE_DOWN,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    float value;          /* peak g, RMS, angle - whatever explains the event */
    uint32_t timestamp_ms;
} app_event_t;

esp_err_t app_event_init(void);

/* Non-blocking post. Returns false when the queue is full. */
bool app_event_send(const app_event_t *event);

/* Blocking receive. Returns false only on timeout. */
bool app_event_receive(app_event_t *event, TickType_t timeout);
