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
    // Valet layout
    lv_obj_t* provider_seg = nullptr;
    lv_obj_t* speech_seg = nullptr;
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
static bool s_setup_rename = false;   // 配置页是为了改名打开的（设备键盘打不了中文，名字在手机或电脑上改）
static std::string s_remove_id;
static void RefreshAssistantList() {
    if (!s_ui.alive || !s_assistants) return;
    lv_obj_clean(s_assistants);
    BuildAssistants(s_assistants);
}
// Entering or leaving "add assistant" changes the whole page, not just the list.
static void RebuildPage() {
    if (!s_ui.alive || !s_assistants) return;
    auto* page = lv_obj_get_parent(s_assistants);
    if (s_setup_poll) { lv_timer_delete(s_setup_poll); s_setup_poll = nullptr; }
    s_assistants = nullptr;
    lv_obj_clean(page);
    SettingsConversationTab_Build(page);
    SettingsConversationTab_OnActivated();
    lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
}
static void StopSetupClicked(lv_event_t*) {
    s_setup_visible = false;
    valet::CloseSetupSession();
    // The button being tapped is inside the page; rebuild after this event returns.
    lv_async_call([](void*) { RebuildPage(); }, nullptr);
}
static void StartSetup(bool rename);
static void StartSetupClicked(lv_event_t*) { StartSetup(false); }
static void StartRenameClicked(lv_event_t*) { StartSetup(true); }
static void StartSetup(bool rename) {
    if (s_setup_starting) return;
    s_setup_rename = rename;
    s_setup_visible = true;
    s_setup_starting = true;
    // 进入「添加助理」会整页重建，页面代数随之变化，所以这里不能用代数判断页面还在不在：
    // 只看页面是否还开着、是否仍在添加流程里（离开页面时 Reset 会清掉 s_setup_visible）。
    if (!Application::GetInstance().Schedule([]() {
        valet::StartSetupServer();
        if (!ScreenLvAsync([](void*) {
            s_setup_starting = false;
            if (s_ui.alive && s_setup_visible) RefreshAssistantList();
            else valet::CloseSetupSession();
        }, nullptr)) { s_setup_starting = false; valet::CloseSetupSession(); }
    }, Application::TaskDomain::Network)) s_setup_starting = false;
    lv_async_call([](void*) { RebuildPage(); }, nullptr);
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
        lv_label_set_text(title, s_setup_rename ? CloudText("在手机或电脑上改名", "Rename on a phone or computer")
                                                : CloudText("在手机或电脑上添加助理", "Add on a phone or computer"));
        auto* hint = lv_label_create(page);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        const bool opening = s_setup_starting || setup.starting;
        lv_label_set_text(hint, opening ? CloudText("正在连 Wi-Fi 并打开配置页，要十来秒…", "Connecting Wi-Fi and opening setup…") : setup.note.c_str());
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
            lv_label_set_text(steps, s_setup_rename
                ? CloudText("同一 Wi-Fi 扫码打开 → 在「已保存的助理」里点「改名」。二维码十分钟有效。", "Same Wi-Fi, scan → in Saved assistants choose Rename. QR expires in 10 minutes.")
                : CloudText("同一 Wi-Fi → 复制提示给助理 → 粘贴回复保存。二维码十分钟有效。", "Same Wi-Fi → send prompt → paste reply and save. QR expires in 10 minutes."));
        } else if (!opening) {
            SettingsCreateSelectableOption(page, CloudText("去网络设置", "Network settings"), OpenWifi, 0);
            SettingsCreateSelectableOption(page, CloudText("再试一次", "Try again"), StartSetupClicked, 0);
        }
        SettingsCreateSelectableOption(page, CloudText("返回", "Back"), StopSetupClicked, 0);
        return;
    }
    const auto profiles = valet::LoadAssistantProfiles();
    const auto* small = fontpack_lv_font_get(25, 2);
    if (!small) small = fontpack_lv_font_ui();
    const auto* strong = fontpack_lv_font_get(30, 4);
    if (!strong) strong = fontpack_lv_font_ui();
    // Heading: how many, and the way to add one.
    auto* head = lv_obj_create(page);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 78);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    auto* title = lv_label_create(head);
    lv_label_set_text_fmt(title, "%s · %u", CloudText("我的助理", "My assistants"), static_cast<unsigned>(profiles.items.size()));
    lv_obj_set_style_text_font(title, strong, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    if (profiles.items.size() < valet::kMaxAssistants) {
        auto* add = SettingsValetButton(head, CloudText("+ 添加助理", "+ Add"), StartSetupClicked);
        lv_obj_set_width(add, 210);
        lv_obj_align(add, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    auto* tip = lv_label_create(page);
    lv_obj_set_width(tip, lv_pct(100));
    lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(tip, small, 0);
    lv_obj_set_style_text_line_space(tip, 6, 0);
    lv_label_set_text(tip, profiles.items.empty()
        ? CloudText("还没有助理。点「添加助理」，用手机或电脑连。", "No assistants yet. Add one using your phone or computer.")
        : CloudText("点「使用」切换。提问和通话都会用选中的助理，各自的对话记录分开保存。", "Choose Use to switch. Ask and Call both use the selected assistant; histories stay separate."));
    for (size_t i = 0; i < profiles.items.size(); ++i) {
        const auto& profile = profiles.items[i];
        const bool current = profile.id == profiles.active_id;
        const bool removing = s_remove_id == profile.id;
        auto* card = lv_obj_create(page);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), 262);
        lv_obj_set_style_border_width(card, 3, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
        if (current) {
            // The one in use carries a thick bar on its left edge.
            auto* bar = lv_obj_create(card);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 9, lv_pct(100));
            lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
        const int left = current ? 36 : 27;
        auto* name = lv_label_create(card);
        lv_label_set_text(name, profile.name.c_str());
        lv_obj_set_style_text_font(name, strong, 0);
        lv_obj_set_width(name, 540);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, left, 30);
        auto* state = lv_label_create(card);
        const char* backend = profile.connection.backend == custom_chat::Backend::Hermes ? "Hermes" : "OpenClaw";
        // Capabilities are recorded by a real handshake; say so when the call was not verified.
        // 有长期凭据才算连上过；没有就是还在等批准，或配对码已经过期。
        const bool paired = !profile.device_token.empty();
        lv_label_set_text_fmt(state, "%s · %s%s", backend,
            current ? CloudText("当前使用", "In use") : CloudText("已保存", "Saved"),
            !paired ? CloudText(" · 还没连上", " · not connected yet")
                    : profile.live ? "" : CloudText(" · 通话未验证", " · call not verified"));
        lv_obj_set_style_text_font(state, small, 0);
        lv_obj_set_pos(state, left, 86);
        const int inner = 617 - 6 - left - 27, half = (inner - 16) / 2, third = (inner - 2 * 16) / 3, by = 152;
        if (removing) {
            auto* yes = SettingsValetButton(card, CloudText("确认移除", "Remove"), ApplyProfileChange, -static_cast<intptr_t>(i) - 1, true);
            lv_obj_set_width(yes, half);
            lv_obj_set_pos(yes, left, by);
            auto* no = SettingsValetButton(card, CloudText("取消", "Cancel"), CancelRemove);
            lv_obj_set_width(no, half);
            lv_obj_set_pos(no, left + half + 16, by);
            continue;
        }
        // 一排三格：使用（在用的写「正在使用」）/ 改名 / 移除
        if (current) {
            auto* using_now = lv_label_create(card);
            lv_label_set_text(using_now, CloudText("正在使用", "In use"));
            lv_obj_set_style_text_font(using_now, small, 0);
            lv_obj_set_pos(using_now, left, by + 24);
        } else {
            auto* use = SettingsValetButton(card, CloudText("使用", "Use"), ApplyProfileChange, static_cast<intptr_t>(i));
            lv_obj_set_width(use, third);
            lv_obj_set_pos(use, left, by);
        }
        auto* rename = SettingsValetButton(card, CloudText("改名", "Rename"), StartRenameClicked);
        lv_obj_set_width(rename, third);
        lv_obj_set_pos(rename, left + third + 16, by);
        auto* remove = SettingsValetButton(card, CloudText("移除", "Remove"), AskRemove, static_cast<intptr_t>(i));
        lv_obj_set_width(remove, third);
        lv_obj_set_pos(remove, left + 2 * (third + 16), by);
    }
    if (!profiles.items.empty()) {
        auto* note = lv_label_create(page);
        lv_label_set_text(note, CloudText("添加时设备会马上去连助理；助理那边要求批准的话，请在十分钟内批准。", "The device connects as soon as you add an assistant. If it asks for approval, approve within ten minutes."));
        lv_obj_set_width(note, lv_pct(100));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(note, small, 0);
        lv_obj_set_style_text_line_space(note, 6, 0);
    }
}
struct SettingsTtsResultMsg {
    uint32_t generation = 0;
    bool ok = false;
    bool enabled = false;
    char err[64] = {};
};

static lv_obj_t* s_page = nullptr;   // 换云服务后整页按新的云服务重画

static const char* CloudText(const char* zh, const char* en) {
    return std::strcmp(Lang::CODE, "zh-CN") == 0 ? zh : en;
}

static void RefreshCloudOptions() {
    const auto saved = chat_cloud::SavedProvider();
    if (s_ui.provider_seg) SettingsValetSelect(s_ui.provider_seg, saved == chat_cloud::Provider::Custom ? 1 : 0);
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
            ? CloudText("正在切换…", "Switching...")
            : CloudText("换一个马上生效，不用重启。正在进行的对话会结束。", "Takes effect at once, no restart. A conversation in progress ends."));
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
    // 保存、拆旧通道、换新云服务都在应用的 Voice 任务上做（内部 RAM 栈，写 NVS 安全；与对话会话串行）
    if (!Application::GetInstance().Schedule([provider]() {
        chat_cloud::SaveProvider(static_cast<chat_cloud::Provider>(provider));
        Application::GetInstance().SwitchChatProviderOnTask();
        ScreenLvAsync([](void*) {
            s_cloud_busy.store(false);
            if (s_ui.alive && s_page && lv_obj_is_valid(s_page)) {
                // 官方和自建这一页的内容不同：整页重画
                lv_obj_clean(s_page);
                SettingsConversationTab_Build(s_page);
                SettingsConversationTab_OnActivated();
            }
            ScreenShowToast(CloudText("已切换，不用重启", "Switched, no restart"));
        });
    }, Application::TaskDomain::Voice)) {
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
    if (s_ui.speech_seg) SettingsValetSelect(s_ui.speech_seg, !s_ui.known ? -1 : s_ui.enabled ? 0 : 1);
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

static std::string ProviderName(chat_cloud::Provider provider) {
    switch (provider) {
        case chat_cloud::Provider::Custom: return CloudText("自建", "Custom");
        case chat_cloud::Provider::Xiaozhi: return CloudText("小智", "Xiaozhi");
        case chat_cloud::Provider::Yunxin: return CloudText("网易", "NetEase");
        default: return CloudText("百问", "Baiwen");
    }
}
static void OnValetSpeech(lv_event_t* e) {
    // Reuse the option handler: it takes 1 for on, 0 for off in the same user data.
    OnOptionClicked(e);
}
void SettingsConversationTab_Build(lv_obj_t* page) {
    ++s_ui_generation;
    s_ui = {};
    s_ui.alive = true;
    s_page = page;
    lv_obj_set_style_pad_row(page, 0, 0);
    // 显示保存的选择：在书里提问时正在用的可能临时是另一个（见 阅读与字库 → 书里提问）
    const bool custom = chat_cloud::SavedProvider() == chat_cloud::Provider::Custom;
    // While an assistant is being added the page does that one thing.
    const bool adding = custom && s_setup_visible;

    // Who is answering right now.
    if (!adding) {
        const auto profiles = valet::LoadAssistantProfiles();
        const auto* active = profiles.Active();
        std::string title = ProviderName(chat_cloud::SavedProvider()), detail;
        bool ok = true;
        if (!custom) detail = CloudText("官方订阅，正在用", "Official plan, in use");
        else if (active) {
            title = active->name + " · " + (active->connection.backend == custom_chat::Backend::Hermes ? "Hermes" : "OpenClaw");
            ok = !active->device_token.empty();
            detail = ok ? CloudText("已连接过，正在用", "Connected, in use")
                        : CloudText("还没连上。请在助理那边批准这台设备，或重新添加", "Not connected yet. Approve this device, or add it again");
        } else { detail = CloudText("还没有助理", "No assistant yet"); ok = false; }
        SettingsValetCard(page, title.c_str(), detail.c_str(), ok);
        SettingsValetGap(page, 30);
        SettingsValetLabel(page, CloudText("谁来回答", "Who answers"));
        const std::string official = ProviderName(chat_cloud::kDefaultProvider);
        const char* const names[] = {official.c_str(), CloudText("自建", "Custom")};
        static const intptr_t kValues[] = {static_cast<intptr_t>(chat_cloud::kDefaultProvider),
                                           static_cast<intptr_t>(chat_cloud::Provider::Custom)};
        s_ui.provider_seg = SettingsValetSegments(page, names, 2, OnCloudClicked, kValues);
        s_ui.cloud_status = SettingsValetNote(page, "");
        if (!custom) {
            const std::string plan = official + CloudText("用的是随机送的订阅。只有按住说话，没有通话。",
                                                         " uses the bundled plan: hold to talk, no calls.");
            // Its own paragraph: the line above is about switching, this one about the current service.
            lv_obj_set_style_margin_top(SettingsValetNote(page, plan.c_str()), 14, 0);
        }
        // 书里提问单独设了：说一声，免得在书里发现不是同一个助理；点进去改
        if (chat_cloud::BookAskChoice() != chat_cloud::BookAsk::Follow) {
            const std::string title = std::string(CloudText("书里提问另外设了：", "In books you use: ")) +
                ProviderName(chat_cloud::BookProvider());
            SettingsValetGap(page, 14);
            SettingsValetRow(page, title.c_str(), CloudText("选字和问这一页走它 · 去阅读与字库改", "Selection and page questions · change in Reading"),
                             [](lv_event_t*) { lv_async_call([](void*) { SettingsScreen_OpenReading(); }, nullptr); }, 0, true);
        }
        SettingsValetGap(page, 30);
    } else {
        s_ui.cloud_status = SettingsValetNote(page, "");
        lv_obj_add_flag(s_ui.cloud_status, LV_OBJ_FLAG_HIDDEN);
    }
    RefreshCloudOptions();

    if (custom) {
        s_assistants = lv_obj_create(page);
        lv_obj_remove_style_all(s_assistants);
        lv_obj_set_width(s_assistants, lv_pct(100));
        lv_obj_set_height(s_assistants, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_assistants, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_assistants, 20, 0);
        lv_obj_clear_flag(s_assistants, LV_OBJ_FLAG_SCROLLABLE);
        BuildAssistants(s_assistants);
        s_setup_poll = lv_timer_create([](lv_timer_t*) {
            if (!s_ui.alive || !s_setup_visible || s_setup_starting) return;
            static std::string last;
            const auto setup = valet::GetSetupStatus();
            const auto key = setup.note + (setup.running ? "1" : "0") + (setup.starting ? "s" : "");
            if (key != last) { last = key; RefreshAssistantList(); }
            // 只在配置页开过又过期时收尾；还在启动（叫醒 Wi-Fi、开服务）时不能关，
            // 否则服务刚起来就被这里关掉。
            if (!setup.running && !setup.starting && !setup.url.empty()) valet::CloseSetupSession();
        }, 1000, nullptr);
        if (!adding) SettingsValetGap(page, 30);
    }
    if (chat_cloud::UsesXiaozhiProtocol() || custom) {
        s_ui.known = true;
        s_ui.enabled = chat_cloud::LocalTtsEnabled();
    }
    if (adding) { RefreshOptions(); return; }

    // Whether replies are spoken. For the official service this is a server-side
    // preference, fetched and saved with its own requests by the existing handlers.
    SettingsValetLabel(page, CloudText("助手回答时", "When the assistant answers"));
    const char* const speech[] = {CloudText("念出来", "Speak"), CloudText("只看字", "Text only")};
    static const intptr_t kSpeech[] = {1, 0};
    s_ui.speech_seg = SettingsValetSegments(page, speech, 2, OnValetSpeech, kSpeech);
    s_ui.status_lbl = SettingsValetNote(page, "");
    lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
    RefreshOptions();
}
