// assistant_screen.cc — Create / class API / VK / lifecycle
#include "assistant_screen.h"
#include "standby_screen/standby_screen.h"
#include "assistant_vk.h"
#include "assistant_ingest.h"
#include "assistant_flow.h"
#include "assistant_render.h"
#include "assistant_paginate.h"
#include "assistant_ptt.h"
#include "assistant_fonts.h"
#include "assistant_screen_priv.h"

#include "a2ui.h"
#include "a2ui_image.h"
#include "application.h"
#include "epd_refresh.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <cmath>
#include "firmware_memory.h"
#include "assistant_boot_photo.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "fontpack_lvgl.h"
#include "chat_cloud.h"
#include "valet_protocol.h"
#include "valet_reading_context.h"
#include <map>
#include "yunxin_binding/yunxin_binding.h"
#include "board.h"
#include "boot_key_handler.h"
#include "display.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "screen_common.h"
#include "task_screen/task_screen.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"

#include <cstdio>
#include <cstring>
#include <string>
#include "assistant_profiles.h"
#include "custom_chat_config.h"
#include <optional>
#include <new>

#include <esp_log.h>
#include "lv_adapter_epdiy.h"
#include <esp_timer.h>

namespace {
std::optional<PowerNeedHold> s_network_hold; // 仅由 LVGL 生命周期持有，独立于语音通道
std::atomic<bool> s_standby{false}; // 阻止待机期间的迟到语音启动和 UI 更新
bool s_resume_load_pending = false; // 唤醒时新建的助手页在目标帧显示后完成前台业务启动
}

AssistantUiState& Assistant_State() {
    static AssistantUiState s;
    return s;
}

bool Assistant_OnVkKey(const char* key) {
    if (key == nullptr) {
        return false;
    }
    if (AssistantPhoto_ClosePreview()) return true;

    // vk_home：直接出栈（阅读进百问回阅读）；不翻页、不清栈回首页
    if (std::strcmp(key, "vk_home") == 0) {
        ESP_LOGD(TAG, "vk_home -> navigate back");
        // 导航可能等待或取消；页面资源统一在实际卸载时清理。
        VkPageRepeatStop();
        ScreenNavigateBack();
        return true;
    }

    if (std::strcmp(key, "vk_prev") == 0) {
        if (Assistant_TurnPage(-1)) {
            return true;
        }
        ESP_LOGD(TAG, "vk_prev on first page -> navigate back");
        return false; // 默认策略 ScreenNavigateBack
    }

    if (!Assistant_State().layout.has_a2ui_content || Assistant_State().layout.pages.empty()) {
        return false;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        Assistant_TurnPage(1);
        return true;
    }
    return false;
}

namespace {
// 悬窗交给全屏（或全屏回到悬窗）时：旧的一面离开不停语音，新的一面进来接着用同一条连接，通话不断。
bool s_keep_voice = false;
bool s_panel_reopening = false;
// 悬窗：阅读页（host）上从 top 到底的一块，画的是同一个助手页；full=true 时原地拉满全屏，不卸载阅读页。
struct PanelState {
    lv_obj_t* host = nullptr;
    lv_obj_t* root = nullptr;
    lv_coord_t top = 0;
    lv_coord_t trim_from = 0;
    bool full = false;
    lv_obj_t* shadow = nullptr;
    lv_obj_t* head_bar = nullptr;
    lv_obj_t* sub = nullptr;
    lv_obj_t* full_btn = nullptr;
    lv_obj_t* full_icon = nullptr;
    lv_obj_t* full_label = nullptr;
    lv_obj_t* mask = nullptr;
    lv_obj_t* container = nullptr;
    lv_obj_t* page_hit = nullptr;
} s_panel;
// 从悬窗点了「全屏」：全屏页返回书页时，悬窗接着开（同一场对话，通话不断）
bool s_panel_return = false;
lv_timer_t* s_panel_return_timer = nullptr;
}
lv_obj_t* BuildAssistant(lv_obj_t* host, lv_coord_t top);
void AssistantScreen_PanelHostUnload(lv_event_t* e);
void BuildPanelHead(lv_obj_t* scr, bool live);
void Assistant_Enter(lv_obj_t* root) {
    auto& app = Application::GetInstance();
    {
        Assistant_State().chrome.scr = root;
        Assistant_State().active.store(true, std::memory_order_release);
        s_standby.store(StandbyScreen::IsResuming());
        s_resume_load_pending = false;
        if (!s_standby.load() && !s_network_hold) s_network_hold.emplace(PowerNeed::ForegroundNetwork);
        const bool hold_through = BootKey_IsHeld();
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->SetStatusTitlePrefix(nullptr);
            // 顶栏：联网提示走 SetStatus；聆听/说话文案过滤；Idle 后刷时钟
            disp->SetIdleStatusMode(IdleStatusMode::kClock);
            disp->UpdateStatusBar(true);
        }
        if (Assistant_State().chrome.content != nullptr && lv_obj_is_valid(Assistant_State().chrome.content)) {
            a2ui_set_host(Assistant_State().chrome.content);
        }
        if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) {
            ScreenShowToast(hint);
            Assistant_ApplyPttWave(false);
            return;
        }
        if (s_standby.load()) {
            s_resume_load_pending = true;
            return;
        }
        app.Schedule([]() {
            if (AssistantScreen::IsActive()) TaskScreen::RequestCacheRefresh();
        });
        app.StartXiaozhiVoice(hold_through);
        Assistant_ApplyPttWave(Assistant_IsPttWaveWanted());
    }
    s_keep_voice = false;   // 交接完成：之后再离开就照常停语音
}
void Assistant_Leave() {
    auto& app = Application::GetInstance();
    {
        ESP_LOGD(TAG, "leave -> schedule voice session stop (keep session RAM)");
        const bool was_panel = Assistant_State().chrome.panel;   // 下面会清掉，先记下
        Assistant_State().active.store(false, std::memory_order_release);
        s_resume_load_pending = false;
        s_standby.store(false);
        s_network_hold.reset();
        if (Assistant_State().stream.coalesce_timer != nullptr) {
            esp_timer_stop(Assistant_State().stream.coalesce_timer);
        }
        Assistant_State().stream.coalesce_pending = false;
        Assistant_State().stream.coalesce_buf.clear();
        VkPageRepeatStop();
        Assistant_StopPttWaveAnim();
        ScreenPaintCoalesceReset(&Assistant_State().stream.page_paint);
        a2ui_set_host(nullptr);
        // 保留 Assistant_State().layout.flow/Assistant_State().layout.pages；须先毁引用像素的控件，再 a2ui_image_clear（与 Assistant_ClearSession 同序）
        a2ui_image_abort_loads();
        if (Assistant_State().chrome.content != nullptr && lv_obj_is_valid(Assistant_State().chrome.content)) {
            lv_obj_clean(Assistant_State().chrome.content);
        }
        a2ui_image_clear();
        Assistant_ResetTouchPttState();
        AssistantPhoto_Reset();
        Assistant_State().chrome.scr = nullptr;
        Assistant_State().chrome.content = nullptr;
        Assistant_State().chrome.page_label = nullptr;
        Assistant_State().chrome.valet_pager = nullptr;
        Assistant_State().chrome.valet_new = nullptr;
        Assistant_State().chrome.photo_action = nullptr;
        Assistant_State().chrome.photo_action_label = nullptr;
        Assistant_State().chrome.idle_hint = nullptr;
        Assistant_State().chrome.status_label = nullptr;
        Assistant_State().chrome.notification_label = nullptr;
        Assistant_State().ptt.ptt_wave = nullptr;
        Assistant_State().ptt.touch_ptt_hit = nullptr;
        Assistant_State().ptt.touch_ptt_label = nullptr;
        Assistant_State().ptt.ptt_wave_visible = false;
        for (int i = 0; i < kPttWaveBars; ++i) {
            Assistant_State().ptt.ptt_wave_bars[i] = nullptr;
        }
        Assistant_State().layout.ui_font = nullptr;
        Assistant_State().layout.ui_font_bold = nullptr;
        Assistant_State().layout.viewport_w = 0;
        Assistant_State().layout.viewport_h = 0;
        Assistant_State().chrome.panel = false;
        Assistant_State().chrome.panel_title = nullptr;
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->SetStatusTitlePrefix(nullptr);
            disp->SetIdleStatusMode(IdleStatusMode::kClock);
        }
        if (!was_panel && s_panel_return) {
            // 全屏页返回书页：悬窗会在书页画好后接着开；8 秒内没开（去了别处）就照常停
            s_keep_voice = true;
            if (s_panel_return_timer) lv_timer_delete(s_panel_return_timer);
            s_panel_return_timer = lv_timer_create([](lv_timer_t* t) {
                lv_timer_delete(t); s_panel_return_timer = nullptr;
                if (!s_panel_return) return;
                s_panel_return = false; s_keep_voice = false;
                if (!AssistantScreen::IsActive()) Application::GetInstance().StopXiaozhiVoice();
                // 没回书页：书里单独选的云服务用完了，换回「谁来回答」那个
                if (!AssistantScreen::IsActive() && chat_cloud::ActiveProvider() != chat_cloud::SavedProvider())
                    Application::GetInstance().Schedule([] { Application::GetInstance().UseProviderOnTask(chat_cloud::SavedProvider()); });
            }, 8000, nullptr);
        }
        if (s_keep_voice) ESP_LOGI(TAG, "leave: voice kept for the next view of the same conversation");
        else app.StopXiaozhiVoice();
    }
}
void Assistant_OnAssistantLifecycle(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) Assistant_Enter(static_cast<lv_obj_t*>(lv_event_get_target(e)));
    else if (code == LV_EVENT_SCREEN_UNLOADED) Assistant_Leave();
}

void Assistant_OpenAssistantAsync(void* /*arg*/) {
    if (AssistantScreen::IsActive()) {
        ESP_LOGD(TAG, "RequestOpen no-op: already active");
        return;
    }
    HapticPulseIfEnabled();
    ESP_LOGD(TAG, "RequestOpen -> ScreenNavigateTo");
    ScreenNavigateTo(AssistantScreen::Create);
}

bool Assistant_OnBootPressDown() {
    // 开听只走 LongPress / hold-through；按下不听，避免短按先闪「聆听中」
    ESP_LOGD(TAG, "boot press-down ignored for listen (click/long own the semantics)");
    return true;
}

bool Assistant_OnBootPressUp() {
    // BootKey 已清 IsHeld；屏触仍按住时不能停听，否则多源 PTT 会互相打断
    Assistant_StopListeningIfNoPttHeld("boot press-up");
    return true;
}

bool Assistant_OnBootLongPress() {
    // 仅开听；打断说话由 StartListening(Speaking) 内部处理，不走 AbortSpeakingToIdle
    ESP_LOGD(TAG, "boot long-press (500ms) -> StartListening (PTT arm, not click-Assistant_abort)");
    ScreenLvAsync([](void*) {
        if (!AssistantScreen::IsActive() || !BootKey_IsHeld()) return;
        if (!Assistant_AllowListening()) return;
        Assistant_RequestSyncPttOverlay();
        Application::GetInstance().StartListening();
    });
    return true;
}

bool Assistant_OnBootClick() {
    // 仅短按打断 → 待命；进待机改走电源键。长按回合已在 BootKey 层屏蔽。
    if (BootKey_DidLongPress()) {
        ESP_LOGD(TAG, "boot short ignored: long-press already armed listen");
        return true;
    }
    ESP_LOGD(TAG, "boot short -> AbortSpeakingToIdle (no listen)");
    if (chat_cloud::IsCustom() && ValetProtocol::Live()) Application::GetInstance().InterruptCustomLive();
    else Application::GetInstance().AbortSpeakingToIdle();
    return true;
}

bool Assistant_OnBootDoubleClick() {
    return AssistantBootPhoto_OnDoubleClick();
}

bool Assistant_OnVkKeyLongPress(const char* key) {
    if (key == nullptr) {
        return false;
    }
    // vk_home 长按不消费 → VkKey 默认一键回系统首页（不再作 PTT）
    if (!AssistantScreen::IsActive()) {
        return false;
    }
    if (AssistantPhoto_ClosePreview()) return true;
    if (VkPageRepeatTryStart(key, Assistant_PageRepeatStep)) {
        return true;
    }
    return false;
}

bool Assistant_OnVkKeyPressUp(const char* key) {
    if (key == nullptr) {
        return false;
    }
    return VkPageRepeatOnPressUp(key);
}

bool AssistantScreen::IsActive() {
    // 主事件循环 / SetStatus 会跨线程调用；勿 lv_obj_is_valid / lv_screen_active
    return Assistant_State().active.load(std::memory_order_acquire) && !s_standby.load();
}

void AssistantScreen::SetStandby(bool standby) {
    if ((!Assistant_State().active.load() && !s_network_hold) || s_standby.exchange(standby) == standby) return;
    if (standby) {
        Assistant_ResetTouchPttState();
        AssistantPhoto_Suspend();
        Assistant_ApplyPttWave(false);
        s_network_hold.reset();
        Application::GetInstance().StopXiaozhiVoice();
    } else {
        s_network_hold.emplace(PowerNeed::ForegroundNetwork);
        if (s_resume_load_pending) {
            s_resume_load_pending = false;
            Application::GetInstance().Schedule([]() {
                if (AssistantScreen::IsActive()) TaskScreen::RequestCacheRefresh();
            });
        }
        Application::GetInstance().StartXiaozhiVoice(BootKey_IsHeld());
    }
}

bool AssistantScreen::IsPttHeld() {
    return Assistant_IsAnyPttHeld();
}

bool AssistantScreen::IsPttWaveVisible() {
    return Assistant_State().ptt.ptt_wave_visible;
}

void AssistantScreen::SyncPttOverlay() {
    Assistant_RequestSyncPttOverlay();
}

void AssistantScreen::RequestLive() {
    if (!chat_cloud::HasCallEntry()) { ScreenShowToast("通话需先连接自建助理"); return; }
    ValetProtocol::SetLive(true);
    RequestOpen();
}

void AssistantScreen::RequestOpen() {
    if (chat_cloud::IsCustom() && !AssistantScreen::IsActive()) valet::SelectStandaloneContext();
    auto* adapter = LvAdapterEpdiy::Instance();
    if (adapter == nullptr || !adapter->IsReady()) {
        return;
    }
    if (!adapter->LockUi(-1)) {
        ESP_LOGW(TAG, "RequestOpen: adapter lock failed");
        return;
    }
    if (lv_async_call(Assistant_OpenAssistantAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "RequestOpen: lv_async_call failed");
    }
    adapter->UnlockUi();
}

void AssistantScreen::SetEmotion(const char* emotion) {
    if (Assistant_State().chrome.idle_hint == nullptr || !lv_obj_is_valid(Assistant_State().chrome.idle_hint)) {
        return;
    }
    if (Assistant_State().layout.has_a2ui_content || !Assistant_State().layout.flow.empty()) {
        Assistant_HideIdleHint();
        return;
    }
    (void)emotion;
    Assistant_ShowIdleHint();
}

namespace {
std::map<std::string, FlowList> context_histories;
std::string displayed_context_key;
void RestoreContextHistory() {
    const auto profiles = valet::LoadAssistantProfiles();
    const std::string key = chat_cloud::IsCustom() ? valet::ContextStorageKey(profiles.active_id) :
        "official:" + std::to_string(static_cast<int>(chat_cloud::ActiveProvider()));
    if (key == displayed_context_key) return;
    auto& layout = Assistant_State().layout;
    std::lock_guard<std::mutex> lock(Assistant_State().stream.session_mu);
    if (!displayed_context_key.empty()) {
        // Bound retained histories; backend remains the source of durable conversation history.
        if (layout.flow.size() > 600) layout.flow.erase(layout.flow.begin(), layout.flow.end() - 600);
        context_histories[displayed_context_key] = std::move(layout.flow);
    }
    layout.flow.clear();
    auto found = context_histories.find(key);
    if (found != context_histories.end()) { layout.flow = std::move(found->second); context_histories.erase(found); }
    while (context_histories.size() > 6) context_histories.erase(context_histories.begin());
    layout.pages.clear(); layout.flow_first_page.clear(); layout.page_flow_begin.clear();
    layout.open_page_flow_begin = static_cast<size_t>(-1); layout.page_index = 0;
    layout.pin_flow_idx = -1; layout.has_a2ui_content = !layout.flow.empty();
    Assistant_State().stream.coalesce_buf.clear();
    displayed_context_key = key;
}
// 「挂断」和提问页的「通话」在同一个位置。墨水屏换页要一秒左右，用户以为没点上会再点一下，
// 第二下落在新页面的「通话」上又打了出去（2026-11-23 用户反馈）。所以：
// - 切换时带上目标状态（挂断 = 回提问，通话 = 进通话），排队的两次点击结果一样，不会来回切；
// - 每次切换后 2 秒内（从新页面建好算起，覆盖墨水屏刷新）不理会这两个按钮。
std::atomic<int64_t> s_talk_mode_switched_at{0};
constexpr int64_t kTalkModeCooldownUs = 2000000;
void SwitchTalkMode(bool live) {
    s_talk_mode_switched_at = esp_timer_get_time();
    if (!ScreenLvAsync([](void* arg) {
        const bool next = arg != nullptr;
        if (!AssistantScreen::IsActive() || ValetProtocol::Live() == next) return;
        s_panel_return = false;
        if (Assistant_State().chrome.panel && s_panel.root) {
            // 悬窗里换通话 / 挂断：重画悬窗，书页不动，保留全屏/半屏状态与选字引用
            auto* host = s_panel.host;
            const auto top = s_panel.top, trim = s_panel.trim_from;
            const bool full = s_panel.full;
            const auto quote = valet::CurrentReadingQuote();
            s_keep_voice = true;
            s_panel_reopening = true;
            AssistantScreen::ClosePanel();
            s_panel_reopening = false;
            if (quote.Valid()) valet::SetReadingQuote(quote);
            AssistantScreen::OpenPanel(host, top, next);
            if (full) AssistantScreen::PanelFullScreen();
            else if (trim > 0) AssistantScreen::PanelTrimAbove(trim);
            s_talk_mode_switched_at = esp_timer_get_time();
            return;
        }
        Application::GetInstance().StopXiaozhiVoice();
        ValetProtocol::SetLive(next);
        ScreenLoadReplace(lv_obj_create(nullptr));
        ScreenLoadReplace(AssistantScreen::Create());
        s_talk_mode_switched_at = esp_timer_get_time();
    }, live ? reinterpret_cast<void*>(1) : nullptr)) ScreenShowToast("操作繁忙，请重试");
}
void ChangeTalkMode(lv_event_t*) {   // 按钮：通话 / 挂断
    if (esp_timer_get_time() - s_talk_mode_switched_at.load() < kTalkModeCooldownUs) {
        ESP_LOGI(TAG, "talk mode tap ignored: screen just switched");
        return;
    }
    SwitchTalkMode(!ValetProtocol::Live());
}
}  // namespace
void AssistantScreen::EndLiveCall() {
    ESP_LOGI(TAG, "call ended by the assistant: back to ask, flow=%u", unsigned(Assistant_State().layout.flow.size()));
    if (ValetProtocol::Live()) SwitchTalkMode(false);
}
namespace {
void ChangeReadingContext(lv_event_t*) {
    auto* context = new bool(valet::SelectedReadingContext().Valid());
    if (!ScreenLvAsync([](void* ptr) {
        const bool was_book = *static_cast<bool*>(ptr); delete static_cast<bool*>(ptr);
        if (!AssistantScreen::IsActive()) return;
        s_panel_return = false;
        if (was_book) valet::SelectStandaloneContext();
        else if (!valet::SelectBookContext()) { ScreenShowToast("请先打开一本书，等待正文显示"); return; }
        // Close the previous protocol; the new screen gets a new voice generation and session key.
        Application::GetInstance().StopXiaozhiVoice();
        ValetProtocol::SetLive(false);
        auto* transition = lv_obj_create(nullptr);
        ScreenLoadReplace(transition); // complete old screen unload before populating new global UI pointers
        ScreenLoadReplace(AssistantScreen::Create());
    }, context)) delete context;
}
}

// ── Valet chrome: header band, context strip, talk bar, pager. Sizes are panel
// pixels measured from sim/ (新设计 → 提问 / 通话).
namespace {
constexpr int kValetSide = 33, kValetHeadH = 94, kValetStripH = 132, kValetBarH = 100, kValetPagerH = 76;
const lv_font_t* ValetSmall() { const auto* f = fontpack_lv_font_get(25, 2); return f ? f : fontpack_lv_font_ui(); }
const lv_font_t* ValetStrong() { const auto* f = fontpack_lv_font_get(30, 4); return f ? f : fontpack_lv_font_ui(); }
const char* ValetText(const char* zh, const char* en) { return std::strcmp(Lang::CODE, "zh-CN") == 0 ? zh : en; }
lv_obj_t* ValetBox(lv_obj_t* parent, int x, int y, int w, int h) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_clear_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return box;
}
void ValetLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, int width, bool white = false) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = white ? lv_color_white() : lv_color_black();
    line.width = width;
    line.opa = LV_OPA_COVER;
    line.round_start = line.round_end = 1;
    line.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    line.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    lv_draw_line(layer, &line);
}
// Icons on the design's 24-unit grid; the UI font pack has no symbol glyphs.
enum ValetIcon : intptr_t { kIconBack, kIconPrev, kIconNext, kIconCamera, kIconWave, kIconMic, kIconMicOff, kIconEnd, kIconExpand, kIconShrink, kIconWhite = 0x100 };
void DrawValetIcon(lv_event_t* e) {
    auto* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    const intptr_t data = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const bool white = (data & kIconWhite) != 0;
    const int size = a.x2 - a.x1 + 1, w = size >= 44 ? 4 : 3;
    auto L = [&](int x1, int y1, int x2, int y2) {
        ValetLine(layer, a.x1 + x1 * size / 240, a.y1 + y1 * size / 240, a.x1 + x2 * size / 240, a.y1 + y2 * size / 240, w, white);
    };
    auto Arc = [&](int cx, int cy, int r, int from, int to) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = white ? lv_color_white() : lv_color_black();
        arc.width = w;
        arc.opa = LV_OPA_COVER;
        arc.rounded = 1;
        arc.center = {static_cast<lv_value_precise_t>(a.x1 + cx * size / 240), static_cast<lv_value_precise_t>(a.y1 + cy * size / 240)};
        arc.radius = r * size / 240 + w / 2;
        arc.start_angle = from;
        arc.end_angle = to;
        lv_draw_arc(layer, &arc);
    };
    switch (data & 0xFF) {
        case kIconBack: case kIconPrev: L(145, 50, 75, 120); L(75, 120, 145, 190); break;
        case kIconNext: L(95, 50, 165, 120); L(165, 120, 95, 190); break;
        case kIconCamera:
            L(35, 80, 67, 80); L(67, 80, 83, 55); L(83, 55, 157, 55); L(157, 55, 173, 80); L(173, 80, 205, 80);
            L(205, 80, 205, 190); L(205, 190, 35, 190); L(35, 190, 35, 80);
            Arc(120, 132, 32, 0, 360);
            break;
        case kIconWave: L(35, 100, 35, 140); L(78, 65, 78, 175); L(120, 30, 120, 210); L(163, 65, 163, 175); L(205, 100, 205, 140); break;
        case kIconMic: case kIconMicOff:
            L(90, 55, 90, 110); L(150, 55, 150, 110); Arc(120, 55, 30, 180, 360); Arc(120, 110, 30, 0, 180);
            Arc(120, 115, 70, 0, 180); L(120, 185, 120, 215);
            if ((data & 0xFF) == kIconMicOff) L(40, 30, 200, 210);
            break;
        case kIconEnd: L(60, 60, 180, 180); L(180, 60, 60, 180); break;
        case kIconExpand:
            L(135, 45, 195, 45); L(195, 45, 195, 105); L(195, 45, 132, 108);
            L(105, 195, 45, 195); L(45, 195, 45, 135); L(45, 195, 108, 132);
            break;
        case kIconShrink:
            L(132, 48, 132, 108); L(132, 108, 192, 108); L(195, 45, 132, 108);
            L(48, 132, 108, 132); L(108, 132, 108, 192); L(45, 195, 108, 132);
            break;
    }
}
lv_obj_t* ValetIconBox(lv_obj_t* parent, intptr_t icon, int size) {
    auto* box = ValetBox(parent, 0, 0, size, size);
    lv_obj_add_event_cb(box, DrawValetIcon, LV_EVENT_DRAW_MAIN, reinterpret_cast<void*>(icon));
    return box;
}
// Back to where the page was opened from (the book, when asked from a book). Runs after the
// tap event returns, since navigation rebuilds the screen. ScreenRequestBack() is not this:
// despite its name it goes to the home screen.
void ValetBack(lv_event_t*) {
    VkPageRepeatStop();
    if (!ScreenLvAsync([](void*) { ScreenNavigateBack(); })) ScreenShowToast(ValetText("操作繁忙，请重试", "Busy. Try again."));
}
// Start over: drop what is on screen and go back to the welcome page. The saved history is left alone.
void ValetNewChat(lv_event_t*) {
    if (!ScreenLvAsync([](void*) {
        if (!AssistantScreen::IsActive()) return;
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        Assistant_ClearSession();
        Assistant_State().layout.has_a2ui_content = false;
        Assistant_ShowIdleHint();
        Assistant_UpdatePageIndicator();
    })) ScreenShowToast(ValetText("操作繁忙，请重试", "Busy. Try again."));
}
void ValetTurn(lv_event_t* e) { Assistant_TurnPage(lv_event_get_user_data(e) ? 1 : -1); }
// "这一页 / 随口问": the two halves of one slanted strip, like the home page's blocks.
void DrawSubject(lv_event_t* e) {
    const bool book = lv_event_get_user_data(e) != nullptr;
    auto* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    const int mid = (a.x1 + a.x2) / 2, slant = 18;
    lv_draw_rect_dsc_t frame;
    lv_draw_rect_dsc_init(&frame);
    frame.bg_opa = LV_OPA_TRANSP;
    frame.border_color = lv_color_black();
    frame.border_width = 3;
    lv_draw_rect(layer, &frame, &a);
    lv_draw_triangle_dsc_t fill;
    lv_draw_triangle_dsc_init(&fill);
    fill.color = lv_color_black();
    fill.opa = LV_OPA_COVER;
    auto P = [](int x, int y) { return lv_point_precise_t{static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)}; };
    lv_point_precise_t q[4] = {P(mid + slant, a.y1), P(a.x2, a.y1), P(a.x2, a.y2), P(mid - slant, a.y2)};
    if (book) { q[0] = P(a.x1, a.y1); q[1] = P(mid + slant, a.y1); q[2] = P(mid - slant, a.y2); q[3] = P(a.x1, a.y2); }
    fill.p[0] = q[0]; fill.p[1] = q[1]; fill.p[2] = q[2];
    lv_draw_triangle(layer, &fill);
    fill.p[1] = q[2]; fill.p[2] = q[3];
    lv_draw_triangle(layer, &fill);
}
}

lv_coord_t Assistant_ValetBodyTop() { return Assistant_State().chrome.valet_body_top; }
void Assistant_ValetSyncChrome() {
    auto& chrome = Assistant_State().chrome;
    if (!chrome.valet_new || !lv_obj_is_valid(chrome.valet_new)) return;
    const bool has = Assistant_State().layout.has_a2ui_content;
    if (has == !lv_obj_has_flag(chrome.valet_new, LV_OBJ_FLAG_HIDDEN)) return;
    if (has) lv_obj_remove_flag(chrome.valet_new, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(chrome.valet_new, LV_OBJ_FLAG_HIDDEN);
}

// ── 通话状态条（设计稿「通话」页最上面那一条）：一排竖条 + 在听／在说／静音 + 一句提示。
// 竖条跟真实声音走：助理说话时取扬声器正在播的那一包，其余时候取麦克风。最新的电平在正中，
// 往两边是前几次的，所以声音一起伏，波纹就从中间往外走。
// 墨水屏每改一次都要整套重绘：120 ms 一次时通话里 CPU 从 22% 涨到约 60%（2026-11-22 实测）。
// 所以每 300 ms 才看一次，高度只分 5 档，档位不变就不动；没有声音时竖条放平，不再刷新。
bool g_valet_live_bars = true;
namespace {
constexpr int kLiveBars = 9, kLiveBarW = 8, kLiveBarGap = 8, kLiveBarMaxH = 52, kLiveBarMinH = 6, kLiveSteps = 4;
constexpr int kLiveRowH = 92;
struct LiveRow {
    lv_obj_t* row = nullptr;
    lv_obj_t* bars[kLiveBars] = {};
    lv_obj_t* label = nullptr;
    lv_obj_t* hint = nullptr;
    lv_timer_t* timer = nullptr;
    float history[(kLiveBars + 1) / 2] = {};
    int shown[kLiveBars] = {};
    int state = -1;
} s_live;
// 均方根 → 0..1：-48 dBFS 以下算没声音，-12 dBFS 算满。
float LiveLevel(int rms) {
    if (rms <= 0) return 0.f;
    const float db = 20.f * std::log10(float(rms) / 32767.f);
    return std::clamp((db + 48.f) / 36.f, 0.f, 1.f);
}
void LiveTick(lv_timer_t*) {
    if (!s_live.row || !lv_obj_is_valid(s_live.row)) return;
    // 刚接通还没对话时，欢迎页（白底铺满）建在状态行之后，会把「在听」盖住：放回最上层。
    if (auto* parent = lv_obj_get_parent(s_live.row);
        parent && lv_obj_get_index(s_live.row) != int32_t(lv_obj_get_child_count(parent)) - 1) {
        auto* hint = Assistant_State().chrome.idle_hint;
        if (hint && lv_obj_is_valid(hint) && lv_obj_get_parent(hint) == parent &&
            lv_obj_get_index(hint) > lv_obj_get_index(s_live.row))
            lv_obj_move_to_index(s_live.row, lv_obj_get_index(hint));
    }
    if (!g_valet_live_bars) return;   // 仅测试：量竖条本身的开销
    const auto refresh = EpdGetStats();
    if (refresh.busy || refresh.completed < refresh.submitted || refresh.test_active) return;
    const auto device = Application::GetInstance().GetDeviceState();
    const bool muted = ValetProtocol::Muted();
    const int64_t now = esp_timer_get_time();
    int state;   // 0 接通中 1 在听 2 在说 3 静音 4 没接通 5 重新连接
    if (ValetProtocol::Reconnecting()) state = 5;
    else if (device == kDeviceStateConnecting) state = 0;
    else if (device == kDeviceStateSpeaking) state = 2;
    else if (device == kDeviceStateListening) state = muted ? 3 : 1;
    else state = 4;
    if (state != s_live.state) {
        s_live.state = state;
        static const char* const kLabel[] = {"接通中", "在听", "在说", "静音", "没接通", "重新连接"};
        static const char* const kLabelEn[] = {"Connecting", "Listening", "Speaking", "Muted", "Not connected", "Reconnecting"};
        static const char* const kHint[] = {"马上就好", "直接说，不用按键", "按「打断」可以插话", "麦克风关着，助手听不到", "按挂断后再点通话", "线路断了，正在接回"};
        static const char* const kHintEn[] = {"One moment", "Just speak", "Tap Interrupt to cut in", "Mic is off", "End, then call again", "Line dropped, reconnecting"};
        lv_label_set_text(s_live.label, ValetText(kLabel[state], kLabelEn[state]));
        lv_label_set_text(s_live.hint, ValetText(kHint[state], kHintEn[state]));
    }
    // 取这一刻的电平；超过 0.3 秒没有新数据就当没声音。
    // 助理说话时竖条摆成固定的高低形状、不再跟着动：墨水屏每刷新一次，两个核上的供数线程
    // 要占满约 0.4 秒，播放和解码抢不到 CPU，扬声器缓冲只有约 60 ms，每动一下就断一次音
    // （2026-11-22 实测 0.5～1 秒）。提高播放优先级会让屏幕欠载闪烁，两头不能兼得。
    int64_t at = 0;
    int rms = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    // 新语音引擎下刷屏不再卡声音（2026-11-23 实测：141 秒回答里刷了 17 次，音频循环最长 36 ms），
    // 助理说话时竖条也跟着扬声器正在播的声音跳；老链路仍用固定形状。
    const bool speaking_live = state == 2 && audio.EngineMode();
    if (state == 1) rms = ValetProtocol::MicLevel(&at);
    else if (speaking_live) rms = audio.OutputLevel(&at);
    constexpr int kHalf = (kLiveBars + 1) / 2;
    if (state == 2 && !speaking_live) {
        static const float kSpeaking[kHalf] = {1.f, 0.55f, 0.85f, 0.4f, 0.65f};
        for (int d = 0; d < kHalf; ++d) s_live.history[d] = kSpeaking[d];
    } else {
        const float level = (now - at < 300000) ? LiveLevel(rms) : 0.f;
        for (int i = kHalf - 1; i > 0; --i) s_live.history[i] = s_live.history[i - 1];
        s_live.history[0] = level;
    }
    for (int i = 0; i < kLiveBars; ++i) {
        const int d = i < kHalf - 1 ? kHalf - 1 - i : i - (kHalf - 1);
        const float shape = 1.f - 0.15f * d;   // 中间高、两边低
        const int step = std::clamp(int(s_live.history[d] * shape * kLiveSteps + 0.5f), 0, kLiveSteps);
        const int h = kLiveBarMinH + (kLiveBarMaxH - kLiveBarMinH) * step / kLiveSteps;
        if (h != s_live.shown[i]) {
            s_live.shown[i] = h;
            lv_obj_set_height(s_live.bars[i], h);
        }
    }
}
void BuildLiveRow(lv_obj_t* scr, lv_coord_t top) {
    s_live = {};
    auto* row = ValetBox(scr, 0, top, LV_HOR_RES, kLiveRowH);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    s_live.row = row;
    for (int i = 0; i < kLiveBars; ++i) {
        auto* bar = lv_obj_create(row);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, kLiveBarW, kLiveBarMinH);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, kLiveBarW / 2, 0);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, kValetSide + i * (kLiveBarW + kLiveBarGap), 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        s_live.bars[i] = bar;
        s_live.shown[i] = kLiveBarMinH;
    }
    const int text_x = kValetSide + kLiveBars * (kLiveBarW + kLiveBarGap) + 14;
    s_live.label = lv_label_create(row);
    lv_obj_set_style_text_font(s_live.label, ValetStrong(), 0);
    lv_label_set_text(s_live.label, "");
    lv_obj_align(s_live.label, LV_ALIGN_LEFT_MID, text_x, 0);
    s_live.hint = lv_label_create(row);
    lv_obj_set_style_text_font(s_live.hint, ValetSmall(), 0);
    lv_label_set_text(s_live.hint, "");
    lv_obj_align(s_live.hint, LV_ALIGN_RIGHT_MID, -kValetSide, 2);
    s_live.timer = lv_timer_create(LiveTick, 300, nullptr);
    // 竖条一直在变，局刷次数很快攒够，墨水屏每 60 秒会插一次约 1 秒的 GC16 全刷：
    // 全刷的准备线程优先级高于播放，扬声器缓冲只有约 60 ms，实测在说话时卡顿 1079 ms。
    // 通话页在时按流式绘制处理（只做 DU，不插全刷），离开通话页时再整屏清一次。
    if (auto* adapter = LvAdapterEpdiy::Instance()) adapter->SetStreamPaintMode(true);
    lv_obj_add_event_cb(row, [](lv_event_t*) {
        if (s_live.timer) lv_timer_delete(s_live.timer);
        s_live = {};
        if (auto* adapter = LvAdapterEpdiy::Instance()) adapter->SetStreamPaintMode(false);
    }, LV_EVENT_DELETE, nullptr);
    LiveTick(nullptr);
}
}  // namespace

lv_obj_t* AssistantScreen::Create() { return BuildAssistant(nullptr, 0); }

// ── 悬窗（sim/ 新设计 → 正文 → 问这一页）：上沿一条抖点阴影，标题栏整条黑底，书页留在上面。
namespace {
constexpr lv_coord_t kPanelShadowH = 10, kPanelBarH = 70, kPanelHeadH = kPanelShadowH + kPanelBarH;
std::string PanelSubtitle() {
    const auto reading = valet::SelectedReadingContext();
    std::string who;
    const auto profiles = valet::LoadAssistantProfiles();
    if (!chat_cloud::IsCustom()) who = AssistantScreen::DisplayName();   // 书里提问用的是官方云
    else if (const auto* active = profiles.Active()) who = active->name;
    char page[48];
    std::snprintf(page, sizeof(page), ValetText("第 %d 页", "p.%d"), reading.page);
    std::string where = page;
    // 从正文选字进来：写问的是哪几个字（长了只留开头），这一页以前的问答还在下面也不会弄混
    const auto quote = valet::CurrentReadingQuote();
    if (quote.Valid()) {
        std::string q;
        const auto* p = reinterpret_cast<const uint8_t*>(quote.text.data());
        size_t left = quote.text.size(), chars = 0;
        while (left > 0) {
            const size_t n = (*p < 0x80) ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
            if (n > left) break;
            if (++chars > 6) { q += "…"; break; }
            q.append(reinterpret_cast<const char*>(p), n);
            p += n; left -= n;
        }
        where = "「" + q + "」";
    }
    return who.empty() ? where : where + " · " + who;
}
const char* PanelTitle(bool live, bool listening) {
    if (listening) return ValetText("在听 · 松开发送", "Listening");
    if (live) return ValetText("聊这一页", "Talking about it");
    const auto quote = valet::CurrentReadingQuote();
    // 从正文选字进来：标题写功能（和选字卡片上的「问 AI」一样），问的是哪几个字写在副标题里
    if (quote.Valid()) return ValetText("问 AI", "Ask AI");
    return ValetText("问这一页", "Ask this page");
}
void (*s_panel_closed_hook)() = nullptr;
void NotifyPanelClosed() {
    if (s_panel_reopening) return;
    valet::ClearReadingQuote();
    if (auto* hook = s_panel_closed_hook) hook();
}
}  // namespace
void BuildPanelHead(lv_obj_t* scr, bool live) {
    // 抖点阴影：墨水屏只有黑白，隔点画就是灰。一张预先画好的 8 位灰度小图（2×2 的点、每 4 像素一个、隔行错开），
    // 用图片显示：画几百个小方块的绘制任务在墨水屏上会被丢掉（实测整条空白）。
    static lv_image_dsc_t dither{};
    static uint8_t* pixels = nullptr;
    if (!pixels) {
        pixels = static_cast<uint8_t*>(heap_caps_malloc(size_t(LV_HOR_RES) * kPanelShadowH, MALLOC_CAP_SPIRAM));
        if (pixels) {
            for (int y = 0; y < kPanelShadowH; ++y)
                for (int x = 0; x < LV_HOR_RES; ++x) {
                    const int shift = (y / 4) % 2 * 2;
                    const bool ink = y % 4 < 2 && (x + 4 - shift) % 4 < 2;
                    pixels[size_t(y) * LV_HOR_RES + x] = ink ? 0 : 255;
                }
            dither.header.magic = LV_IMAGE_HEADER_MAGIC;
            dither.header.cf = LV_COLOR_FORMAT_L8;
            dither.header.w = LV_HOR_RES;
            dither.header.h = kPanelShadowH;
            dither.header.stride = LV_HOR_RES;
            dither.data_size = uint32_t(LV_HOR_RES) * kPanelShadowH;
            dither.data = pixels;
        }
    }
    if (pixels) {
        auto* shadow = lv_image_create(scr);
        lv_image_set_src(shadow, &dither);
        lv_obj_set_pos(shadow, 0, 0);
        lv_obj_clear_flag(shadow, LV_OBJ_FLAG_CLICKABLE);
        if (s_panel.full) lv_obj_add_flag(shadow, LV_OBJ_FLAG_HIDDEN);
        s_panel.shadow = shadow;
    }
    auto* bar = ValetBox(scr, 0, s_panel.full ? 0 : kPanelShadowH, LV_HOR_RES, kPanelBarH);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_panel.head_bar = bar;
    auto* title = lv_label_create(bar);
    lv_label_set_text(title, PanelTitle(live, false));
    lv_obj_set_style_text_font(title, ValetStrong(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kValetSide, 0);
    lv_obj_update_layout(title);
    Assistant_State().chrome.panel_title = title;
    auto* sub = lv_label_create(bar);
    lv_label_set_text(sub, PanelSubtitle().c_str());
    lv_obj_set_style_text_font(sub, ValetSmall(), 0);
    lv_obj_set_style_text_color(sub, lv_color_white(), 0);
    lv_obj_set_width(sub, LV_HOR_RES - kValetSide - 236 - lv_obj_get_width(title) - 24);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_height(sub, ValetSmall()->line_height);   // 只占一行：放不下就以「…」收尾，不折行
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, kValetSide + lv_obj_get_width(title) + 24, 2);
    s_panel.sub = sub;
    // 右边：放大/缩小按钮（在阅读页上原地拉满全屏或缩回半屏）和 ✕（直接收起回到当前书页）
    auto* full = ValetBox(bar, LV_HOR_RES - 236, 0, 136, kPanelBarH);
    lv_obj_set_flex_flow(full, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(full, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(full, 8, 0);
    lv_obj_add_flag(full, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(full);
    auto* full_icon = ValetIconBox(full, (s_panel.full ? kIconShrink : kIconExpand) | kIconWhite, 28);
    lv_obj_clear_flag(full_icon, LV_OBJ_FLAG_CLICKABLE);
    auto* full_label = lv_label_create(full);
    lv_label_set_text(full_label, s_panel.full ? ValetText("半屏", "Half") : ValetText("全屏", "Full"));
    lv_obj_set_style_text_font(full_label, ValetSmall(), 0);
    lv_obj_set_style_text_color(full_label, lv_color_white(), 0);
    lv_obj_set_style_text_decor(full_label, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_clear_flag(full_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(full, [](lv_event_t*) {
        if (!ScreenLvAsync([](void*) { AssistantScreen::PanelFullScreen(); })) ScreenShowToast(ValetText("操作繁忙，请重试", "Busy. Try again."));
    }, LV_EVENT_CLICKED, nullptr);
    s_panel.full_btn = full;
    s_panel.full_icon = full_icon;
    s_panel.full_label = full_label;
    auto* close = ValetBox(bar, LV_HOR_RES - 100, 0, 100, kPanelBarH);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(close);
    lv_obj_center(ValetIconBox(close, kIconEnd | kIconWhite, 30));
    lv_obj_add_event_cb(close, [](lv_event_t*) {
        if (!ScreenLvAsync([](void*) { AssistantScreen::ClosePanel(); })) ScreenShowToast(ValetText("操作繁忙，请重试", "Busy. Try again."));
    }, LV_EVENT_CLICKED, nullptr);
}

bool AssistantScreen::PanelOpen() { return s_panel.root != nullptr; }
bool AssistantScreen::PanelFull() { return s_panel.root != nullptr && s_panel.full; }
lv_coord_t AssistantScreen::PanelTop() { return s_panel.top; }
void AssistantScreen::PanelTrimAbove(lv_coord_t from) {
    if (!s_panel.root) return;
    s_panel.trim_from = from;
    const lv_coord_t h = s_panel.full ? 0 : (s_panel.top - from);
    if (h <= 0) {
        if (s_panel.mask && lv_obj_is_valid(s_panel.mask)) lv_obj_add_flag(s_panel.mask, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!s_panel.mask || !lv_obj_is_valid(s_panel.mask)) {
        // 悬窗上方一条白：盖住被悬窗切了一半的那行（画在悬窗外面，所以悬窗要允许子对象画出界）
        s_panel.mask = ValetBox(s_panel.root, 0, 0, LV_HOR_RES, 1);
        lv_obj_set_style_bg_color(s_panel.mask, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_panel.mask, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_panel.root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
    lv_obj_remove_flag(s_panel.mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_panel.mask, 0, -h);
    lv_obj_set_size(s_panel.mask, LV_HOR_RES, h);
}
// 悬窗没有状态栏放录音波形：在听时标题换成「在听 · 松开发送」
void Assistant_PanelListening(bool listening) {
    auto* title = Assistant_State().chrome.panel_title;
    if (!title || !lv_obj_is_valid(title)) return;
    lv_label_set_text(title, PanelTitle(chat_cloud::IsCustom() && ValetProtocol::Live(), listening));
    if (s_panel.sub && lv_obj_is_valid(s_panel.sub)) {
        lv_obj_update_layout(title);
        lv_obj_align(s_panel.sub, LV_ALIGN_LEFT_MID, kValetSide + lv_obj_get_width(title) + 24, 2);
        lv_obj_set_width(s_panel.sub, LV_HOR_RES - kValetSide - 236 - lv_obj_get_width(title) - 24);
    }
}

bool AssistantScreen::OpenPanel(lv_obj_t* host, lv_coord_t top, bool live) {
    if (s_panel.root) return true;
    if (!host) return false;   // 官方云也行：这一页由服务器来设备上取（self.reader.get_current_page），只是没有通话
    if (!valet::SelectBookContext()) { ScreenShowToast(ValetText("当前页没有可提问的正文，请等待页面加载", "This page has no text to ask about yet")); return false; }
    if (live && !chat_cloud::HasCallEntry()) { ScreenShowToast(ValetText("这个助理还没配置通话", "This assistant has no Call set up")); return false; }
    ValetProtocol::SetLive(live);
    s_panel.full = false;
    s_panel.trim_from = top;
    // 书页（host）离开时先收起悬窗：不等它跟着书页一起被删
    lv_obj_remove_event_cb(host, AssistantScreen_PanelHostUnload);
    lv_obj_add_event_cb(host, AssistantScreen_PanelHostUnload, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    auto* root = BuildAssistant(host, top);
    s_panel.host = host; s_panel.root = root; s_panel.top = top;
    lv_obj_add_event_cb(root, [](lv_event_t* e) {
        if (lv_event_get_target(e) != s_panel.root) return;
        s_panel = {};
        Assistant_Leave();
        NotifyPanelClosed();
    }, LV_EVENT_DELETE, nullptr);
    ESP_LOGI(TAG, "panel open at y=%d (%s), page %d", int(top), live ? "call" : "ask", valet::SelectedReadingContext().page);
    Assistant_Enter(root);
    return true;
}

void AssistantScreen::ClosePanel() {
    if (!s_panel.root) return;
    auto* root = s_panel.root;
    s_panel = {};
    Assistant_Leave();
    if (lv_obj_is_valid(root)) lv_obj_delete(root);
    NotifyPanelClosed();
    ESP_LOGI(TAG, "panel closed");
}

void AssistantScreen::SetPanelClosedHook(void (*hook)()) { s_panel_closed_hook = hook; }

void AssistantScreen_PanelHostUnload(lv_event_t* e) {
    if (s_panel.root && lv_event_get_target(e) == s_panel.host) AssistantScreen::ClosePanel();
}

void AssistantScreen::PanelPageChanged() {
    if (!s_panel.root) return;
    if (!valet::SelectBookContext()) return;
    if (s_panel.sub && lv_obj_is_valid(s_panel.sub)) lv_label_set_text(s_panel.sub, PanelSubtitle().c_str());
    ValetProtocol::ReadingContextChanged();
}

void AssistantScreen::PanelFullScreen() {
    if (!s_panel.root || !lv_obj_is_valid(s_panel.root)) return;
    s_panel.full = !s_panel.full;
    const lv_coord_t eff_top = s_panel.full ? 0 : s_panel.top;
    const lv_coord_t view_h = LV_VER_RES - eff_top;
    lv_obj_set_pos(s_panel.root, 0, eff_top);
    lv_obj_set_size(s_panel.root, LV_HOR_RES, view_h);
    lv_obj_move_foreground(s_panel.root);
    if (s_panel.shadow && lv_obj_is_valid(s_panel.shadow)) {
        if (s_panel.full) lv_obj_add_flag(s_panel.shadow, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_panel.shadow, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_panel.head_bar && lv_obj_is_valid(s_panel.head_bar)) {
        lv_obj_set_pos(s_panel.head_bar, 0, s_panel.full ? 0 : kPanelShadowH);
    }
    if (s_panel.full_label && lv_obj_is_valid(s_panel.full_label)) {
        lv_label_set_text(s_panel.full_label, s_panel.full ? ValetText("半屏", "Half") : ValetText("全屏", "Full"));
    }
    if (s_panel.full_icon && lv_obj_is_valid(s_panel.full_icon)) {
        lv_obj_remove_event_cb(s_panel.full_icon, DrawValetIcon);
        lv_obj_add_event_cb(s_panel.full_icon, DrawValetIcon, LV_EVENT_DRAW_MAIN,
                            reinterpret_cast<void*>((s_panel.full ? kIconShrink : kIconExpand) | kIconWhite));
        lv_obj_invalidate(s_panel.full_icon);
    }
    PanelTrimAbove(s_panel.trim_from);
    lv_coord_t header_h = s_panel.full ? kPanelBarH : kPanelHeadH;
    if (s_live.row && lv_obj_is_valid(s_live.row)) {
        lv_obj_set_pos(s_live.row, 0, header_h);
        header_h += kLiveRowH;
    }
    Assistant_State().chrome.valet_body_top = header_h;
    const lv_coord_t body_h = view_h - header_h - kFooterH - kPageIndicatorH;
    const lv_font_t* ui_font = Assistant_UiFont();
    if (s_panel.container && lv_obj_is_valid(s_panel.container)) {
        lv_obj_set_size(s_panel.container, LV_HOR_RES, view_h - header_h);
        lv_obj_align(s_panel.container, LV_ALIGN_TOP_MID, 0, header_h);
    }
    if (auto* content = Assistant_State().chrome.content; content && lv_obj_is_valid(content)) {
        lv_obj_set_height(content, body_h);
    }
    if (s_panel.page_hit && lv_obj_is_valid(s_panel.page_hit)) {
        lv_obj_set_size(s_panel.page_hit, LV_HOR_RES, body_h);
        lv_obj_set_pos(s_panel.page_hit, 0, header_h);
    }
    if (auto* hint = Assistant_State().chrome.idle_hint; hint && lv_obj_is_valid(hint)) {
        lv_obj_set_size(hint, LV_HOR_RES, view_h);
        lv_obj_clean(hint);
        Assistant_BuildIdleHint(hint);
    }
    if (s_panel.head_bar && lv_obj_is_valid(s_panel.head_bar)) lv_obj_move_foreground(s_panel.head_bar);
    auto& layout = Assistant_State().layout;
    {
        std::lock_guard<std::mutex> g(Assistant_State().stream.session_mu);
        size_t anchor_flow = 0;
        bool had_anchor = false;
        if (layout.page_index >= 0 && static_cast<size_t>(layout.page_index) < layout.page_flow_begin.size()) {
            anchor_flow = layout.page_flow_begin[static_cast<size_t>(layout.page_index)];
            had_anchor = true;
        }
        layout.viewport_w = LV_HOR_RES - kContentPadX * 2;
        layout.viewport_h = std::max<lv_coord_t>(body_h - kContentPadY * 2, Assistant_FontLineHeight(ui_font));
        if (!layout.flow.empty()) {
            Assistant_RebuildPages();
            Assistant_TrimFlowToMaxPages();
            if (had_anchor && !layout.page_flow_begin.empty()) {
                int best = 0;
                for (int p = 0; p < static_cast<int>(layout.page_flow_begin.size()); ++p) {
                    if (layout.page_flow_begin[static_cast<size_t>(p)] <= anchor_flow) best = p;
                    else break;
                }
                layout.page_index = best;
            }
            if (layout.page_index >= static_cast<int>(layout.pages.size()))
                layout.page_index = static_cast<int>(layout.pages.size()) - 1;
            if (layout.page_index < 0) layout.page_index = 0;
        }
    }
    Assistant_State().stream.rendered_page_json.clear();
    if (layout.has_a2ui_content && !layout.pages.empty()) {
        Assistant_HideIdleHint();
        Assistant_RenderCurrentPage();
    } else {
        Assistant_ShowIdleHint();
        Assistant_UpdatePageIndicator();
    }
    ESP_LOGI(TAG, "panel toggle full=%d view_h=%d pages=%u idx=%d", s_panel.full ? 1 : 0, int(view_h),
             unsigned(layout.pages.size()), layout.page_index);
}

bool AssistantScreen::PanelVkKey(const char* key) {
    if (!s_panel.root || !s_panel.full || !key) return false;
    if (std::strcmp(key, "vk_next") == 0) {
        if (Assistant_State().layout.has_a2ui_content && !Assistant_State().layout.pages.empty()) {
            Assistant_TurnPage(1);
        }
        return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        if (Assistant_TurnPage(-1)) return true;
        ScreenLvAsync([](void*) { AssistantScreen::ClosePanel(); });
        return true;
    }
    return false;
}

bool AssistantScreen::PanelReturnPending() { return s_panel_return; }
bool AssistantScreen::ConsumePanelReturn() {
    if (!s_panel_return) return false;
    s_panel_return = false;
    if (s_panel_return_timer) { lv_timer_delete(s_panel_return_timer); s_panel_return_timer = nullptr; }
    return true;
}

bool AssistantScreen::PanelBootClick() {
    if (!s_panel.root) return false;
    if (BootKey_DidLongPress()) return true;
    // AI 在说：打断；没在说：收起悬窗
    if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) return Assistant_OnBootClick();
    ScreenLvAsync([](void*) { AssistantScreen::ClosePanel(); });
    return true;
}
bool AssistantScreen::PanelBootLongPress() {
    if (!s_panel.root) return false;
    if (chat_cloud::IsCustom() && ValetProtocol::Live()) return true;   // 通话里一直在听
    return Assistant_OnBootLongPress();
}
bool AssistantScreen::PanelBootPressUp() {
    if (!s_panel.root) return false;
    return Assistant_OnBootPressUp();
}

// host = nullptr：整屏助手页；否则画进 host（阅读页）上从 top 到底的一块悬窗。
lv_obj_t* BuildAssistant(lv_obj_t* host, lv_coord_t top) {
    RestoreContextHistory();
    const bool panel = host != nullptr;
    ESP_LOGI(TAG, "launch AssistantScreen%s", panel ? " (panel over the page)" : "");
    const lv_font_t* ui_font = Assistant_UiFont();
    Assistant_UiFontBold();

    lv_obj_t* scr = lv_obj_create(panel ? host : nullptr);
    const lv_coord_t view_h = panel ? LV_VER_RES - top : LV_VER_RES;
    if (panel) {
        lv_obj_remove_style_all(scr);
        lv_obj_set_size(scr, LV_HOR_RES, view_h);
        lv_obj_set_pos(scr, 0, top);
        lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);   // 悬窗里的点按不漏到下面的书页
    }
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ui_font, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Assistant_DisableScroll(scr);
    Assistant_State().chrome.panel = panel;

    // 顶栏与其它 App 共用 ScreenCreateStatusBar（25@2），勿用会话 30px 字；悬窗没有状态栏
    EpdStatusBar status{};
    if (!panel) status = ScreenCreateStatusBar(scr);
    lv_coord_t header_h = panel ? 0 : status.height;
    lv_obj_t* context_header = nullptr;
    const bool custom = chat_cloud::IsCustom();
    const bool live_now = custom && ValetProtocol::Live();
    if (panel) {
        BuildPanelHead(scr, live_now);
        header_h += kPanelHeadH;
    } else {
        // Back, what this page is, who answers; "新对话" on the right while there is a conversation.
        auto* head = ValetBox(scr, 0, header_h, LV_HOR_RES, kValetHeadH);
        lv_obj_set_style_border_width(head, 3, 0);
        lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(head, lv_color_black(), 0);
        lv_obj_set_style_bg_color(head, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
        auto* back = ValetBox(head, 0, 0, 96, 90);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(back);
        lv_obj_add_event_cb(back, ValetBack, LV_EVENT_CLICKED, nullptr);
        lv_obj_align(ValetIconBox(back, kIconBack, 44), LV_ALIGN_LEFT_MID, 28, 0);
        auto* title = lv_label_create(head);
        lv_label_set_text(title, live_now ? ValetText("通话", "Call") : ValetText("提问", "Ask"));
        lv_obj_set_style_text_font(title, ValetStrong(), 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 96, 0);
        lv_obj_update_layout(title);
        std::string who = AssistantScreen::DisplayName();
        if (custom) {
            const auto profiles = valet::LoadAssistantProfiles();
            if (const auto* active = profiles.Active())
                who = active->name + " · " + (active->connection.backend == custom_chat::Backend::Hermes ? "Hermes" : "OpenClaw");
        }
        auto* sub = lv_label_create(head);
        lv_label_set_text(sub, who.c_str());
        lv_obj_set_style_text_font(sub, ValetSmall(), 0);
        lv_obj_set_width(sub, LV_HOR_RES - 96 - lv_obj_get_width(title) - 18 - 150);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 96 + lv_obj_get_width(title) + 18, 3);
        auto* fresh = ValetBox(head, 0, 0, 140, 90);
        lv_obj_align(fresh, LV_ALIGN_RIGHT_MID, -kValetSide + 8, 0);
        lv_obj_add_flag(fresh, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(fresh);
        lv_obj_add_event_cb(fresh, ValetNewChat, LV_EVENT_CLICKED, nullptr);
        auto* fresh_label = lv_label_create(fresh);
        lv_label_set_text(fresh_label, ValetText("新对话", "New"));
        lv_obj_set_style_text_font(fresh_label, ValetSmall(), 0);
        lv_obj_set_style_text_decor(fresh_label, LV_TEXT_DECOR_UNDERLINE, 0);
        lv_obj_align(fresh_label, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_add_flag(fresh, LV_OBJ_FLAG_HIDDEN);
        Assistant_State().chrome.valet_new = live_now ? nullptr : fresh;
        context_header = head;
        header_h += kValetHeadH;
    }
    if (custom && !panel) {
        // What the talk is about: this page of the book, or nothing in particular.
        const auto reading = valet::SelectedReadingContext();
        const bool book = reading.Valid();
        auto* strip = ValetBox(scr, 0, header_h, LV_HOR_RES, book ? kValetStripH + 70 : kValetStripH - 44);
        lv_obj_set_style_bg_color(strip, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        auto* subject = ValetBox(strip, kValetSide, 16, LV_HOR_RES - 2 * kValetSide, 70);
        lv_obj_add_flag(subject, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(subject);
        lv_obj_add_event_cb(subject, DrawSubject, LV_EVENT_DRAW_MAIN, book ? reinterpret_cast<void*>(1) : nullptr);
        lv_obj_add_event_cb(subject, ChangeReadingContext, LV_EVENT_CLICKED, nullptr);
        for (int i = 0; i < 2; ++i) {
            auto* name = lv_label_create(subject);
            lv_label_set_text(name, i == 0 ? ValetText("这一页", "This page") : ValetText("随口问", "Anything"));
            const bool on = (i == 0) == book;
            lv_obj_set_style_text_font(name, on ? ValetStrong() : fontpack_lv_font_ui(), 0);
            lv_obj_set_style_text_color(name, on ? lv_color_white() : lv_color_black(), 0);
            lv_obj_align(name, LV_ALIGN_CENTER, (i == 0 ? -1 : 1) * (LV_HOR_RES - 2 * kValetSide) / 4, 0);
            lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
        }
        if (book) {
            // Which page went along, with a bar down its left side like a quotation.
            auto* quote = ValetBox(strip, kValetSide, 102, LV_HOR_RES - 2 * kValetSide, 88);
            lv_obj_set_style_border_width(quote, 6, 0);
            lv_obj_set_style_border_side(quote, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(quote, lv_color_black(), 0);
            auto* where = lv_label_create(quote);
            lv_label_set_text_fmt(where, ValetText("%s · %s · 第 %d 页%s", "%s · %s · p.%d%s"), reading.title.c_str(),
                                  reading.chapter.c_str(), reading.page, reading.truncated ? ValetText("（节选）", " (excerpt)") : "");
            lv_obj_set_style_text_font(where, ValetSmall(), 0);
            lv_obj_set_width(where, LV_HOR_RES - 2 * kValetSide - 30);
            lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);
            lv_obj_set_height(where, lv_font_get_line_height(ValetSmall()));   // 只占一行：长书名省略，不压住下面的摘录
            lv_obj_set_pos(where, 20, 4);
            auto* text = lv_label_create(quote);
            // 补全过前后段落的正文以说明行开头：卡片上从这一页的第一个字显示
            const auto start = reading.text.find("【本页开始】", reading.text.find('\n') == std::string::npos ? 0 : reading.text.find('\n'));   // 第一行是说明
            lv_label_set_text(text, start == std::string::npos ? reading.text.c_str()
                                                                : reading.text.c_str() + start + std::strlen("【本页开始】"));
            lv_obj_set_style_text_font(text, fontpack_lv_font_ui(), 0);
            lv_obj_set_width(text, LV_HOR_RES - 2 * kValetSide - 30);
            lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
            lv_obj_set_height(text, 36);
            lv_obj_set_pos(text, 20, 44);
        }
        header_h += book ? kValetStripH + 70 : kValetStripH - 44;
    }
    if (live_now) {
        BuildLiveRow(scr, header_h);
        header_h += kLiveRowH;
    }
    Assistant_State().chrome.valet_body_top = header_h;
    const lv_coord_t body_h = view_h - header_h - kFooterH - kPageIndicatorH;
    Assistant_State().layout.viewport_w = LV_HOR_RES - kContentPadX * 2;
    Assistant_State().layout.viewport_h = body_h - kContentPadY * 2;
    if (Assistant_State().layout.viewport_h < Assistant_FontLineHeight(ui_font)) {
        Assistant_State().layout.viewport_h = Assistant_FontLineHeight(ui_font);
    }

    lv_obj_t* container = lv_obj_create(scr);
    lv_obj_set_size(container, LV_HOR_RES, view_h - header_h);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_white(), 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(container);
    if (panel) s_panel.container = container;

    lv_obj_t* content = lv_obj_create(container);
    lv_obj_set_width(content, LV_HOR_RES);
    lv_obj_set_height(content, body_h);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_hor(content, kContentPadX, 0);
    lv_obj_set_style_pad_ver(content, kContentPadY, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(content);

    // 空会话页由 LVGL 控件绘制，状态栏保持在最上层。
    lv_obj_t* idle_hint = lv_obj_create(scr);
    lv_obj_remove_style_all(idle_hint);
    lv_obj_set_size(idle_hint, LV_HOR_RES, view_h);
    lv_obj_add_flag(idle_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(idle_hint, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    Assistant_State().chrome.valet_body_top = header_h;   // 欢迎页从标题下面排
    Assistant_BuildIdleHint(idle_hint);
    lv_obj_t* ptt_wave = nullptr;
    if (!panel) {
        lv_obj_move_foreground(status.bar);
        lv_obj_move_foreground(status.overlay);
        // 录音波形叠在状态栏条带居中：高度约半栏偏矮、竖条更密；不改状态栏尺寸；按住说话时盖住时钟
        Assistant_State().ptt.ptt_wave_max_h = status.height / 2;
        if (Assistant_State().ptt.ptt_wave_max_h < kPttWaveMinBarH) {
            Assistant_State().ptt.ptt_wave_max_h = kPttWaveMinBarH;
        }
        ptt_wave = Assistant_CreatePttWaveHost(scr, Assistant_State().ptt.ptt_wave_max_h);
        lv_obj_align(ptt_wave, LV_ALIGN_TOP_MID, 0, (status.height - Assistant_State().ptt.ptt_wave_max_h) / 2);
        lv_obj_add_flag(ptt_wave, LV_OBJ_FLAG_HIDDEN);
    }   // 悬窗没有状态栏：在听时改标题（见 Assistant_ApplyPttWave）

    // 正文透明层只翻页，覆盖欢迎页但不覆盖顶栏或底栏。
    auto* page_hit = lv_obj_create(scr);
    lv_obj_remove_style_all(page_hit);
    lv_obj_set_size(page_hit, LV_HOR_RES, body_h);
    lv_obj_set_pos(page_hit, 0, header_h);
    lv_obj_add_flag(page_hit, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(page_hit);
    HapticAttachClick(page_hit);
    lv_obj_add_event_cb(page_hit, OnTouchPageClicked, LV_EVENT_CLICKED, nullptr);
    // 「自建」还没连助理：没有回答可翻，欢迎页上是「去连接」按钮，别让翻页层把它盖住
    if (chat_cloud::IsCustom() && !chat_cloud::Available()) lv_obj_add_flag(page_hit, LV_OBJ_FLAG_HIDDEN);
    if (panel) s_panel.page_hit = page_hit;

    const int bar_w = LV_HOR_RES - 2 * kValetSide;
    auto* bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, bar_w, kValetBarH);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 3, 0);
    lv_obj_set_style_border_color(bar, lv_color_black(), 0);
    Assistant_DisableScroll(bar);
    const bool custom_live = chat_cloud::IsCustom() && ValetProtocol::Live();
    if (custom_live) {
        // 通话里沿用提问页的胶囊条：左边「静音」「打断」两格，右边是和「通话」同位置、同斜切的黑色「挂断」。
        // （2026-11-23 用户：进了通话也保留圆角和斜切的风格。）
        constexpr lv_coord_t kEndW = 190;   // 与提问页「通话」楔形同宽
        const lv_coord_t inner_w = bar_w - 6, inner_h = kValetBarH - 6, half = (inner_w - kEndW) / 2;
        static lv_obj_t* s_mute_label = nullptr;
        static lv_obj_t* s_mute_icon = nullptr;
        const auto on_click = [](lv_event_t* e) {
            const intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
            if (which == 2) { ChangeTalkMode(nullptr); return; }
            if (which == 1) {
                // Stop what the assistant is saying; the call stays up.
                Application::GetInstance().AbortSpeaking(kAbortReasonNone);
                return;
            }
            ValetProtocol::SetMuted(!ValetProtocol::Muted());
            if (s_mute_label && lv_obj_is_valid(s_mute_label))
                lv_label_set_text(s_mute_label, ValetProtocol::Muted() ? ValetText("取消静音", "Unmute") : ValetText("静音", "Mute"));
            if (s_mute_icon && lv_obj_is_valid(s_mute_icon)) {   // 图标画法存在绘制回调的参数里：换一个回调
                lv_obj_remove_event_cb(s_mute_icon, DrawValetIcon);
                lv_obj_add_event_cb(s_mute_icon, DrawValetIcon, LV_EVENT_DRAW_MAIN,
                                    reinterpret_cast<void*>(ValetProtocol::Muted() ? kIconMicOff : kIconMic));
                lv_obj_invalidate(s_mute_icon);
            }
            ScreenShowToast(ValetProtocol::Muted() ? ValetText("麦克风已静音", "Microphone muted") : ValetText("麦克风已恢复", "Microphone on"));
        };
        // 左边两格：图标 + 文字，横排居中
        for (int i = 0; i < 2; ++i) {
            auto* cell = lv_obj_create(bar);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, half, inner_h);
            lv_obj_set_pos(cell, i * half, 0);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(cell, 10, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            Assistant_DisableScroll(cell);
            HapticAttachClick(cell);
            if (i == 0) {
                auto* icon = ValetIconBox(cell, ValetProtocol::Muted() ? kIconMicOff : kIconMic, 36);
                lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
                s_mute_icon = icon;
            } else {   // 打断：实心小方块（停止）
                auto* stop = lv_obj_create(cell);
                lv_obj_remove_style_all(stop);
                lv_obj_set_size(stop, 22, 22);
                lv_obj_set_style_radius(stop, 4, 0);
                lv_obj_set_style_bg_color(stop, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(stop, LV_OPA_COVER, 0);
                lv_obj_clear_flag(stop, LV_OBJ_FLAG_CLICKABLE);
            }
            auto* label = lv_label_create(cell);
            lv_label_set_text(label, i == 0 ? (ValetProtocol::Muted() ? ValetText("取消静音", "Unmute") : ValetText("静音", "Mute"))
                                            : ValetText("打断", "Interrupt"));
            lv_obj_set_style_text_font(label, ValetStrong(), 0);
            lv_obj_set_style_text_color(label, lv_color_black(), 0);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
            if (i == 0) s_mute_label = label;
            lv_obj_add_event_cb(cell, on_click, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        }
        // 两格之间一条短竖线
        auto* divider = lv_obj_create(bar);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, 2, inner_h / 2);
        lv_obj_set_pos(divider, half - 1, inner_h / 4);
        lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        // 右边黑色斜切「挂断」，画法与提问页「通话」相同
        auto* end = lv_obj_create(bar);
        lv_obj_remove_style_all(end);
        lv_obj_set_size(end, kEndW, inner_h);
        lv_obj_align(end, LV_ALIGN_RIGHT_MID, 0, 0);
        Assistant_DisableScroll(end);
        lv_obj_add_flag(end, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(end);
        lv_obj_add_event_cb(end, [](lv_event_t* e) {
            lv_area_t a;
            lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
            lv_draw_triangle_dsc_t fill;
            lv_draw_triangle_dsc_init(&fill);
            fill.color = lv_color_black();
            fill.opa = LV_OPA_COVER;
            auto P = [](int x, int y) { return lv_point_precise_t{static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)}; };
            fill.p[0] = P(a.x1 + 26, a.y1); fill.p[1] = P(a.x1 + 30, a.y2); fill.p[2] = P(a.x1, a.y2);
            lv_draw_triangle(lv_event_get_layer(e), &fill);
            lv_draw_rect_dsc_t rest;
            lv_draw_rect_dsc_init(&rest);
            rest.bg_color = lv_color_black();
            rest.bg_opa = LV_OPA_COVER;
            lv_area_t body = {a.x1 + 26, a.y1, a.x2, a.y2};
            lv_draw_rect(lv_event_get_layer(e), &rest, &body);
        }, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_add_event_cb(end, on_click, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(2)));
        lv_obj_align(ValetIconBox(end, kIconEnd | kIconWhite, 36), LV_ALIGN_LEFT_MID, 44, 0);
        auto* end_label = lv_label_create(end);
        lv_label_set_text(end_label, ValetText("挂断", "End"));
        lv_obj_set_style_text_font(end_label, ValetStrong(), 0);
        lv_obj_set_style_text_color(end_label, lv_color_white(), 0);
        lv_obj_align(end_label, LV_ALIGN_LEFT_MID, 90, 0);
        lv_obj_clear_flag(end_label, LV_OBJ_FLAG_CLICKABLE);
    }
    const bool baiwen = chat_cloud::ActiveProvider() == chat_cloud::Provider::Baiwen;
    const bool camera_available = baiwen && Board::GetInstance().GetCamera() != nullptr;
    // 百问固定预留拍照入口，检测完成只显示图标，不移动录音区域。
    // Camera on the left when there is one, the call wedge on the right when calls are possible;
    // everything between is the hold-to-talk area.
    const lv_coord_t camera_w = baiwen ? 106 : 0;
    const bool call_entry = chat_cloud::HasCallEntry() && !custom_live;
    const lv_coord_t action_w = call_entry ? 190 : 0;
    lv_obj_t* touch_ptt_hit = Assistant_CreateTouchPttHitLayer(bar);
    lv_obj_set_pos(touch_ptt_hit, camera_w, 0);
    lv_obj_set_size(touch_ptt_hit, bar_w - 6 - camera_w - action_w, kValetBarH - 6);
    lv_obj_set_style_text_font(touch_ptt_hit, ValetStrong(), 0);
    lv_obj_set_style_text_letter_space(touch_ptt_hit, 3, 0);
    if (baiwen) {
        auto* camera = lv_button_create(bar);
        lv_obj_remove_style_all(camera);
        lv_obj_add_flag(camera, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(camera, camera_w, kValetBarH - 6);
        lv_obj_set_pos(camera, 0, 0);
        if (!camera_available) lv_obj_add_flag(camera, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(camera, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(camera, 0, 0);
        lv_obj_set_style_text_color(camera, lv_color_black(), 0);
        Assistant_DisableScroll(camera);
        lv_obj_add_event_cb(camera, [](lv_event_t* event) {
            lv_area_t bounds;
            lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(event)), &bounds);
            const int cx = (bounds.x1 + bounds.x2) / 2;
            const int cy = (bounds.y1 + bounds.y2) / 2;
            lv_draw_rect_dsc_t box;
            lv_draw_rect_dsc_init(&box);
            box.bg_opa = LV_OPA_TRANSP;
            box.border_color = lv_color_black();
            box.border_width = 2;
            box.radius = UiSx(4);
            lv_area_t area = {cx - UiSx(18), cy - UiSy(12), cx + UiSx(18), cy + UiSy(12)};
            lv_draw_rect(lv_event_get_layer(event), &box, &area);
            lv_draw_arc_dsc_t lens;
            lv_draw_arc_dsc_init(&lens);
            lens.color = lv_color_black();
            lens.width = 2;
            lens.center = {cx, cy};
            lens.radius = UiSx(7);
            lens.start_angle = 0;
            lens.end_angle = 360;
            lv_draw_arc(lv_event_get_layer(event), &lens);
            area = {cx - UiSx(8), cy - UiSy(16), cx + UiSx(8), cy - UiSy(12)};
            box.bg_opa = LV_OPA_COVER;
            box.bg_color = lv_color_black();
            lv_draw_rect(lv_event_get_layer(event), &box, &area);
        }, LV_EVENT_DRAW_MAIN, nullptr);
        HapticAttachClick(camera);
        lv_obj_add_event_cb(camera, [](lv_event_t*) { AssistantBootPhoto_OnDoubleClick(); }, LV_EVENT_CLICKED, nullptr);
    }
    // Right end of the pill: "send" while a photo is pending, otherwise the way into a call.
    auto* action = lv_obj_create(bar);
    lv_obj_remove_style_all(action);
    lv_obj_set_size(action, call_entry ? action_w : 150, kValetBarH - 6);
    lv_obj_align(action, LV_ALIGN_RIGHT_MID, 0, 0);
    Assistant_DisableScroll(action);
    lv_obj_add_event_cb(action, [](lv_event_t*) { AssistantPhoto_Send(); }, LV_EVENT_CLICKED, nullptr);
    if (custom_live) { lv_obj_add_flag(touch_ptt_hit, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(action, LV_OBJ_FLAG_HIDDEN); }
    auto* action_label = lv_label_create(action);
    lv_label_set_text(action_label, Lang::Strings::ASSISTANT_SEND);
    lv_obj_set_style_text_font(action_label, ValetStrong(), 0);
    lv_obj_set_style_text_color(action_label, lv_color_white(), 0);
    lv_obj_center(action_label);
    lv_obj_add_flag(action_label, LV_OBJ_FLAG_HIDDEN);
    if (call_entry) {
        // A dark wedge cut on the same slant as the home page's blocks.
        lv_obj_add_flag(action, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(action);
        lv_obj_add_event_cb(action, [](lv_event_t* e) {
            lv_area_t a;
            lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
            lv_draw_triangle_dsc_t fill;
            lv_draw_triangle_dsc_init(&fill);
            fill.color = lv_color_black();
            fill.opa = LV_OPA_COVER;
            auto P = [](int x, int y) { return lv_point_precise_t{static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)}; };
            // 斜边用一个三角形，其余用矩形盖住：两个三角形沿对角线拼，接缝的抗锯齿会留一道白线。
            fill.p[0] = P(a.x1 + 26, a.y1); fill.p[1] = P(a.x1 + 30, a.y2); fill.p[2] = P(a.x1, a.y2);
            lv_draw_triangle(lv_event_get_layer(e), &fill);
            lv_draw_rect_dsc_t rest;
            lv_draw_rect_dsc_init(&rest);
            rest.bg_color = lv_color_black();
            rest.bg_opa = LV_OPA_COVER;
            lv_area_t body = {a.x1 + 26, a.y1, a.x2, a.y2};
            lv_draw_rect(lv_event_get_layer(e), &rest, &body);
        }, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_add_event_cb(action, ChangeTalkMode, LV_EVENT_CLICKED, nullptr);
        lv_obj_align(ValetIconBox(action, kIconWave | kIconWhite, 36), LV_ALIGN_LEFT_MID, 44, 0);
        auto* call_label = lv_label_create(action);
        lv_label_set_text(call_label, ValetText("通话", "Call"));
        lv_obj_set_style_text_font(call_label, ValetStrong(), 0);
        lv_obj_set_style_text_color(call_label, lv_color_white(), 0);
        lv_obj_align(call_label, LV_ALIGN_LEFT_MID, 90, 0);
        lv_obj_clear_flag(call_label, LV_OBJ_FLAG_CLICKABLE);
    }
    // Pager above the bar: previous, "n / m", next. Only shown while there is an answer.
    auto* pager = ValetBox(scr, 0, 0, 340, kValetPagerH);
    lv_obj_align(pager, LV_ALIGN_BOTTOM_MID, 0, -24 - kValetBarH - 4);
    for (int i = 0; i < 2; ++i) {
        auto* turn = ValetBox(pager, 0, 0, 96, kValetPagerH);
        lv_obj_align(turn, i ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(turn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(turn);
        lv_obj_add_event_cb(turn, ValetTurn, LV_EVENT_CLICKED, i ? reinterpret_cast<void*>(1) : nullptr);
        lv_obj_center(ValetIconBox(turn, i ? kIconNext : kIconPrev, 32));
    }
    lv_obj_add_flag(pager, LV_OBJ_FLAG_HIDDEN);
    Assistant_State().chrome.valet_pager = pager;
    auto* footer = lv_label_create(pager);
    lv_obj_set_width(footer, 148);
    lv_obj_center(footer);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, ValetSmall(), 0);
    lv_label_set_text(footer, "");
    lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);

    Assistant_State().chrome.scr = scr;
    Assistant_State().chrome.content = content;
    Assistant_State().chrome.page_label = footer;
    Assistant_State().chrome.photo_action = action;
    Assistant_State().chrome.photo_action_label = action_label;
    Assistant_State().chrome.idle_hint = idle_hint;
    Assistant_State().chrome.status_label = status.status_label;
    Assistant_State().chrome.notification_label = status.notification_label;
    Assistant_State().ptt.ptt_wave = ptt_wave;
    if (custom_live) lv_obj_add_flag(touch_ptt_hit, LV_OBJ_FLAG_HIDDEN);
    Assistant_State().ptt.touch_ptt_hit = touch_ptt_hit;
    Assistant_State().ptt.ptt_wave_visible = false;
    if (camera_available) AssistantPhoto_Create(scr);
    Assistant_ResetTouchPttState();
    ScreenPaintCoalesceReset(&Assistant_State().stream.page_paint);
    Assistant_State().stream.rendered_page_json.clear();
    const bool restore_session = !Assistant_State().layout.flow.empty();
    if (!restore_session) {
        Assistant_State().layout.pages.clear();
        Assistant_State().layout.flow_first_page.clear();
        Assistant_State().layout.page_flow_begin.clear();
        Assistant_State().layout.open_page_flow_begin = static_cast<size_t>(-1);
        Assistant_State().layout.page_index = 0;
        Assistant_State().layout.pin_flow_idx = -1;
        Assistant_State().layout.has_a2ui_content = false;
    }

    Assistant_EnsureA2ui(scr);
    a2ui_set_host(content);
    if (restore_session) {
        ESP_LOGD(TAG, "restore session flow=%u pages=%u idx=%d",
                 static_cast<unsigned>(Assistant_State().layout.flow.size()), static_cast<unsigned>(Assistant_State().layout.pages.size()),
                 Assistant_State().layout.page_index);
        Assistant_RebuildPages();
        Assistant_TrimFlowToMaxPages();
        if (Assistant_State().layout.page_index >= static_cast<int>(Assistant_State().layout.pages.size())) {
            Assistant_State().layout.page_index = static_cast<int>(Assistant_State().layout.pages.size()) - 1;
        }
        if (Assistant_State().layout.page_index < 0) {
            Assistant_State().layout.page_index = 0;
        }
        if (!Assistant_State().layout.pages.empty()) {
            Assistant_State().layout.has_a2ui_content = true;
            Assistant_HideIdleHint();
            Assistant_RenderCurrentPage();
        } else {
            Assistant_ShowIdleHint();
        }
    } else {
        if (!chat_cloud::IsCustom()) Assistant_ReplayHistoryFromStore();
        if (Assistant_State().layout.has_a2ui_content || !Assistant_State().layout.flow.empty()) {
            Assistant_HideIdleHint();
        } else {
            Assistant_ShowIdleHint();
        }
    }
    if (panel) {
        if (Assistant_State().chrome.panel_title) lv_obj_move_foreground(lv_obj_get_parent(Assistant_State().chrome.panel_title));
        FirmwareMemoryCheckpoint("assistant-panel-ready");
        return scr;   // 悬窗：按键归阅读页（它转给悬窗），进入 / 离开由悬窗自己管
    }
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->SetStatusTitlePrefix(nullptr);
        disp->SetIdleStatusMode(IdleStatusMode::kClock);
        disp->UpdateStatusBar(true);
    }

    if (context_header) lv_obj_move_foreground(context_header);
    ScreenSetIsHome(false);
    {
        VkKeyScreenDesc desc{};
        desc.factory = AssistantScreen::Create;
        desc.on_key = Assistant_OnVkKey;
        desc.on_boot_click = Assistant_OnBootClick;
        desc.on_boot_long_press = Assistant_OnBootLongPress;
        desc.on_boot_press_down = Assistant_OnBootPressDown;
        desc.on_boot_press_up = Assistant_OnBootPressUp;
        desc.on_key_long_press = Assistant_OnVkKeyLongPress;
        desc.on_key_press_up = Assistant_OnVkKeyPressUp;
        desc.on_boot_double_click = Assistant_OnBootDoubleClick;
        VkKey_AttachScreen(scr, "assistant", desc);
    }
    lv_obj_add_event_cb(scr, Assistant_OnAssistantLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, Assistant_OnAssistantLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    if (auto* binding = YunxinBinding::CreatePanel(scr)) {
        lv_obj_set_size(binding, LV_HOR_RES, body_h);
        lv_obj_set_pos(binding, 0, header_h);
    }
    FirmwareMemoryCheckpoint("assistant-screen-ready");
    return scr;
}

void Assistant_UpdatePhotoAction() {
    auto& state = Assistant_State();
    if (!state.chrome.photo_action) return;
    const bool pending = AssistantPhoto_HasPending();
    lv_obj_set_style_bg_color(state.chrome.photo_action, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(state.chrome.photo_action, pending ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (pending) {
        lv_obj_add_flag(state.chrome.photo_action, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(state.chrome.photo_action_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(state.chrome.photo_action, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(state.chrome.photo_action_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(state.chrome.photo_action);
}

bool Assistant_AllowListening() {
    if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) {
        ScreenShowToast(hint);
        return false;
    }
    return AssistantPhoto_AllowListening();
}

bool AssistantScreen::BeginPhotoListening(uint32_t turn) {
    return ScreenLvAsync([](void* value) {
        if (AssistantScreen::IsActive()) AssistantPhoto_BeginListening(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value)));
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(turn)));
}

bool AssistantScreen::EndPhotoListening(uint32_t turn) {
    return ScreenLvAsync([](void* value) {
        if (AssistantScreen::IsActive()) AssistantPhoto_EndListening(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value)));
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(turn)));
}

bool AssistantScreen::UploadPhotoQuestion(uint32_t turn, bool ready) {
    struct Result {
        uint32_t turn; // 提问轮次。
        bool ready; // WebSocket 会话是否就绪。
    };
    auto* result = new (std::nothrow) Result{turn, ready};
    if (!result) {
        ESP_LOGE(TAG, "photo readiness failed stage=allocate");
        return false;
    }
    const auto apply = [](void* context) {
        std::unique_ptr<Result> result(static_cast<Result*>(context));
        AssistantPhoto_UploadQuestion(result->turn, result->ready);
    };
    if (ScreenLvAsync(apply, result) || ScreenLvAsyncUrgent(apply, result)) return true;
    delete result;
    ESP_LOGE(TAG, "photo readiness failed stage=dispatch");
    return false;
}

void AssistantScreen::PhotoQuestionError(const std::string& image_id, bool reupload, bool disabled) {
    struct Error {
        std::string image_id; // 失败问题关联的图片编号，仅用于匹配，不写入日志。
        bool reupload; // 是否需要重新上传原图。
        bool disabled; // 服务端是否未开启识图。
    };
    Error* error = nullptr;
    try { error = new Error{image_id, reupload, disabled}; }
    catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "photo error delivery failed stage=allocate");
        return;
    }
    const auto apply = [](void* context) {
        std::unique_ptr<Error> error(static_cast<Error*>(context));
        if (AssistantScreen::IsActive()) AssistantPhoto_QuestionError(error->image_id, error->reupload, error->disabled);
    };
    if (!ScreenLvAsync(apply, error) && !ScreenLvAsyncUrgent(apply, error)) {
        delete error;
        ESP_LOGE(TAG, "photo error delivery failed stage=dispatch");
    }
}

void AssistantScreen::FinishPhotoQuestion() {
    const auto apply = [](void*) { AssistantPhoto_FinishQuestion(); };
    if (!ScreenLvAsync(apply) && !ScreenLvAsyncUrgent(apply)) {
        ESP_LOGE(TAG, "photo release failed stage=dispatch");
    }
}
