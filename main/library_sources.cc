#include "library_sources.h"

#include "settings.h"

#include <cJSON.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace valet {
namespace {
constexpr const char* kNamespace = "valet_lib";
constexpr const char* kKey = "libraries_v1";
constexpr size_t kMaxBlobBytes = 4096;

std::mutex s_mutex;
std::vector<LibrarySource> s_items;
bool s_loaded = false;

std::string Encode(const std::vector<LibrarySource>& items) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return {};
    cJSON* list = cJSON_AddArrayToObject(root, "items");
    if (list == nullptr) {
        cJSON_Delete(root);
        return {};
    }
    for (const auto& item : items) {
        cJSON* entry = cJSON_CreateObject();
        if (entry == nullptr) {
            cJSON_Delete(root);
            return {};
        }
        cJSON_AddItemToArray(list, entry);
        cJSON_AddStringToObject(entry, "id", item.id.c_str());
        cJSON_AddStringToObject(entry, "name", item.name.c_str());
        cJSON_AddStringToObject(entry, "url", item.url.c_str());
        if (!item.member.empty()) cJSON_AddStringToObject(entry, "member", item.member.c_str());
        if (!item.user.empty()) cJSON_AddStringToObject(entry, "user", item.user.c_str());
        if (!item.password.empty()) cJSON_AddStringToObject(entry, "password", item.password.c_str());
        cJSON_AddBoolToObject(entry, "rich", item.rich);
    }
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) return {};
    std::string result(text);
    cJSON_free(text);
    return result;
}

std::vector<LibrarySource> Decode(const std::string& json) {
    std::vector<LibrarySource> items;
    cJSON* root = cJSON_ParseWithOpts(json.c_str(), nullptr, true);
    if (root == nullptr) return items;
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (cJSON_IsArray(list)) {
        for (const cJSON* entry = list->child; entry != nullptr; entry = entry->next) {
            LibrarySource item;
            const auto* id = cJSON_GetObjectItemCaseSensitive(entry, "id");
            const auto* name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            const auto* url = cJSON_GetObjectItemCaseSensitive(entry, "url");
            const auto* member = cJSON_GetObjectItemCaseSensitive(entry, "member");
            const auto* user = cJSON_GetObjectItemCaseSensitive(entry, "user");
            const auto* password = cJSON_GetObjectItemCaseSensitive(entry, "password");
            const auto* rich = cJSON_GetObjectItemCaseSensitive(entry, "rich");
            if (cJSON_IsString(id) && id->valuestring) item.id = id->valuestring;
            if (cJSON_IsString(name) && name->valuestring) item.name = name->valuestring;
            if (cJSON_IsString(url) && url->valuestring) item.url = url->valuestring;
            if (cJSON_IsString(member) && member->valuestring) item.member = member->valuestring;
            if (cJSON_IsString(user) && user->valuestring) item.user = user->valuestring;
            if (cJSON_IsString(password) && password->valuestring) item.password = password->valuestring;
            if (cJSON_IsBool(rich)) item.rich = cJSON_IsTrue(rich);
            if (item.id.empty() || item.url.empty()) continue;
            if (item.name.empty()) item.name = LibraryNameFromUrl(item.url);
            if (items.size() >= kMaxLibraries) break;
            items.push_back(std::move(item));
        }
    }
    cJSON_Delete(root);
    return items;
}

void EnsureLoaded() {
    if (s_loaded) return;
    Settings settings(kNamespace);
    const auto json = settings.GetBlob(kKey, kMaxBlobBytes);
    s_items = json.empty() ? std::vector<LibrarySource>{} : Decode(json);
    s_loaded = true;
}

std::string UrlOrigin(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t slash = url.find('/', scheme + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}
} // namespace

std::vector<LibrarySource> LoadLibrarySources() {
    std::lock_guard<std::mutex> lock(s_mutex);
    EnsureLoaded();
    return s_items;
}

bool SaveLibrarySources(const std::vector<LibrarySource>& items) {
    const auto json = Encode(items);
    if (json.empty()) return false;
    Settings settings(kNamespace, true);
    if (!settings.SetBlob(kKey, json) || !settings.Commit()) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    s_items = items;
    s_loaded = true;
    return true;
}

bool LibraryUrlValid(const std::string& url) {
    if (url.size() < 10 || url.size() > kMaxLibraryUrl) return false;
    const bool http = url.compare(0, 7, "http://") == 0;
    const bool https = url.compare(0, 8, "https://") == 0;
    if (!http && !https) return false;
    const size_t host_start = https ? 8 : 7;
    if (host_start >= url.size() || url[host_start] == '/') return false;
    for (size_t i = 0; i < url.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (c <= 0x20 || c == 0x7f) return false;   // 空格和控制字符一律不收
    }
    return url.find('.', host_start) != std::string::npos || url.find("localhost", host_start) != std::string::npos;
}

std::string LibraryNameFromUrl(const std::string& url) {
    const size_t start = url.compare(0, 8, "https://") == 0 ? 8 : (url.compare(0, 7, "http://") == 0 ? 7 : 0);
    if (start == 0 || start >= url.size()) return {};
    size_t end = url.find_first_of(":/?#", start);
    if (end == std::string::npos) end = url.size();
    std::string host = url.substr(start, end - start);
    if (host.empty()) return {};
    return host;
}

bool LibraryRichUrl(const std::string& url) {
    // 带成员短码的同步订阅：/opds/<短码>，或者 /api/u/<短码>/
    return url.find("/api/u/") != std::string::npos ||
           (url.find("/opds/") != std::string::npos && url.find("/opds/", url.find("/opds/") + 6) == std::string::npos);
}

std::string LibraryAuthHeaderForUrl(const std::string& url) {
    const std::string origin = UrlOrigin(url);
    if (origin.empty()) return {};
    std::string user, password;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        EnsureLoaded();
        for (const auto& item : s_items) {
            if (item.password.empty() && item.user.empty()) continue;
            if (url.rfind(item.url, 0) == 0 || UrlOrigin(item.url) == origin) {
                user = item.user;
                password = item.password;
                break;
            }
        }
    }
    if (user.empty() && password.empty()) return {};
    const std::string raw = user + ":" + password;
    unsigned char encoded[256] = {};
    size_t out_len = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &out_len,
                              reinterpret_cast<const unsigned char*>(raw.data()), raw.size()) != 0) {
        return {};
    }
    encoded[out_len] = '\0';
    return std::string("Basic ") + reinterpret_cast<const char*>(encoded);
}

std::string NewLibraryId() {
    uint32_t random[2];
    esp_fill_random(random, sizeof(random));
    char id[32];
    std::snprintf(id, sizeof(id), "lib-%08lx%08lx", static_cast<unsigned long>(random[0]),
                  static_cast<unsigned long>(random[1]));
    return id;
}
} // namespace valet
