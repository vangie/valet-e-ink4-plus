#include "pairing.h"
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <memory>
namespace valet {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::string Field(const cJSON* root, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
std::string Canonical(std::string url) {
    const auto host_end = url.find('/', 6);
    if (host_end == std::string::npos) url += '/';
    return url;
}
}
const char* ParsePairing(const std::string& payload, AssistantProfile& output) {
    if (payload.empty() || payload.size() > 6144 || payload.find('\0') != std::string::npos) return "配对回复过长或格式错误";
    Json root(cJSON_ParseWithOpts(payload.c_str(), nullptr, true), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) return "请粘贴助理返回的 JSON 配对信息";
    AssistantProfile profile;
    profile.id = Field(root.get(), "id");
    profile.name = Field(root.get(), "name");
    profile.connection.url = Field(root.get(), "url");
    auto code = Field(root.get(), "code");
    const auto backend = Field(root.get(), "backend");
    if (backend != "openclaw" && backend != "hermes") return "请选择 OpenClaw 或 Hermes";
    profile.connection.backend = backend == "hermes" ? custom_chat::Backend::Hermes : custom_chat::Backend::OpenClaw;
    if (code.rfind("oc-pair://", 0) == 0) code.erase(0, 10);
    code.erase(std::remove_if(code.begin(), code.end(), [](unsigned char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }), code.end());
    if (code.empty() || code.size() > 1399) return "配对码无效或不完整";
    profile.connection.code = code;
    std::replace(code.begin(), code.end(), '-', '+');
    std::replace(code.begin(), code.end(), '_', '/');
    while (code.size() % 4) code += '=';
    unsigned char bytes[1400]{};
    size_t count = 0;
    if (mbedtls_base64_decode(bytes, sizeof(bytes) - 1, &count,
            reinterpret_cast<const unsigned char*>(code.data()), code.size()) != 0 || count == 0) return "配对码无法解码";
    Json inner(cJSON_ParseWithLengthOpts(reinterpret_cast<char*>(bytes), count + 1, nullptr, true), cJSON_Delete);
    if (!inner) return "配对码内容不完整";
    const auto url = Field(inner.get(), "url");
    const auto token = Field(inner.get(), "bootstrapToken");
    if (token.empty() || token.size() >= 128 || url.size() >= 160) return "配对码缺少有效授权信息";
    if (profile.connection.url.empty()) profile.connection.url = url;
    if (Canonical(url) != Canonical(profile.connection.url)) return "网关地址与配对码不一致";
    if (url.rfind("wss://", 0) != 0) return "请提供证书有效的 wss:// 网关地址";
    // The bootstrap token is stored only within its original pairing code, never sent as Bearer auth.
    if (!ValidProfile(profile)) return "请检查助理名称、地址与配对信息";
    output = std::move(profile);
    return nullptr;
}
}
