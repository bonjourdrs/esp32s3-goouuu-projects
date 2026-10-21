#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() { event_group_handle_ = xEventGroupCreate(); }

WebsocketProtocol::~WebsocketProtocol() { vEventGroupDelete(event_group_handle_); }

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        MarkIncomingTime();
        if (binary) {
            // 网络数据不可信：先做长度校验，且不原地修改回调 buffer
            constexpr size_t kMaxAudioPayload = 32 * 1024;
            const auto* raw = reinterpret_cast<const uint8_t*>(data);
            if (version_ == 2) {
                if (len < sizeof(BinaryProtocol2)) {
                    ESP_LOGE(TAG, "Invalid v2 binary frame, len=%u", (unsigned)len);
                    return;
                }
                BinaryProtocol2 header;
                memcpy(&header, data, sizeof(header));
                uint32_t payload_size = ntohl(header.payload_size);
                if (payload_size > len - sizeof(BinaryProtocol2) ||
                    payload_size > kMaxAudioPayload) {
                    ESP_LOGE(TAG, "Invalid v2 payload size %u, frame len=%u",
                             (unsigned)payload_size, (unsigned)len);
                    return;
                }
                if (on_incoming_audio_ != nullptr) {
                    const uint8_t* payload = raw + sizeof(BinaryProtocol2);
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_.load(),
                        .frame_duration = server_frame_duration_.load(),
                        .timestamp = ntohl(header.timestamp),
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)}));
                }
            } else if (version_ == 3) {
                if (len < sizeof(BinaryProtocol3)) {
                    ESP_LOGE(TAG, "Invalid v3 binary frame, len=%u", (unsigned)len);
                    return;
                }
                BinaryProtocol3 header;
                memcpy(&header, data, sizeof(header));
                uint16_t payload_size = ntohs(header.payload_size);
                if (payload_size > len - sizeof(BinaryProtocol3) ||
                    payload_size > kMaxAudioPayload) {
                    ESP_LOGE(TAG, "Invalid v3 payload size %u, frame len=%u",
                             (unsigned)payload_size, (unsigned)len);
                    return;
                }
                if (on_incoming_audio_ != nullptr) {
                    const uint8_t* payload = raw + sizeof(BinaryProtocol3);
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_.load(),
                        .frame_duration = server_frame_duration_.load(),
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)}));
                }
            } else if (on_incoming_audio_ != nullptr) {
                on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .sample_rate = server_sample_rate_.load(),
                    .frame_duration = server_frame_duration_.load(),
                    .timestamp = 0,
                    .payload = std::vector<uint8_t>(raw, raw + len)}));
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGE(TAG, "Failed to parse JSON message");
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits =
        xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id_item = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id_item)) {
        SetSessionId(session_id_item->valuestring);
        ESP_LOGI(TAG, "Session ID: %s", session_id().c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            const int rate = sample_rate->valueint;
            if (rate == 8000 || rate == 16000 || rate == 24000 || rate == 48000) {
                server_sample_rate_ = rate;
            } else {
                ESP_LOGW(TAG, "Unsupported sample_rate %d from server, keep %d", rate,
                         server_sample_rate_.load());
            }
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            const int duration = frame_duration->valueint;
            if (duration == 10 || duration == 20 || duration == 40 || duration == 60) {
                server_frame_duration_ = duration;
            } else {
                ESP_LOGW(TAG, "Unsupported frame_duration %d from server, keep %d", duration,
                         server_frame_duration_.load());
            }
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
