#include "server.h"
#include "pairing.h"
#include "assistant_profiles.h"
#include "application.h"
#include "power_policy.h"
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <mutex>
#include <memory>
#include <cstdio>
#include <cstring>

extern const uint8_t setup_page_start[] asm("_binary_valet_setup_html_gz_start");
extern const uint8_t setup_page_end[] asm("_binary_valet_setup_html_gz_end");
namespace valet {
namespace {
std::mutex s_mutex;
SetupStatus s_status;
std::string s_access;
std::string s_origin;
int64_t s_expires = 0;
httpd_handle_t s_server = nullptr; // Network domain owns server lifetime.
bool s_hold = false;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const char* String(const cJSON* root, const char* key) {
    const auto* v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
esp_err_t Reply(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(req, body);
}
esp_err_t JsonReply(httpd_req_t* req, cJSON* root) {
    if (!root) return Reply(req, "500 Internal Server Error", "{\"error\":\"内存不足\"}");
    char* body = cJSON_PrintUnformatted(root);
    if (!body) return Reply(req, "500 Internal Server Error", "{\"error\":\"内存不足\"}");
    const auto result = Reply(req, "200 OK", body);
    cJSON_free(body);
    return result;
}
bool Authorized(httpd_req_t* req, uint32_t& generation) {
    char token[65]{};
    char origin[96]{};
    if (httpd_req_get_hdr_value_str(req, "X-Valet-Setup", token, sizeof(token)) != ESP_OK) return false;
    const auto origin_size = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_size && httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_status.running || esp_timer_get_time() >= s_expires || s_access != token ||
        (origin_size && s_origin != origin)) return false;
    generation = s_status.generation;
    return true;
}
esp_err_t Page(httpd_req_t* req) {
    const auto status = GetSetupStatus();
    if (!status.running) return Reply(req, "410 Gone", "{\"error\":\"配置已关闭，请在设备重新添加助理\"}");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    return httpd_resp_send(req, reinterpret_cast<const char*>(setup_page_start), setup_page_end - setup_page_start);
}
esp_err_t Profiles(httpd_req_t* req) {
    uint32_t generation;
    if (!Authorized(req, generation)) return Reply(req, "403 Forbidden", "{\"error\":\"配置已过期，请重新扫描设备二维码\"}");
    const auto status = GetSetupStatus();
    const auto profiles = LoadAssistantProfiles();
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) return JsonReply(req, nullptr);
    cJSON_AddStringToObject(root.get(), "active", profiles.active_id.c_str());
    cJSON_AddBoolToObject(root.get(), "saving", status.saving);
    cJSON_AddStringToObject(root.get(), "note", status.note.c_str());
    cJSON_AddStringToObject(root.get(), "transport", "passport-v1");
    auto* items = cJSON_AddArrayToObject(root.get(), "items");
    if (!items) return JsonReply(req, nullptr);
    for (const auto& profile : profiles.items) {
        auto* item = cJSON_CreateObject();
        if (!item) return JsonReply(req, nullptr);
        cJSON_AddItemToArray(items, item);
        cJSON_AddStringToObject(item, "id", profile.id.c_str());
        cJSON_AddStringToObject(item, "name", profile.name.c_str());
        cJSON_AddStringToObject(item, "backend", profile.connection.backend == custom_chat::Backend::Hermes ? "Hermes" : "OpenClaw");
        cJSON_AddStringToObject(item, "url", profile.connection.url.c_str());
        // Codes and tokens are write-only, including after the device is read back.
    }
    return JsonReply(req, root.get());
}
esp_err_t Mutate(httpd_req_t* req) {
    uint32_t generation;
    if (!Authorized(req, generation)) return Reply(req, "403 Forbidden", "{\"error\":\"配置已关闭或过期\"}");
    if (req->content_len <= 0 || req->content_len > 6144) return Reply(req, "413 Content Too Large", "{\"error\":\"配对回复过长\"}");
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int n = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (n <= 0) return Reply(req, "400 Bad Request", "{\"error\":\"没有收到完整回复，请重试\"}");
        received += n;
    }
    Json root(cJSON_ParseWithOpts(body.c_str(), nullptr, true), cJSON_Delete);
    if (!root || body.find('\0') != std::string::npos) return Reply(req, "400 Bad Request", "{\"error\":\"JSON 格式错误\"}");
    const std::string action = String(root.get(), "action");
    const std::string id = String(root.get(), "id");
    AssistantProfile profile;
    if (action == "add") {
        if (const char* error = ParsePairing(body, profile)) {
            Json result(cJSON_CreateObject(), cJSON_Delete);
            cJSON_AddStringToObject(result.get(), "error", error);
            char* message = cJSON_PrintUnformatted(result.get());
            const auto response = Reply(req, "400 Bad Request", message ? message : "{}");
            cJSON_free(message);
            return response;
        }
        uint32_t random[2]; esp_fill_random(random, sizeof(random));
        char generated[32]; std::snprintf(generated, sizeof(generated), "agent-%08lx%08lx", static_cast<unsigned long>(random[0]), static_cast<unsigned long>(random[1]));
        profile.id = generated;
    } else if (action != "select" && action != "remove") return Reply(req, "400 Bad Request", "{\"error\":\"未知操作\"}");
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_status.generation != generation || !s_status.running || esp_timer_get_time() >= s_expires)
            return Reply(req, "410 Gone", "{\"error\":\"配置已关闭\"}");
        if (s_status.saving) return Reply(req, "409 Conflict", "{\"error\":\"正在保存，请稍候\"}");
        s_status.saving = true;
        s_status.note = "正在保存到设备";
    }
    if (!Application::GetInstance().Schedule([generation, action, id, profile]() {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_status.running || s_status.generation != generation || esp_timer_get_time() >= s_expires) return;
        }
        auto profiles = LoadAssistantProfiles();
        bool changed = false;
        if (action == "add") {
            bool exists = false;
            for (const auto& item : profiles.items) if (item.connection.url == profile.connection.url) exists = true;
            if (!exists) changed = profiles.Upsert(profile) && profiles.Select(profile.id);
        } else if (action == "select") changed = profiles.Select(id);
        else changed = profiles.Remove(id);
        const bool saved = changed && SaveAssistantProfiles(profiles);
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_status.generation != generation) return;
        s_status.saving = false;
        s_status.note = saved ? "已保存到设备。请在设备进入提问或通话，完成签名配对与语音测试。" :
            "未保存。请检查是否重复添加、连接数或存储空间已满。";
    }, Application::TaskDomain::Storage)) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.saving = false;
        s_status.note = "设备繁忙，请重试";
        return Reply(req, "503 Service Unavailable", "{\"error\":\"设备繁忙，请重试\"}");
    }
    return Reply(req, "202 Accepted", "{\"saving\":true}");
}
}
SetupStatus GetSetupStatus() {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto result = s_status;
    if (result.running && esp_timer_get_time() >= s_expires) { result.running = false; result.note = "配置已过期，请重新打开"; }
    return result;
}
bool StartSetupServer() {
    StopSetupServer();
    wifi_ap_record_t ap{};
    esp_netif_ip_info_t ip{};
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.note = "请先让设备与手机连接同一个 Wi-Fi";
        return false;
    }
    uint8_t random[24]; esp_fill_random(random, sizeof(random));
    char access[49]{};
    for (size_t i = 0; i < sizeof(random); ++i) std::snprintf(access + i * 2, 3, "%02x", random[i]);
    char origin[80]; std::snprintf(origin, sizeof(origin), "http://" IPSTR ":8080", IP2STR(&ip.ip));
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_origin = origin; s_access = access; s_expires = esp_timer_get_time() + 600LL * 1000000;
        s_status.url = s_origin + "/setup#" + s_access;
        s_status.note = "等待手机或电脑打开配置页";
        s_status.saving = false;
        ++s_status.generation;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080; config.ctrl_port = 32769;
    config.stack_size = 8192; config.max_uri_handlers = 3;
    config.max_open_sockets = 2; config.recv_wait_timeout = 5; config.send_wait_timeout = 5;
    if (httpd_start(&s_server, &config) != ESP_OK) { StopSetupServer(); return false; }
    const httpd_uri_t routes[] = {
        {.uri = "/setup", .method = HTTP_GET, .handler = Page},
        {.uri = "/api/assistants", .method = HTTP_GET, .handler = Profiles},
        {.uri = "/api/assistants", .method = HTTP_POST, .handler = Mutate},
    };
    for (const auto& route : routes) if (httpd_register_uri_handler(s_server, &route) != ESP_OK) { StopSetupServer(); return false; }
    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    s_hold = true;
    std::lock_guard<std::mutex> lock(s_mutex);
    s_status.running = true;
    return true;
}
void StopSetupServer() {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.running = false; s_status.saving = false; s_access.clear(); ++s_status.generation;
    }
    if (s_server) { httpd_stop(s_server); s_server = nullptr; }
    if (s_hold) {
        PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        s_hold = false;
    }
}
void CloseSetupSession() {
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_status.running && s_access.empty()) return;
        s_status.running = false; s_status.saving = false; s_access.clear();
        generation = ++s_status.generation;
    }
    Application::GetInstance().Schedule([generation] {
        { std::lock_guard<std::mutex> lock(s_mutex); if (generation != s_status.generation) return; }
        StopSetupServer();
    }, Application::TaskDomain::Network);
}
}
