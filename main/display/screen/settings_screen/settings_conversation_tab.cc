#include "settings_conversation_tab.h"

#include "api_endpoints.h"
#include "api_http.h"
#include "assets/lang_config.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "power_policy.h"
#include "screen_common.h"
#include "settings_common.h"
#include "chat_cloud.h"
#include "application.h"
#include "display.h"
#include "screen_keyboard.h"
#include "custom_chat_config.h"
#include "valet_setup/server.h"
#include "settings_screen.h"
#include "ui_qrcode.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>


static constexpr const char* TAG = "SettingsConv";
static constexpr int kHttpTimeoutMs = 15000;
static constexpr int kWorkerStack = 8 * 1024;

struct SettingsConversationOption {
    bool enabled;
};

static constexpr SettingsConversationOption kOptions[] = {
    {true},
    {false},
};
static constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

struct SettingsConversationUi {
    lv_obj_t* btns[kOptionCount] = {};
    lv_obj_t* current_lbl = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* cloud_btns[chat_cloud::kProviderCount] = {};
    lv_obj_t* cloud_status = nullptr;
    bool alive = false;
    bool known = false;
    bool enabled = true;
};

static SettingsConversationUi s_ui;
static std::atomic<bool> s_busy{false};
static std::atomic<bool> s_cloud_busy{false};
// Accessed on the LVGL thread; workers carry the generation captured at launch.
static uint32_t s_ui_generation = 0;
static const char* CloudText(const char* zh, const char* en);
static void RefreshCloudOptions();
static lv_obj_t* s_assistants = nullptr;
static void BuildAssistants(lv_obj_t* page);

static lv_timer_t* s_setup_poll = nullptr;
static std::atomic<bool> s_setup_starting{false};
static bool s_setup_visible = false;
static std::string s_remove_id;
static void RefreshAssistantList() {
    if (!s_ui.alive || !s_assistants) return;
    lv_obj_clean(s_assistants);
    BuildAssistants(s_assistants);
}
static void StopSetupClicked(lv_event_t*) {
    s_setup_visible = false;
    valet::CloseSetupSession();
    RefreshAssistantList();
}
static void StartSetupClicked(lv_event_t*) {
    if (s_setup_starting) return;
    s_setup_visible = true;
    s_setup_starting = true;
    const uint32_t generation = s_ui_generation;
    if (!Application::GetInstance().Schedule([generation]() {
        valet::StartSetupServer();
        auto* expected = new uint32_t(generation);
        if (!ScreenLvAsync([](void* context) {
            const auto generation = *static_cast<uint32_t*>(context);
            delete static_cast<uint32_t*>(context);
            s_setup_starting = false;
            if (s_ui.alive && generation == s_ui_generation && s_setup_visible) RefreshAssistantList();
            else valet::CloseSetupSession();
        }, expected)) { delete expected; s_setup_starting = false; valet::CloseSetupSession(); }
    }, Application::TaskDomain::Network)) s_setup_starting = false;
    RefreshAssistantList();
}
static void OpenWifi(lv_event_t*) { ScreenNavigateTo(SettingsScreen::CreateNetwork); }
static void AskRemove(lv_event_t* event) {
    const auto profiles = valet::LoadAssistantProfiles();
    const auto index = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (index >= profiles.items.size()) return;
    s_remove_id = profiles.items[index].id;
    RefreshAssistantList();
}
static void CancelRemove(lv_event_t*) { s_remove_id.clear(); RefreshAssistantList(); }
static void ApplyProfileChange(lv_event_t* event) {
    if (s_cloud_busy.exchange(true)) return;
    auto profiles = valet::LoadAssistantProfiles();
    const intptr_t action = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    const auto index = static_cast<size_t>(action >= 0 ? action : -action - 1);
    if (index >= profiles.items.size()) { s_cloud_busy.store(false); return; }
    const auto id = profiles.items[index].id;

    s_remove_id.clear();
    const uint32_t generation = s_ui_generation;
    RefreshCloudOptions();
    if (!Application::GetInstance().Schedule([id, action, generation]() {
        auto latest = valet::LoadAssistantProfiles();
        const bool changed = action >= 0 ? latest.Select(id) : latest.Remove(id);
        const bool ok = changed && valet::SaveAssistantProfiles(latest);
        s_cloud_busy.store(false);
        struct Result { bool ok; uint32_t generation; };
        auto* result = new Result{ok, generation};
        if (!ScreenLvAsync([](void* context) {
            auto* result = static_cast<Result*>(context);
            if (s_ui.alive && s_ui_generation == result->generation) {
                if (result->ok && s_assistants) {
                    lv_obj_clean(s_assistants);
                    BuildAssistants(s_assistants);
                }
                RefreshCloudOptions();
                lv_label_set_text(s_ui.cloud_status, result->ok
                    ? CloudText("已保存。重新进入对话后使用所选助理。", "Saved. Reopen the conversation to use this assistant.")
                    : CloudText("保存失败，请重试", "Could not save. Try again."));
            }
            delete result;
        }, result)) delete result;
    }, Application::TaskDomain::Storage)) {
        s_cloud_busy.store(false);
        RefreshCloudOptions();
    }
}
static void BuildAssistants(lv_obj_t* page) {
    if (s_setup_visible) {
        const auto setup = valet::GetSetupStatus();
        auto* title = lv_label_create(page);
        lv_label_set_text(title, CloudText("在手机或电脑上添加助理", "Add on a phone or computer"));
        auto* hint = lv_label_create(page);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_label_set_text(hint, s_setup_starting ? CloudText("正在打开本地配置页…", "Opening local setup…") : setup.note.c_str());
        if (setup.running) {
            auto* frame = UiCreateQrFrame(page, UiSx(170));
            auto* qr = lv_qrcode_create(frame);
            lv_qrcode_set_size(qr, UiSx(170));
            lv_qrcode_set_dark_color(qr, lv_color_black());
            lv_qrcode_set_light_color(qr, lv_color_white());
            lv_obj_center(qr);
            lv_qrcode_update(qr, setup.url.data(), setup.url.size());
            auto* address = lv_label_create(page);
            const auto hash = setup.url.find('#');
            const auto public_url = setup.url.substr(0, hash);
            lv_label_set_text(address, public_url.c_str());
            auto* code = lv_label_create(page);
            lv_obj_set_width(code, lv_pct(100));
            lv_label_set_long_mode(code, LV_LABEL_LONG_WRAP);
            lv_label_set_text_fmt(code, "%s %s", CloudText("手动打开后输入访问码：", "Access code for manual entry:"), setup.url.substr(hash + 1).c_str());
            auto* steps = lv_label_create(page);
            lv_obj_set_width(steps, lv_pct(100));
            lv_label_set_long_mode(steps, LV_LABEL_LONG_WRAP);
            lv_label_set_text(steps, CloudText("同一 Wi-Fi → 复制提示给助理 → 粘贴回复保存。二维码十分钟有效。", "Same Wi-Fi → send prompt → paste reply and save. QR expires in 10 minutes."));
        } else if (!s_setup_starting) {
            SettingsCreateSelectableOption(page, CloudText("连接 Wi-Fi", "Connect Wi-Fi"), OpenWifi, 0);
            SettingsCreateSelectableOption(page, CloudText("重新打开配置", "Open setup again"), StartSetupClicked, 0);
        }
        SettingsCreateSelectableOption(page, CloudText("关闭配置并返回", "Close setup and return"), StopSetupClicked, 0);
        return;
    }
    const auto profiles = valet::LoadAssistantProfiles();
    auto* title = lv_label_create(page);
    lv_label_set_text(title, CloudText("我的助理", "My assistants"));
    for (size_t i = 0; i < profiles.items.size(); ++i) {
        const auto& profile = profiles.items[i];
        auto* name = lv_label_create(page);
        const bool current = profile.id == profiles.active_id;
        lv_label_set_text_fmt(name, "%s%s · %s", current ? "● " : "", profile.name.c_str(),
            profile.connection.backend == custom_chat::Backend::Hermes ? "Hermes" : "OpenClaw");
        lv_obj_set_width(name, lv_pct(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        if (!current) SettingsCreateSelectableOption(page, CloudText("使用这个助理", "Use this assistant"), ApplyProfileChange, i);
        if (s_remove_id == profile.id) {
            SettingsCreateSelectableOption(page, CloudText("确认移除此设备保存的连接", "Confirm removal from this device"), ApplyProfileChange, -static_cast<intptr_t>(i) - 1);
            SettingsCreateSelectableOption(page, CloudText("取消", "Cancel"), CancelRemove, 0);
        } else SettingsCreateSelectableOption(page, CloudText("管理：移除此助理", "Remove assistant"), AskRemove, i);
    }
    if (profiles.items.size() < valet::kMaxAssistants) SettingsCreateSelectableOption(page, CloudText("添加助理", "Add assistant"), StartSetupClicked, 0);
    if (profiles.items.empty()) {
        auto* empty = lv_label_create(page);
        lv_label_set_text(empty, CloudText("还没有助理。点添加，用手机或电脑配置。", "No assistants yet. Add one using your phone or computer."));
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    }
    auto* note = lv_label_create(page);
    lv_label_set_text(note, CloudText("已保存的连接不代表在线。首次使用时会签名认证；网关可能要求批准新设备。", "A saved connection is not an online check. First use authenticates the device; approve its pending request on the gateway if needed."));
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

struct SettingsTtsResultMsg {
    uint32_t generation = 0;
    bool ok = false;
    bool enabled = false;
    char err[64] = {};
};

static const char* CloudText(const char* zh, const char* en) {
    return std::strcmp(Lang::CODE, "zh-CN") == 0 ? zh : en;
}

static void RefreshCloudOptions() {
    const auto saved = chat_cloud::SavedProvider();
    for (int i = 0; i < chat_cloud::kProviderCount; ++i) {
        if (s_ui.cloud_btns[i]) {
            SettingsStyleSelectable(s_ui.cloud_btns[i], lv_obj_get_child(s_ui.cloud_btns[i], 0),
                                    static_cast<int>(saved) == i);
            if (s_cloud_busy.load()) {
                lv_obj_add_state(s_ui.cloud_btns[i], LV_STATE_DISABLED);
            } else {
                lv_obj_clear_state(s_ui.cloud_btns[i], LV_STATE_DISABLED);
            }
        }
    }
    if (s_ui.cloud_status) {
        lv_label_set_text(s_ui.cloud_status, s_cloud_busy.load()
            ? CloudText("正在保存…", "Saving...")
            : saved != chat_cloud::ActiveProvider()
            ? CloudText("已保存，正在重启…", "Saved. Restarting...")
            : CloudText("切换云服务后自动重启", "Automatically restarts after switching cloud service."));
    }
}

static void OnCloudClicked(lv_event_t* e) {
    const int provider = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (provider == static_cast<int>(chat_cloud::SavedProvider()) || s_cloud_busy.exchange(true)) {
        return;
    }
    ++s_ui_generation;
    ESP_LOGI(TAG, "Cloud switch requested provider=%d (TTS busy=%d)", provider, s_busy.load());
    RefreshCloudOptions();
    // LVGL runs on a PSRAM stack. Even opening a writable NVS namespace can
    // write Flash and disable caches, so the entire Settings lifetime must
    // run on the application's internal-RAM task, including its commit.
    if (!Application::GetInstance().Schedule([provider]() {
        chat_cloud::SaveProvider(static_cast<chat_cloud::Provider>(provider));
        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            DisplayLockGuard lock(display);
            if (lock && s_ui.alive && s_ui.cloud_status) {
                lv_label_set_text(s_ui.cloud_status, CloudText("已保存，正在重启…", "Saved. Restarting..."));
            }
        }
        // SaveProvider commits before returning; release the display lock before teardown.
        Application::GetInstance().Reboot();
    }, Application::TaskDomain::Storage)) {
        s_cloud_busy.store(false);
        RefreshCloudOptions();
    }
}

static const char* OptionTitle(bool enabled) {
    return enabled ? Lang::Strings::SETTINGS_CONV_TTS_ON : Lang::Strings::SETTINGS_CONV_TTS_OFF;
}
static const char* CurrentText(bool enabled) {
    return enabled ? Lang::Strings::SETTINGS_CONV_CUR_ON : Lang::Strings::SETTINGS_CONV_CUR_OFF;
}

static void SetStatus(const char* text) {
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, text != nullptr ? text : "");
        if (text != nullptr && text[0] != '\0') {
            lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void RefreshOptions() {
    RefreshCloudOptions();
    for (int i = 0; i < kOptionCount; ++i) {
        if (s_ui.btns[i] == nullptr) {
            continue;
        }
        const bool selected = s_ui.known && (kOptions[i].enabled == s_ui.enabled);
        SettingsStyleSelectable(s_ui.btns[i], lv_obj_get_child(s_ui.btns[i], 0), selected);
        if (s_busy.load() || s_cloud_busy.load()) {
            lv_obj_clear_flag(s_ui.btns[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_state(s_ui.btns[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_flag(s_ui.btns[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_state(s_ui.btns[i], LV_STATE_DISABLED);
        }
    }
    if (s_ui.current_lbl != nullptr) {
        if (!s_ui.known) {
            lv_label_set_text(s_ui.current_lbl, Lang::Strings::SETTINGS_CONV_CUR_DASH);
        } else {
            lv_label_set_text(s_ui.current_lbl, CurrentText(s_ui.enabled));
        }
    }
}

static bool ParseTtsEnabled(const std::string& body, bool* enabled_out, std::string& err_out) {
    if (enabled_out == nullptr) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err_out = Lang::Strings::SETTINGS_CONV_PARSE_FAIL;
        return false;
    }
    const cJSON* code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        const cJSON* msg = cJSON_GetObjectItem(root, "msg");
        err_out = cJSON_IsString(msg) && msg->valuestring != nullptr ? msg->valuestring : Lang::Strings::SETTINGS_CONV_REQUEST_FAIL;
        cJSON_Delete(root);
        return false;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const cJSON* en = data != nullptr ? cJSON_GetObjectItem(data, "enabled") : nullptr;
    if (!cJSON_IsBool(en)) {
        err_out = Lang::Strings::SETTINGS_CONV_MISSING_ENABLED;
        cJSON_Delete(root);
        return false;
    }
    *enabled_out = cJSON_IsTrue(en);
    cJSON_Delete(root);
    return true;
}

static bool HttpJson(const char* method, const std::string& url, const char* body_in, std::string& body_out,
              std::string& err_out) {
    body_out.clear();
    if (url.empty()) {
        err_out = "api not configured";
        ESP_LOGI(TAG, "skip tts settings: cloud endpoints blank");
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::SETTINGS_CONV_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::SETTINGS_CONV_CONN_FAIL;
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    if (body_in != nullptr) {
        api::ApplyJsonHeaders(http);
        http->SetContent(std::string(body_in));
    } else {
        api::ApplyCommonHeaders(http);
    }
    // 与 curl 示例一致：URL 也带 Device-Id
    std::string full = url;
    if (full.find('?') == std::string::npos) {
        full += "?Device-Id=";
        full += api::DeviceId();
    }
    if (!http->Open(method, full)) {
        err_out = Lang::Strings::SETTINGS_CONV_REQUEST_FAIL;
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        err_out = body_out.empty() ? ("HTTP " + std::to_string(status)) : Lang::Strings::SETTINGS_CONV_REQUEST_FAIL;
        return false;
    }
    return true;
}

static void AsyncApplyResult(void* p) {
    auto* msg = static_cast<SettingsTtsResultMsg*>(p);
    s_busy.store(false);
    if (!s_ui.alive || s_cloud_busy.load() || msg->generation != s_ui_generation) {
        ESP_LOGI(TAG, "Discard stale TTS result after switch or page change");
        delete msg;
        if (s_ui.alive && !s_cloud_busy.load()) {
            // A rebuilt page can start its own request now that the old one
            // has drained; never leave its controls disabled indefinitely.
            SettingsConversationTab_OnActivated();
        }
        return;
    }
    if (!msg->ok) {
        SetStatus(msg->err[0] != '\0' ? msg->err : Lang::Strings::SETTINGS_CONV_REQUEST_FAIL);
        RefreshOptions();
        delete msg;
        return;
    }
    s_ui.known = true;
    s_ui.enabled = msg->enabled;
    SetStatus("");
    RefreshOptions();
    delete msg;
}

// 短时联网：等待 WiFi 完成后再拉取 HTTP 配置，释放前已处理资源收口。
static void FetchTask(void* arg) {
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        auto* msg = static_cast<SettingsTtsResultMsg*>(arg);
        std::string body;
        std::string err;
        if (!Board::GetInstance().EnsureNetworkReady()) {
            msg->ok = false;
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::SETTINGS_CONV_NET_NOT_READY);
        } else if (!HttpJson("GET", api::DeviceSettingsTtsUrl(), nullptr, body, err) ||
                   !ParseTtsEnabled(body, &msg->enabled, err)) {
            msg->ok = false;
            std::snprintf(msg->err, sizeof(msg->err), "%s", err.c_str());
        } else {
            msg->ok = true;
        }
        if (!ScreenLvAsync(AsyncApplyResult, msg)) {
            ESP_LOGW(TAG, "UI result queue rejected fetch result");
            delete msg;
            s_busy.store(false);
        }
    } // PowerNeedHold Release：须在 vTaskDelete 前析构
    vTaskDeleteWithCaps(nullptr);
}

static void PutTask(void* arg) {
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        auto* msg = static_cast<SettingsTtsResultMsg*>(arg);
        const bool enable = msg->enabled;
        std::string body;
        std::string err;
        if (!Board::GetInstance().EnsureNetworkReady()) {
            err = Lang::Strings::SETTINGS_CONV_NET_NOT_READY;
        } else {
            char req[32];
            std::snprintf(req, sizeof(req), "{\"enabled\":%s}", enable ? "true" : "false");
            if (HttpJson("PUT", api::DeviceSettingsTtsUrl(), req, body, err)) {
                msg->ok = ParseTtsEnabled(body, &msg->enabled, err);
                if (!msg->ok) {
                    // A code-only acknowledgement confirms the request, not the
                    // resulting preference. Read it back instead of guessing.
                    cJSON* root = cJSON_Parse(body.c_str());
                    const cJSON* code = root ? cJSON_GetObjectItem(root, "code") : nullptr;
                    const bool accepted = cJSON_IsNumber(code) && code->valueint == 0;
                    cJSON_Delete(root);
                    if (accepted) {
                        msg->ok = HttpJson("GET", api::DeviceSettingsTtsUrl(), nullptr, body, err) &&
                                  ParseTtsEnabled(body, &msg->enabled, err);
                    }
                }
            }
        }
        if (!msg->ok) {
            std::snprintf(msg->err, sizeof(msg->err), "%s", err.c_str());
        }
        if (!ScreenLvAsync(AsyncApplyResult, msg)) {
            ESP_LOGW(TAG, "UI result queue rejected put result");
            delete msg;
            s_busy.store(false);
        }
    }
    vTaskDeleteWithCaps(nullptr);
}

static bool StartWorker(TaskFunction_t fn, void* arg, const char* name) {
    if (s_busy.exchange(true)) {
        return false;
    }
    RefreshOptions();
    auto* msg = new SettingsTtsResultMsg();
    msg->generation = s_ui_generation;
    msg->enabled = arg != nullptr;
    if (xTaskCreateWithCaps(fn, name, kWorkerStack, msg, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(%s) failed", name);
        delete msg;
        s_busy.store(false);
        RefreshOptions();
        return false;
    }
    return true;
}

static void OnOptionClicked(lv_event_t* e) {
    if (s_busy.load() || s_cloud_busy.load()) {
        return;
    }
    const bool enable = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    if (s_ui.known && s_ui.enabled == enable) {
        return;
    }
    SetStatus(enable ? Lang::Strings::SETTINGS_CONV_ENABLING : Lang::Strings::SETTINGS_CONV_DISABLING);
    if (chat_cloud::UsesXiaozhiProtocol() || chat_cloud::IsCustom()) {
        if (s_busy.exchange(true)) { return; }
        RefreshOptions();
        const auto generation = s_ui_generation;
        if (!Application::GetInstance().Schedule([enable, generation]() {
            Application::GetInstance().SetLocalChatTtsEnabled(enable);
            auto* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                DisplayLockGuard lock(display);
                s_busy.store(false);
                if (!lock) return;
                if (s_ui.alive && !s_cloud_busy.load() && generation == s_ui_generation) {
                    s_ui.known = true;
                    s_ui.enabled = chat_cloud::LocalTtsEnabled();
                    SetStatus("");
                    RefreshOptions();
                }
            } else {
                s_busy.store(false);
            }
        })) {
            s_busy.store(false);
            SetStatus(Lang::Strings::SETTINGS_CONV_BUSY);
            RefreshOptions();
        }
        return;
    }
    if (!StartWorker(PutTask, enable ? reinterpret_cast<void*>(1) : nullptr, "tts_put")) {
        SetStatus(Lang::Strings::SETTINGS_CONV_BUSY);
    }
}


void SettingsConversationTab_Reset() {
    s_setup_visible = false;
    s_remove_id.clear();
    valet::CloseSetupSession();
    if (s_setup_poll) { lv_timer_delete(s_setup_poll); s_setup_poll = nullptr; }
    s_assistants = nullptr;
    // 仅拆 UI；busy 留给 AsyncApplyResult，避免离 Tab 时强清导致重入双任务
    ++s_ui_generation;
    s_ui = {};
}

void SettingsConversationTab_OnActivated() {
    if (!s_ui.alive) {
        return;
    }
    if (s_busy.load()) {
        return;
    }
    RefreshCloudOptions();
    if (chat_cloud::UsesXiaozhiProtocol() || chat_cloud::IsCustom()) {
        s_ui.known = true;
        s_ui.enabled = chat_cloud::LocalTtsEnabled();
        SetStatus("");
        RefreshOptions();
        return;
    }
    SetStatus(Lang::Strings::SETTINGS_CONV_CONNECTING);
    if (!StartWorker(FetchTask, nullptr, "tts_get")) {
        SetStatus(Lang::Strings::SETTINGS_CONV_BUSY);
    }
}

void SettingsConversationTab_Build(lv_obj_t* page) {
    ++s_ui_generation;
    s_ui = {};
    s_ui.alive = true;

    auto* cloud_title = lv_label_create(page);
    lv_label_set_text(cloud_title, CloudText("云服务切换", "Cloud service"));
    lv_obj_set_style_text_font(cloud_title, fontpack_lv_font_ui(), 0);
    static const char* kCloudNames[][2] = {
        {"百问AI", "Baiwen AI"}, {"小智AI", "Xiaozhi AI"},
        {"网易AI", "NetEase AI"}, {"自建", "Custom"},
    };
    for (int i = 0; i < chat_cloud::kProviderCount; ++i) {
        const auto provider = static_cast<chat_cloud::Provider>(i);
        if (provider != chat_cloud::kDefaultProvider && provider != chat_cloud::Provider::Custom) continue;
        s_ui.cloud_btns[i] = SettingsCreateSelectableOption(page,
            CloudText(kCloudNames[i][0], kCloudNames[i][1]), OnCloudClicked, i);
    }
    s_ui.cloud_status = lv_label_create(page);
    lv_obj_set_width(s_ui.cloud_status, lv_pct(100));
    lv_label_set_long_mode(s_ui.cloud_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.cloud_status, fontpack_lv_font_ui(), 0);
    RefreshCloudOptions();
    if (chat_cloud::IsCustom()) {
        s_assistants = lv_obj_create(page);
        lv_obj_remove_style_all(s_assistants);
        lv_obj_set_width(s_assistants, lv_pct(100));
        lv_obj_set_height(s_assistants, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_assistants, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_assistants, UiSy(12), 0);
        lv_obj_clear_flag(s_assistants, LV_OBJ_FLAG_SCROLLABLE);
        BuildAssistants(s_assistants);
        s_setup_poll = lv_timer_create([](lv_timer_t*) {
            if (!s_ui.alive || !s_setup_visible || s_setup_starting) return;
            static std::string last;
            const auto setup = valet::GetSetupStatus();
            const auto key = setup.note + (setup.running ? "1" : "0");
            if (key != last) { last = key; RefreshAssistantList(); }
            if (!setup.running) valet::CloseSetupSession();
        }, 1000, nullptr);
    }
    if (chat_cloud::UsesXiaozhiProtocol() || chat_cloud::IsCustom()) {
        s_ui.known = true;
        s_ui.enabled = chat_cloud::LocalTtsEnabled();
    }

    lv_obj_t* split = lv_obj_create(page);
    lv_obj_remove_style_all(split);
    lv_obj_set_size(split, lv_pct(100), kSettingsSplitLineW);
    lv_obj_set_style_bg_color(split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(split, LV_OPA_COVER, 0);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_CONV_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    if (!chat_cloud::UsesXiaozhiProtocol() && !chat_cloud::IsCustom()) {
        lv_obj_t* hint = lv_label_create(page);
        lv_label_set_text(hint, Lang::Strings::SETTINGS_CONV_HINT);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
        lv_obj_set_style_text_color(hint, lv_color_black(), 0);
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    }

    s_ui.status_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_ui.status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kOptionCount; ++i) {
        s_ui.btns[i] = SettingsCreateSelectableOption(
            page, OptionTitle(kOptions[i].enabled), OnOptionClicked,
            static_cast<intptr_t>(kOptions[i].enabled ? 1 : 0));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, Lang::Strings::SETTINGS_CONV_CUR_DASH);
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    RefreshOptions();
}
