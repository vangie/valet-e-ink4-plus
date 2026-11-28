#include "server.h"
#include "pairing.h"
#include "assistant_profiles.h"
#include "library_sources.h"
#include "protocols/valet_protocol.h"
#include "application.h"
#include "power_policy.h"
#include "board.h"
#include "dual_network_board.h"
#include "wifi_station.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <mutex>
#include <utility>
#include <memory>
#include <cstdio>
#include <cstring>
#include <algorithm>

extern const uint8_t setup_page_start[] asm("_binary_valet_setup_html_gz_start");
extern const uint8_t setup_page_end[] asm("_binary_valet_setup_html_gz_end");
namespace valet {
namespace {
const char* TAG = "ValetSetup";
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
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; img-src 'self' data:; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
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
    cJSON_AddBoolToObject(root.get(), "paired", status.paired);
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
        cJSON_AddBoolToObject(item, "paired", !profile.device_token.empty());
        // Codes and tokens are write-only, including after the device is read back.
    }
    return JsonReply(req, root.get());
}
// 保存后立刻配对，并在配置页开着的这段时间里每 4 秒重试：这样「需要批准」当场出现，
// 批准后自动换到长期凭据，不用等用户以后进提问页，那时配对码多半已经过期（网关给 10 分钟）。
void PairWhileOpen(uint32_t generation, std::string id) {
    const auto still_open = [generation] {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_status.running && s_status.generation == generation && esp_timer_get_time() < s_expires;
    };
    const auto note = [generation](const std::string& text, bool paired) {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_status.generation != generation) return;
        s_status.note = text; s_status.paired = paired;
    };
    if (!still_open()) return;
    const auto result = ValetProtocol::PairNow(id);
    using Pairing = ValetProtocol::Pairing;
    if (result == Pairing::Paired) { note("已连接。现在可以在设备上提问了。", true); return; }
    note(ValetProtocol::PairingText(result), false);
    if (result != Pairing::NeedsApproval && result != Pairing::Unreachable) return;   // 重试也没用
    if (!still_open()) return;
    // 不占住 Network 任务：隔几秒再排一次。定时器只建一个，反复使用。
    static esp_timer_handle_t s_pair_timer = nullptr;
    static uint32_t s_pair_generation = 0;
    static std::string s_pair_id;
    { std::lock_guard<std::mutex> lock(s_mutex); s_pair_generation = generation; s_pair_id = std::move(id); }
    if (!s_pair_timer) {
        const esp_timer_create_args_t args = {
            .callback = [](void*) {
                uint32_t generation; std::string id;
                { std::lock_guard<std::mutex> lock(s_mutex); generation = s_pair_generation; id = s_pair_id; }
                Application::GetInstance().Schedule([generation, id] { PairWhileOpen(generation, id); },
                                                    Application::TaskDomain::Network);
            },
            .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "valet_pair", .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_pair_timer) != ESP_OK) { s_pair_timer = nullptr; return; }
    }
    esp_timer_stop(s_pair_timer);
    esp_timer_start_once(s_pair_timer, 4 * 1000000);
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
    } else if (action == "rename") {
        profile.name = String(root.get(), "name");
        // 前后空白去掉；不能空、不能太长（和添加时一样最多 32 个字）、不能有控制字符
        while (!profile.name.empty() && (profile.name.back() == ' ' || profile.name.back() == '\t')) profile.name.pop_back();
        while (!profile.name.empty() && (profile.name.front() == ' ' || profile.name.front() == '\t')) profile.name.erase(0, 1);
        size_t chars = 0;
        bool bad = false;
        for (unsigned char c : profile.name) { if ((c & 0xC0) != 0x80) ++chars; if (c < 0x20 || c == 0x7F) bad = true; }
        if (profile.name.empty() || chars > 32 || bad) return Reply(req, "400 Bad Request", "{\"error\":\"名字不能空，最多 32 个字\"}");
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
        } else if (action == "rename") {
            for (auto& item : profiles.items)
                if (item.id == id && item.name != profile.name) { item.name = profile.name; changed = ValidProfile(item); }
        } else if (action == "select") changed = profiles.Select(id);
        else changed = profiles.Remove(id);
        const bool saved = changed && SaveAssistantProfiles(profiles);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_status.generation != generation) return;
            s_status.saving = false;
            s_status.paired = false;
            s_status.note = !saved ? "未保存。请检查是否重复添加、连接数或存储空间已满。"
                          : action == "add" ? "已保存，正在连接助理…" : "已保存到设备。";
        }
        if (saved && action == "add")
            Application::GetInstance().Schedule([generation, id = profile.id] { PairWhileOpen(generation, id); },
                                                Application::TaskDomain::Network);
    }, Application::TaskDomain::Storage)) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.saving = false;
        s_status.note = "设备繁忙，请重试";
        return Reply(req, "503 Service Unavailable", "{\"error\":\"设备繁忙，请重试\"}");
    }
    return Reply(req, "202 Accepted", "{\"saving\":true}");
}
}
// ---- 书库订阅（OPDS）：配置页的「书库订阅」卡片 ----
esp_err_t Libraries(httpd_req_t* req) {
    uint32_t generation;
    if (!Authorized(req, generation)) return Reply(req, "403 Forbidden", "{\"error\":\"配置已关闭或过期\"}");
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) return JsonReply(req, nullptr);
    auto* items = cJSON_AddArrayToObject(root.get(), "items");
    if (!items) return JsonReply(req, nullptr);
    for (const auto& item : LoadLibrarySources()) {
        auto* entry = cJSON_CreateObject();
        if (!entry) return JsonReply(req, nullptr);
        cJSON_AddItemToArray(items, entry);
        cJSON_AddStringToObject(entry, "id", item.id.c_str());
        cJSON_AddStringToObject(entry, "name", item.name.c_str());
        cJSON_AddStringToObject(entry, "url", item.url.c_str());
        if (!item.member.empty()) cJSON_AddStringToObject(entry, "member", item.member.c_str());
        if (!item.user.empty()) cJSON_AddStringToObject(entry, "user", item.user.c_str());
        cJSON_AddBoolToObject(entry, "auth", !item.password.empty() || !item.user.empty());
        cJSON_AddBoolToObject(entry, "rich", item.rich);
    }
    return JsonReply(req, root.get());
}
esp_err_t LibrariesMutate(httpd_req_t* req) {
    uint32_t generation;
    if (!Authorized(req, generation)) return Reply(req, "403 Forbidden", "{\"error\":\"配置已关闭或过期\"}");
    if (req->content_len <= 0 || req->content_len > 1024) return Reply(req, "413 Content Too Large", "{\"error\":\"请求过长\"}");
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int n = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (n <= 0) return Reply(req, "400 Bad Request", "{\"error\":\"没有收到完整请求，请重试\"}");
        received += n;
    }
    Json root(cJSON_ParseWithOpts(body.c_str(), nullptr, true), cJSON_Delete);
    if (!root || body.find('\0') != std::string::npos) return Reply(req, "400 Bad Request", "{\"error\":\"JSON 格式错误\"}");
    const std::string action = String(root.get(), "action");
    auto items = LoadLibrarySources();
    if (action == "add") {
        std::string url = String(root.get(), "url");
        while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
        while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);
        if (!LibraryUrlValid(url)) return Reply(req, "400 Bad Request", "{\"error\":\"不是书库链接：要以 http:// 或 https:// 开头\"}");
        std::string name = String(root.get(), "name");
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
        std::string user = String(root.get(), "user");
        std::string password = String(root.get(), "password");
        for (auto& item : items) {
            if (item.url != url) continue;
            if (!name.empty()) item.name = name;
            if (!user.empty() || !password.empty()) {
                item.user = user;
                item.password = password;
            }
            SaveLibrarySources(items);
            Json ok(cJSON_CreateObject(), cJSON_Delete);
            if (!ok) return JsonReply(req, nullptr);
            cJSON_AddStringToObject(ok.get(), "name", item.name.c_str());
            return JsonReply(req, ok.get());
        }
        if (items.size() >= kMaxLibraries) return Reply(req, "409 Conflict", "{\"error\":\"最多订阅 8 个书库\"}");
        LibrarySource item;
        item.id = NewLibraryId();
        item.url = url;
        item.name = name.empty() ? LibraryNameFromUrl(url) : name;
        item.user = user;
        item.password = password;
        item.rich = LibraryRichUrl(url);
        items.push_back(item);
        if (!SaveLibrarySources(items)) return Reply(req, "500 Internal Server Error", "{\"error\":\"存储空间不足，没能保存\"}");
        Json ok(cJSON_CreateObject(), cJSON_Delete);
        if (!ok) return JsonReply(req, nullptr);
        cJSON_AddStringToObject(ok.get(), "name", item.name.c_str());
        return JsonReply(req, ok.get());
    }
    if (action == "remove") {
        const std::string id = String(root.get(), "id");
        const auto before = items.size();
        items.erase(std::remove_if(items.begin(), items.end(), [&id](const LibrarySource& item) { return item.id == id; }), items.end());
        if (items.size() == before) return Reply(req, "404 Not Found", "{\"error\":\"没有这个书库\"}");
        if (!SaveLibrarySources(items)) return Reply(req, "500 Internal Server Error", "{\"error\":\"存储空间不足，没能保存\"}");
        return Reply(req, "200 OK", "{\"ok\":true}");
    }
    return Reply(req, "400 Bad Request", "{\"error\":\"未知操作\"}");
}

SetupStatus GetSetupStatus() {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto result = s_status;
    if (result.running && esp_timer_get_time() >= s_expires) { result.running = false; result.note = "配置已过期，请重新打开"; }
    return result;
}
static bool StartSetupServerLocked();
bool StartSetupServer() {
    StopSetupServer();
    { std::lock_guard<std::mutex> lock(s_mutex); s_status.starting = true; s_status.note = "正在连 Wi-Fi 并打开配置页，要十来秒…"; }
    const bool ok = StartSetupServerLocked();
    { std::lock_guard<std::mutex> lock(s_mutex); s_status.starting = false; }
    return ok;
}
static bool StartSetupServerLocked() {
    // 配置页只能在局域网里打开。Wi-Fi 空闲一会儿射频就会被省电关掉，
    // 所以先申请保持联网、把它叫起来；只有上网方式不是 Wi-Fi 时才让用户去换。
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    const bool wifi_selected = (dual ? dual->GetNetworkType() == NetworkType::WIFI : true) &&
                               WifiStation::GetInstance().IsUserEnabled();
    if (!wifi_selected) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.note = "现在用的是 4G。配置页要在同一个 Wi-Fi 里打开，请先把上网方式换成 Wi-Fi。";
        return false;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    s_hold = true;
    if (!WifiStation::GetInstance().IsConnected()) Board::GetInstance().EnsureNetworkReady(20000);
    wifi_ap_record_t ap{};
    esp_netif_ip_info_t ip{};
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr) {
        ESP_LOGW(TAG, "no Wi-Fi address after wake-up");
        StopSetupServer();
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.note = "Wi-Fi 没连上。请检查 Wi-Fi 后再试。";
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
    config.stack_size = 8192; config.max_uri_handlers = 5;
    config.max_open_sockets = 2; config.recv_wait_timeout = 5; config.send_wait_timeout = 5;
    const esp_err_t started = httpd_start(&s_server, &config);
    if (started != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(started));
        s_server = nullptr;
        StopSetupServer();
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.note = "配置页没能打开，请再试一次。";
        return false;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/setup", .method = HTTP_GET, .handler = Page},
        {.uri = "/api/assistants", .method = HTTP_GET, .handler = Profiles},
        {.uri = "/api/assistants", .method = HTTP_POST, .handler = Mutate},
        {.uri = "/api/libraries", .method = HTTP_GET, .handler = Libraries},
        {.uri = "/api/libraries", .method = HTTP_POST, .handler = LibrariesMutate},
    };
    for (const auto& route : routes) {
        const esp_err_t registered = httpd_register_uri_handler(s_server, &route);
        if (registered == ESP_OK) continue;
        ESP_LOGE(TAG, "route %s failed: %s", route.uri, esp_err_to_name(registered));
        StopSetupServer();
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.note = "配置页没能打开，请再试一次。";
        return false;
    }
    ESP_LOGI(TAG, "setup page up at %s", origin);
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
        ESP_LOGW(TAG, "close requested: running=%d starting=%d", s_status.running ? 1 : 0, s_status.starting ? 1 : 0);
        s_status.running = false; s_status.saving = false; s_access.clear();
        generation = ++s_status.generation;
    }
    Application::GetInstance().Schedule([generation] {
        { std::lock_guard<std::mutex> lock(s_mutex); if (generation != s_status.generation) return; }
        StopSetupServer();
    }, Application::TaskDomain::Network);
}
}
