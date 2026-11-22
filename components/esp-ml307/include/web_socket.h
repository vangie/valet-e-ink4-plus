#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <atomic>
#include <functional>
#include <string>
#include <map>
#include <thread>
#include <vector>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "tcp.h"

class NetworkInterface;

class WebSocket {
public:
    WebSocket(NetworkInterface* network, int connect_id);
    ~WebSocket();

    void SetHeader(const char* key, const char* value);
    void SetReceiveBufferSize(size_t size);
    // 建连（含 TLS 握手）和握手请求的总时限，默认 20 秒。
    void SetConnectTimeout(int ms) { connect_timeout_ms_ = ms; }
    // 最后一次从服务器收到数据的时刻（esp_timer 微秒），用来发现悄悄断掉的连接。
    int64_t LastReceiveUs() const { return last_receive_us_.load(); }
    bool IsConnected() const;
    bool Connect(const char* uri);
    bool Send(const std::string& data);
    bool Send(const void* data, size_t len, bool binary = false, bool fin = true);
    void Ping();
    void Close();

    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);
    void OnData(std::function<void(const char*, size_t, bool binary)> callback);
    void OnError(std::function<void(int)> callback);

    // 获取最后一次错误码
    int GetLastError();

private:
    NetworkInterface* network_;
    int connect_id_;
    std::unique_ptr<Tcp> tcp_;
    bool continuation_ = false;
    std::vector<char> current_message_;
    bool is_fragmented_ = false;
    bool is_binary_ = false;
    size_t receive_buffer_size_ = 2048;
    std::string receive_buffer_;
    bool handshake_completed_ = false;
    bool connected_ = false;
    int connect_timeout_ms_ = 20000;
    std::atomic<int64_t> last_receive_us_{0};

    // Mutex for sending data and replying pong
    std::mutex send_mutex_;
    
    EventGroupHandle_t handshake_event_group_;
    static const EventBits_t HANDSHAKE_SUCCESS_BIT = BIT0;
    static const EventBits_t HANDSHAKE_FAILED_BIT = BIT1;

    std::map<std::string, std::string> headers_;
    std::function<void(const char*, size_t, bool binary)> on_data_;
    std::function<void(int)> on_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    void OnTcpData(const std::string& data);
    void FailHandshake(const char* reason);
    bool SendControlFrame(uint8_t opcode, const void* data, size_t len);
};

#endif // WEBSOCKET_H
