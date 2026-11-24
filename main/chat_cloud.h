#pragma once

#include "settings.h"
#include "api_endpoints.h"
#include "sdkconfig.h"
#include "custom_chat_config.h"
#include "assistant_profiles.h"
#include <atomic>

namespace chat_cloud {
enum class Provider { Baiwen = 0, Xiaozhi = 1, Yunxin = 2, Custom = 3 };
inline constexpr int kProviderCount = 4;
#if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
inline constexpr Provider kDefaultProvider = Provider::Yunxin;
#else
inline constexpr Provider kDefaultProvider = Provider::Baiwen;
#endif

// 书里提问（正文选字、问这一页、书页下方的悬窗）找谁：跟「谁来回答」走，或单独选官方云 / 自己的助理。
enum class BookAsk { Follow = 0, Official = 1, Custom = 2 };
namespace detail {
inline std::atomic<BookAsk> book_ask{BookAsk::Follow};
inline std::atomic<Provider> saved{kDefaultProvider};
inline std::atomic<Provider> active{kDefaultProvider};   // 正在用的；换云服务不重启，所以可能在运行中变
inline std::atomic<bool> local_tts{true};
}  // namespace detail

// Call once on the internal-RAM startup task, before UI/network workers exist.
// NVS reads also disable Flash caches; never lazily load from a UI accessor.
inline void InitializeFromNvs() {
    valet::InitializeAssistantProfiles();
    Settings settings("chat_cloud");
    const int stored = settings.GetInt("provider", static_cast<int>(kDefaultProvider));
    const auto provider = stored >= 0 && stored < kProviderCount
        ? static_cast<Provider>(stored) : kDefaultProvider;
    detail::active.store(provider, std::memory_order_release);
    detail::saved.store(provider, std::memory_order_release);
    detail::local_tts.store(settings.GetBool(provider == Provider::Custom ? "agent_tts" : provider == Provider::Yunxin ? "yx_tts" : "xz_tts", true), std::memory_order_release);
    const int book = settings.GetInt("book_ask", 0);
    detail::book_ask.store(book >= 0 && book <= 2 ? static_cast<BookAsk>(book) : BookAsk::Follow, std::memory_order_release);
}

// 换云服务不重启：把 provider 设为正在用的（不改保存的选择），并读回它自己的「回答出声」开关。
// 只在应用的内部 RAM 任务上调（读 NVS），且旧的对话通道已经拆掉之后。
inline void ActivateProvider(Provider provider) {
    Settings settings("chat_cloud");
    detail::local_tts.store(settings.GetBool(provider == Provider::Custom ? "agent_tts" : provider == Provider::Yunxin ? "yx_tts" : "xz_tts", true), std::memory_order_release);
    detail::active.store(provider, std::memory_order_release);
}

inline Provider SavedProvider() { return detail::saved.load(std::memory_order_acquire); }
inline Provider ActiveProvider() { return detail::active.load(std::memory_order_acquire); }

// Application's internal-RAM task only. Publish the cached selection after
// Settings has committed it. Application::SwitchChatProviderOnTask makes it the active one.
inline void SaveProvider(Provider provider) {
    if (static_cast<int>(provider) < 0 || static_cast<int>(provider) >= kProviderCount) return;
    {
        Settings settings("chat_cloud", true);
        settings.SetInt("provider", static_cast<int>(provider));
    }
    detail::saved.store(provider, std::memory_order_release);
}

inline void ActivateSavedProvider() { ActivateProvider(SavedProvider()); }
inline BookAsk BookAskChoice() { return detail::book_ask.load(std::memory_order_acquire); }
// 书里提问实际用哪一个
inline Provider BookProvider() {
    switch (BookAskChoice()) {
        case BookAsk::Official: return kDefaultProvider;
        case BookAsk::Custom: return Provider::Custom;
        default: return SavedProvider();
    }
}
// Application's internal-RAM task only (writes NVS).
inline void SaveBookAsk(BookAsk choice) {
    {
        Settings settings("chat_cloud", true);
        settings.SetInt("book_ask", static_cast<int>(choice));
    }
    detail::book_ask.store(choice, std::memory_order_release);
}
inline bool IsXiaozhi() { return ActiveProvider() == Provider::Xiaozhi; }
inline bool IsYunxin() { return ActiveProvider() == Provider::Yunxin; }
inline bool IsCustom() { return ActiveProvider() == Provider::Custom; }
inline bool UsesXiaozhiProtocol() { return IsXiaozhi() || IsYunxin(); }
inline custom_chat::Config CustomConfig() {
    const auto profiles = valet::LoadAssistantProfiles();
    if (const auto* active = profiles.Active()) return active->connection;
    return {};
}
// Calls belong to an assistant of your own: no call entry anywhere until
// 「自建」 is selected and an assistant is connected.
inline bool HasCallEntry() {
    return IsCustom() && valet::LoadAssistantProfiles().Active() != nullptr;
}
// 书里（阅读卡片、悬窗）有没有通话：书里提问用的是自己的助理，并且有一个助理
inline bool BookHasCallEntry() {
    return BookProvider() == Provider::Custom && valet::LoadAssistantProfiles().Active() != nullptr;
}
inline std::string CustomUrl() {
    return CustomConfig().url;
}
inline std::string CustomCode() {
    return CustomConfig().code;
}
inline bool SaveCustomConfig(const custom_chat::Config& config) {
    if (custom_chat::Validate(config) != custom_chat::ConfigError::None) return false;
    auto profiles = valet::LoadAssistantProfiles();
    valet::AssistantProfile profile;
    if (const auto* active = profiles.Active()) profile = *active;
    else { profile.id = "legacy"; profile.name = "Imported assistant"; }
    profile.connection = config;
    return profiles.Upsert(profile) && valet::SaveAssistantProfiles(profiles);
}
inline bool LocalTtsEnabled() { return detail::local_tts.load(std::memory_order_acquire); }
inline bool SpeechPlaybackEnabled() { return IsCustom() ? true : !UsesXiaozhiProtocol() || LocalTtsEnabled(); }

// Internal-RAM task only, just like SaveProvider. Baiwen's preference stays remote.
inline void SaveLocalTtsEnabled(bool enabled) {
    {
        Settings settings("chat_cloud", true);
        settings.SetBool(IsCustom() ? "agent_tts" : IsYunxin() ? "yx_tts" : "xz_tts", enabled);
    }
    detail::local_tts.store(enabled, std::memory_order_release);
}

inline bool CustomConfigured() {
    const auto profiles = valet::LoadAssistantProfiles();
    const auto* active = profiles.Active();
    return active && valet::ValidProfile(*active);
}
inline bool Available() { return IsCustom() ? CustomConfigured() : UsesXiaozhiProtocol() || api::HasHost(); }
// 书里提问用的那一个能不能用（自建要有一个配好的助理；百问要有服务地址）
inline bool BookProviderReady() {
    const auto provider = BookProvider();
    if (provider == Provider::Custom) return CustomConfigured();
    return provider != Provider::Baiwen || api::HasHost();
}
inline const char* WebsocketNamespace() { return IsYunxin() ? "yx_websocket" : IsXiaozhi() ? "xz_websocket" : "websocket"; }
inline const char* MqttNamespace() { return IsYunxin() ? "yx_mqtt" : IsXiaozhi() ? "xz_mqtt" : "mqtt"; }
inline std::string YunxinWebsocketUrl(const std::string& mac) {
    return std::string(yunxin_websocket_url) + "?device_id=" + mac +
           "&app_key=" + yunxin_app_key;
}
inline const char* YunxinToken() { return yunxin_token; }
inline const char* ChatOtaUrl() {
    return IsYunxin()
        ? yunxin_ota_url
        : xiaozhi_ota_url;
}

// Append the activation path before the query, preserving Yunxin's appkey.
inline std::string ActivationUrl(const std::string& ota_url) {
    if (ota_url.empty()) { return {}; }
    const auto query_pos = ota_url.find('?');
    std::string path = ota_url.substr(0, query_pos);
    const std::string query = query_pos == std::string::npos ? "" : ota_url.substr(query_pos);
    if (path.empty()) return {};
    if (path.back() != '/') { path += '/'; }
    return path + "activate" + query;
}
}  // namespace chat_cloud
