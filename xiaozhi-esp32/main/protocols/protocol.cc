#include "protocol.h"
#include "assets.h"

#include <esp_log.h>

#define TAG "Protocol"

namespace {
// cJSON 输出后立即释放，避免调用方遗漏 cJSON_Delete/cJSON_free
std::string SerializeAndFree(cJSON* json) {
    if (json == nullptr) {
        return std::string();
    }
    char* str = cJSON_PrintUnformatted(json);
    std::string result = (str != nullptr) ? str : "";
    if (str != nullptr) {
        cJSON_free(str);
    }
    cJSON_Delete(json);
    return result;
}
}  // namespace

void Protocol::AddTextFontCapabilities(cJSON* root) {
    auto capability = Assets::GetInstance().text_font_capability();
    cJSON* features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsObject(features)) {
        cJSON_AddBoolToObject(features, "glyph_push", capability.glyph_push);
    }

    if (!capability.glyph_push) {
        return;
    }
    cJSON* font = cJSON_CreateObject();
    cJSON_AddStringToObject(font, "bundle", capability.bundle.c_str());
    cJSON_AddStringToObject(font, "charset", capability.charset.c_str());
    cJSON_AddNumberToObject(font, "size", capability.size);
    cJSON_AddNumberToObject(font, "bpp", capability.bpp);
    cJSON_AddItemToObject(root, "text_font", font);
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) { on_connected_ = callback; }

void Protocol::OnDisconnected(std::function<void()> callback) { on_disconnected_ = callback; }

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        return;
    }
    cJSON_AddStringToObject(json, "session_id", session_id().c_str());
    cJSON_AddStringToObject(json, "type", "abort");
    if (reason == kAbortReasonWakeWordDetected) {
        cJSON_AddStringToObject(json, "reason", "wake_word_detected");
    }
    SendText(SerializeAndFree(json));
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        return;
    }
    cJSON_AddStringToObject(json, "session_id", session_id().c_str());
    cJSON_AddStringToObject(json, "type", "listen");
    cJSON_AddStringToObject(json, "state", "detect");
    cJSON_AddStringToObject(json, "text", wake_word.c_str());
    SendText(SerializeAndFree(json));
}

void Protocol::SendStartListening(ListeningMode mode) {
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        return;
    }
    cJSON_AddStringToObject(json, "session_id", session_id().c_str());
    cJSON_AddStringToObject(json, "type", "listen");
    cJSON_AddStringToObject(json, "state", "start");
    if (mode == kListeningModeRealtime) {
        cJSON_AddStringToObject(json, "mode", "realtime");
    } else if (mode == kListeningModeAutoStop) {
        cJSON_AddStringToObject(json, "mode", "auto");
    } else {
        cJSON_AddStringToObject(json, "mode", "manual");
    }
    SendText(SerializeAndFree(json));
}

void Protocol::SendStopListening() {
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        return;
    }
    cJSON_AddStringToObject(json, "session_id", session_id().c_str());
    cJSON_AddStringToObject(json, "type", "listen");
    cJSON_AddStringToObject(json, "state", "stop");
    SendText(SerializeAndFree(json));
}

void Protocol::SendMcpMessage(const std::string& payload) {
    cJSON* payload_json = cJSON_Parse(payload.c_str());
    if (payload_json == nullptr) {
        ESP_LOGE(TAG, "SendMcpMessage: invalid JSON payload");
        return;
    }
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        cJSON_Delete(payload_json);
        return;
    }
    cJSON_AddStringToObject(json, "session_id", session_id().c_str());
    cJSON_AddStringToObject(json, "type", "mcp");
    cJSON_AddItemToObject(json, "payload", payload_json);
    SendText(SerializeAndFree(json));
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    std::chrono::time_point<std::chrono::steady_clock> last_incoming;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_incoming = last_incoming_time_;
    }
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
