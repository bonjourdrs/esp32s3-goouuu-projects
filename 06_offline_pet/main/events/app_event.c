#include "app_event.h"

#include <string.h>

#include "esp_log.h"

#define TAG "EVENT"

#define EVENT_QUEUE_LEN 24

static QueueHandle_t s_queue;

esp_err_t app_event_init(void)
{
    s_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(app_event_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "cannot create the event queue");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_event_send(const app_event_t *event)
{
    if (s_queue == NULL) {
        return false;
    }
    if (xQueueSend(s_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped event %d", (int)event->type);
        return false;
    }
    return true;
}

bool app_event_receive(app_event_t *event, TickType_t timeout)
{
    if (s_queue == NULL) {
        return false;
    }
    return xQueueReceive(s_queue, event, timeout) == pdTRUE;
}
