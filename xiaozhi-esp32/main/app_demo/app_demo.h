#ifndef APP_DEMO_H
#define APP_DEMO_H

#include <string>

namespace appdemo {

// Android V2 App 对接 Demo 固件的总入口。
// 刻意不启动音频 / 云端协议 / AI 对话，只跑 BLE + 选路网络 + 传感器状态。
class AppDemo {
public:
    static AppDemo& GetInstance();

    void Start();

private:
    AppDemo() = default;
    AppDemo(const AppDemo&) = delete;
    AppDemo& operator=(const AppDemo&) = delete;

    void HandleCommand(const std::string& json);
    void SendAck(int id, bool ok, const char* error = nullptr);
};

}  // namespace appdemo

#endif  // APP_DEMO_H
