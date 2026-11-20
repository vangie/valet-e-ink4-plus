#include "settings_screen.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "settings_about_tab.h"
#include "settings_bluetooth_tab.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "settings_pager_tab.h"
#include "screen_text.h"
#include "settings_common.h"
#include "settings_conversation_tab.h"
#include "settings_frontlight_tab.h"
#include "settings_haptic_tab.h"
#include "settings_language_tab.h"
#include "settings_network_tab.h"
#include "settings_power_tab.h"
#include "settings_charging_tab.h"
#include "assets/lang_charging.h"
#include "settings_test/settings_test_tab.h"
#include "settings_theme_tab.h"
#include "settings_log_tab.h"
#include "settings_log_upload.h"
#include "vk_key_handler.h"
#include "book_screen/book_screen.h"
#include "home_screen/home_screen.h"
#include "chat_cloud.h"
#include "board.h"
#include "audio_codec.h"
#include "frontlight.h"
#include "application.h"
#include "valet_setup/server.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <esp_app_desc.h>
#include "SdCardManager.hpp"
#include "metalio_power.h"
#include "power_policy.h"
#include <algorithm>

#define TAG "SettingsScreen"
static constexpr const char* kScreenId = "settings";

enum class SettingsTab : int {
    kNetwork = 0,
    kTheme = 1,
    kLanguage = 2,
    kHaptic = 3,
    kPower = 4,
    kFrontlight = 5,
    kConversation = 6,
    kBluetooth = 7,
    kPager = 12, // BLE 翻页器
    kTest = 8,
    kLog = 9,
    kCharging = 11, // 充电电流配置
    kAbout = 10, // 关于始终放最后
};

struct SettingsTabEntry {
    SettingsTab id;
    const char* (*name)();
};

static const char* TabNameNetwork() {
    return Lang::Strings::SETTINGS_TAB_NETWORK;
}
static const char* TabNameTheme() {
    return Lang::Strings::SETTINGS_TAB_THEME;
}
static const char* TabNameLanguage() {
    return Lang::Strings::SETTINGS_TAB_LANGUAGE;
}
static const char* TabNameHaptic() {
    return Lang::Strings::SETTINGS_TAB_HAPTIC;
}
static const char* TabNameCharging() { return Lang::Strings::SETTINGS_TAB_CHARGING; }
static const char* TabNamePower() {
    return Lang::Strings::SETTINGS_TAB_POWER;
}
static const char* TabNameFrontlight() {
    return Lang::Strings::SETTINGS_TAB_FRONTLIGHT;
}
static const char* TabNameConversation() {
    return Lang::Strings::SETTINGS_TAB_CONVERSATION;
}
static const char* TabNameBluetooth() {
    return Lang::Strings::SETTINGS_TAB_BLUETOOTH;
}
static const char* TabNamePager() { return Lang::Strings::SETTINGS_PAGER; }
static const char* TabNameTest() {
    return Lang::Strings::SETTINGS_TAB_TEST;
}
static const char* TabNameLog() { return Lang::Strings::SETTINGS_TAB_LOG; }
static const char* TabNameAbout() {
    return Lang::Strings::SETTINGS_TAB_ABOUT;
}

static constexpr SettingsTabEntry kTabs[] = {
    {SettingsTab::kNetwork, TabNameNetwork},
    {SettingsTab::kTheme, TabNameTheme},
    {SettingsTab::kLanguage, TabNameLanguage},
    {SettingsTab::kHaptic, TabNameHaptic},
    {SettingsTab::kPower, TabNamePower},
    {SettingsTab::kCharging, TabNameCharging},
    {SettingsTab::kFrontlight, TabNameFrontlight},
    {SettingsTab::kConversation, TabNameConversation},
    {SettingsTab::kBluetooth, TabNameBluetooth},
    {SettingsTab::kPager, TabNamePager},
    {SettingsTab::kTest, TabNameTest},
    {SettingsTab::kLog, TabNameLog},
    {SettingsTab::kAbout, TabNameAbout},
};
static constexpr int kTabCount = sizeof(kTabs) / sizeof(kTabs[0]);
static_assert(kTabs[kTabCount - 1].id == SettingsTab::kAbout, "about tab must be last");

struct SettingsShellUi {
    lv_obj_t* tab_btns[kTabCount] = {};
    lv_obj_t* content_page = nullptr;
    int active_tab = -1;
    bool content_built = false;
    lv_obj_t* heading = nullptr;
    int group = -1;
    bool direct = false; // the open page is the whole group, with no list in between
};

static SettingsShellUi s_ui;
static lv_obj_t* s_status_battery = nullptr; // the status bar's battery text, kept current by the display
static SettingsTab s_pending_tab = SettingsTab::kNetwork;
static bool s_open_overview = true;
static lv_obj_t* s_settings_scr = nullptr;
static uint32_t s_settings_generation = 0;

static int TabIndexOf(SettingsTab id) {
    for (int i = 0; i < kTabCount; ++i) {
        if (kTabs[i].id == id) {
            return i;
        }
    }
    return 0;
}

static ScreenFactory FactoryForTab(SettingsTab id) {
    if (id == SettingsTab::kPager) return SettingsScreen::CreatePager;
    if (id == SettingsTab::kBluetooth) return SettingsScreen::CreateBluetooth;
    if (id == SettingsTab::kTest) {
        return SettingsScreen::CreateTest;
    }
    return SettingsScreen::Create;
}

static void ResetTab(SettingsTab id) {
    switch (id) {
        case SettingsTab::kNetwork:
            SettingsNetworkTab_Reset();
            break;
        case SettingsTab::kTheme:
            SettingsThemeTab_Reset();
            break;
        case SettingsTab::kLanguage:
            SettingsLanguageTab_Reset();
            break;
        case SettingsTab::kHaptic:
            SettingsHapticTab_Reset();
            break;
        case SettingsTab::kCharging:
            SettingsChargingTab_Reset();
            break;
        case SettingsTab::kPower:
            SettingsPowerTab_Reset();
            break;
        case SettingsTab::kFrontlight:
            SettingsFrontlightTab_Reset();
            break;
        case SettingsTab::kConversation:
            SettingsConversationTab_Reset();
            break;
        case SettingsTab::kBluetooth:
            SettingsBluetoothTab_Reset();
            break;
        case SettingsTab::kPager:
            SettingsPagerTab_Reset();
            break;
        case SettingsTab::kTest:
            break;
        case SettingsTab::kLog:
            SettingsLogTab_Reset();
            break;
        case SettingsTab::kAbout:
            SettingsAboutTab_Reset();
            break;
    }
}

static void BuildTab(SettingsTab id) {
    lv_obj_t* page = s_ui.content_page;
    if (page == nullptr) {
        return;
    }
    switch (id) {
        case SettingsTab::kNetwork:
            SettingsNetworkTab_Build(page);
            break;
        case SettingsTab::kTheme:
            SettingsThemeTab_Build(page);
            break;
        case SettingsTab::kLanguage:
            SettingsLanguageTab_Build(page);
            break;
        case SettingsTab::kHaptic:
            SettingsHapticTab_Build(page);
            break;
        case SettingsTab::kCharging:
            SettingsChargingTab_Build(page);
            break;
        case SettingsTab::kPower:
            SettingsPowerTab_Build(page);
            break;
        case SettingsTab::kFrontlight:
            SettingsFrontlightTab_Build(page);
            break;
        case SettingsTab::kConversation:
            SettingsConversationTab_Build(page);
            break;
        case SettingsTab::kBluetooth:
            SettingsBluetoothTab_Build(page);
            break;
        case SettingsTab::kPager:
            SettingsPagerTab_Build(page);
            break;
        case SettingsTab::kTest:
            SettingsTestTab_Build(page);
            break;
        case SettingsTab::kLog:
            SettingsLogTab_Build(page);
            break;
        case SettingsTab::kAbout:
            SettingsAboutTab_Build(page);
            break;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(page); ++i) {
        auto* child = lv_obj_get_child(page, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_width(child, LV_PCT(100));
            lv_label_set_long_mode(child, LV_LABEL_LONG_WRAP);
        }
    }
    s_ui.content_built = true;
}

static void TearDownActiveTab() {
    if (s_ui.content_page == nullptr || !s_ui.content_built || s_ui.active_tab < 0 ||
        s_ui.active_tab >= kTabCount) {
        return;
    }
    const SettingsTab id = kTabs[s_ui.active_tab].id;
    if (id == SettingsTab::kBluetooth) {
        SettingsBluetoothTab_OnDeactivated();
    }
    ResetTab(id);
    lv_obj_clean(s_ui.content_page);
    s_ui.content_built = false;
}

static void ShowTab(int index) {
    if (index < 0 || index >= kTabCount) {
        return;
    }
    if (s_ui.content_built && s_ui.active_tab == index) {
        return;
    }

    TearDownActiveTab();
    lv_obj_clean(s_ui.content_page);
    lv_obj_set_style_pad_row(s_ui.content_page, kSettingsOptionGap, 0);
    s_ui.active_tab = index;

    for (int i = 0; i < kTabCount; ++i) {
        if (s_ui.tab_btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.tab_btns[i], lv_obj_get_child(s_ui.tab_btns[i], 0),
                                    i == index);
        }
    }

    const SettingsTab id = kTabs[index].id;
    BuildTab(id);
    if (id == SettingsTab::kConversation) {
        SettingsConversationTab_OnActivated();
    }
    if (id == SettingsTab::kBluetooth) {
        SettingsBluetoothTab_OnActivated();
    }
    VkKey_SetScreenFactory(kScreenId, FactoryForTab(id));
}

// 页面内下钻保留同一个 Screen，旧页签的建页/销毁逻辑继续负责各自资源。
struct GroupEntry { const char* zh; const char* en; };
static constexpr GroupEntry kGroups[] = {
    {"助手", "Assistant"}, {"网络", "Network"}, {"显示", "Display"},
    {"阅读与字库", "Reading & fonts"}, {"声音与蓝牙", "Sound & Bluetooth"}, {"设备", "Device"},
};
static const char* Text(const char* zh, const char* en) {
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
}
static void ShowOverview();
static void ShowGroup(int group);
// Design sizes are panel pixels measured from sim/ (新设计 → 设置).
static void DrawLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, int width) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = width;
    line.opa = LV_OPA_COVER;
    line.round_start = line.round_end = 1;
    line.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    line.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    lv_draw_line(layer, &line);
}
// Line icons on the design's 24-unit grid; the UI font pack has no symbol glyphs.
enum Glyph : intptr_t { kGlyphBack, kGlyphGo, kGlyphSun, kGlyphSpeaker, kGlyphWave, kGlyphNet, kGlyphBook, kGlyphDevice };
static void DrawGlyph(lv_event_t* e) {
    auto* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    const int size = a.x2 - a.x1 + 1;
    const int w = size >= 44 ? 4 : 3;
    auto X = [&](int tenths) { return a.x1 + tenths * size / 240; };
    auto Y = [&](int tenths) { return a.y1 + tenths * size / 240; };
    auto L = [&](int x1, int y1, int x2, int y2) { DrawLine(layer, X(x1), Y(y1), X(x2), Y(y2), w); };
    auto Ring = [&](int cx, int cy, int r, int from, int to) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_black();
        arc.width = w;
        arc.opa = LV_OPA_COVER;
        arc.rounded = 1;
        arc.center = {static_cast<lv_value_precise_t>(X(cx)), static_cast<lv_value_precise_t>(Y(cy))};
        arc.radius = r * size / 240 + w / 2;
        arc.start_angle = from;
        arc.end_angle = to;
        lv_draw_arc(layer, &arc);
    };
    switch (reinterpret_cast<intptr_t>(lv_event_get_user_data(e))) {
        case kGlyphBack: L(145, 50, 75, 120); L(75, 120, 145, 190); break;
        case kGlyphGo: L(95, 50, 165, 120); L(165, 120, 95, 190); break;
        case kGlyphSun:
            Ring(120, 120, 36, 0, 360);
            L(120, 28, 120, 52); L(120, 188, 120, 212); L(28, 120, 52, 120); L(188, 120, 212, 120);
            L(55, 55, 72, 72); L(168, 168, 185, 185); L(55, 185, 72, 168); L(168, 72, 185, 55);
            break;
        case kGlyphSpeaker:
            L(40, 95, 40, 145); L(40, 145, 75, 145); L(75, 145, 125, 185); L(125, 185, 125, 55);
            L(125, 55, 75, 95); L(75, 95, 40, 95);
            Ring(130, 120, 42, 315, 45); Ring(130, 120, 78, 320, 40);
            break;
        case kGlyphWave: L(35, 100, 35, 140); L(78, 65, 78, 175); L(120, 30, 120, 210); L(163, 65, 163, 175); L(205, 100, 205, 140); break;
        case kGlyphNet: L(45, 200, 45, 170); L(95, 200, 95, 130); L(145, 200, 145, 90); L(195, 200, 195, 40); break;
        case kGlyphBook:
            L(120, 65, 120, 205); L(120, 65, 75, 45); L(75, 45, 30, 45); L(30, 45, 30, 185); L(30, 185, 75, 185);
            L(75, 185, 120, 205); L(120, 65, 165, 45); L(165, 45, 210, 45); L(210, 45, 210, 185);
            L(210, 185, 165, 185); L(165, 185, 120, 205);
            break;
        case kGlyphDevice: L(65, 25, 175, 25); L(175, 25, 175, 215); L(175, 215, 65, 215); L(65, 215, 65, 25); L(100, 180, 140, 180); break;
    }
}
static lv_obj_t* AddGlyph(lv_obj_t* parent, Glyph glyph, int size) {
    auto* icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, size, size);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(icon, DrawGlyph, LV_EVENT_DRAW_MAIN, reinterpret_cast<void*>(static_cast<intptr_t>(glyph)));
    return icon;
}
static const lv_font_t* SmallFont() {
    const auto* font = fontpack_lv_font_get(25, 2);
    return font ? font : fontpack_lv_font_ui();
}
static const lv_font_t* StrongFont() {
    const auto* font = fontpack_lv_font_get(30, 4);
    return font ? font : fontpack_lv_font_ui();
}

// Front light and volume: ten blocks each. Tap a block to set that level; tap
// the block that is already the level to go one lower, so zero is reachable.
static void SetQuick(lv_event_t* event) {
    const intptr_t data = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    const bool light = data < 100;
    const int block = static_cast<int>(data % 100);
    const uint32_t generation = s_settings_generation;
    if (!Application::GetInstance().Schedule([light, block, generation]() {
        if (light) {
            auto& front = Frontlight::GetInstance();
            const int now = (front.target_brightness() + 5) / 10;
            front.SetBrightness((now == block ? block - 1 : block) * 10, true);
        } else if (auto* codec = Board::GetInstance().GetAudioCodec()) {
            const int now = (codec->output_volume() + 5) / 10;
            codec->SetOutputVolume((now == block ? block - 1 : block) * 10);
        }
        auto* expected = new uint32_t(generation);
        if (!ScreenLvAsync([](void* context) {
            const uint32_t expected = *static_cast<uint32_t*>(context);
            delete static_cast<uint32_t*>(context);
            if (!s_settings_scr || expected != s_settings_generation || s_ui.content_built) return;
            if (s_ui.group == -1) ShowOverview();
            else if (s_ui.group == 4) ShowGroup(4);
        }, expected)) delete expected;
    }, Application::TaskDomain::Storage)) {
        ScreenShowToast(Text("操作繁忙，请重试", "Busy. Try again."));
    }
}
static void AddLevel(lv_obj_t* box, Glyph glyph, const char* name, int value, bool light) {
    auto* row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 72);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(AddGlyph(row, glyph, 36), LV_ALIGN_LEFT_MID, 0, 0);
    auto* label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, SmallFont(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 52, 0);
    const int filled = (std::clamp(value, 0, 100) + 5) / 10;
    // Without a name the blocks take the room the name would have used.
    const bool named = name && *name;
    const int kBlockW = named ? 32 : 43, kBlockGap = named ? 5 : 6, kBlocksX = named ? 130 : 58;
    for (int i = 0; i < 10; ++i) {
        // The touch target is the full row height; the drawn block is the centre of it.
        auto* hit = lv_obj_create(row);
        lv_obj_remove_style_all(hit);
        lv_obj_set_size(hit, kBlockW + kBlockGap, 72);
        lv_obj_set_pos(hit, kBlocksX + i * (kBlockW + kBlockGap), 0);
        lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, SetQuick, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>((light ? 0 : 100) + i + 1)));
        auto* block = lv_obj_create(hit);
        lv_obj_remove_style_all(block);
        lv_obj_set_size(block, kBlockW, 38);
        lv_obj_align(block, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_border_width(block, 2, 0);
        lv_obj_set_style_border_color(block, lv_color_black(), 0);
        lv_obj_set_style_bg_color(block, i < filled ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
        lv_obj_clear_flag(block, LV_OBJ_FLAG_CLICKABLE);
    }
    auto* number = lv_label_create(row);
    lv_label_set_text_fmt(number, "%d", value);
    lv_obj_set_style_text_font(number, SmallFont(), 0);
    lv_obj_align(number, LV_ALIGN_RIGHT_MID, 0, 0);
}
static lv_obj_t* AddQuickBox() {
    auto* box = lv_obj_create(s_ui.content_page);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(box, 22, 0);
    lv_obj_set_style_pad_ver(box, 7, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}
static void OpenTab(lv_event_t* e) {
    ShowTab(TabIndexOf(static_cast<SettingsTab>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))));
    lv_label_set_text(s_ui.heading, kTabs[s_ui.active_tab].name());
    lv_obj_scroll_to_y(s_ui.content_page, 0, LV_ANIM_OFF);
}
static void OpenGroup(lv_event_t* e) {
    ShowGroup(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}
static void OpenReading(lv_event_t*) { ScreenNavigateTo(BookScreen::Create); }
static void AddRow(const char* title, const char* detail, lv_event_cb_t callback, intptr_t data,
                   int glyph = -1) {
    const bool two = detail && *detail;
    auto* row = lv_obj_create(s_ui.content_page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), glyph >= 0 ? 134 : two ? 112 : 84);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, reinterpret_cast<void*>(data));
    int left = 4;
    if (glyph >= 0) {
        lv_obj_align(AddGlyph(row, static_cast<Glyph>(glyph), 50), LV_ALIGN_LEFT_MID, 6, 0);
        left = 84;
    }
    auto* name = lv_label_create(row);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_font(name, StrongFont(), 0);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_pad_right(name, left + 48, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, left, two ? -20 : 0);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
    if (two) {
        auto* sub = lv_label_create(row);
        lv_label_set_text(sub, detail);
        lv_obj_set_style_text_font(sub, SmallFont(), 0);
        lv_obj_set_width(sub, lv_pct(100));
        lv_obj_set_style_pad_right(sub, left + 48, 0);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, left, 22);
        lv_obj_clear_flag(sub, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_align(AddGlyph(row, kGlyphGo, 26), LV_ALIGN_RIGHT_MID, -2, 0);
}
static void AddTab(SettingsTab tab, const char* detail = nullptr) {
    AddRow(kTabs[TabIndexOf(tab)].name(), detail, OpenTab, static_cast<intptr_t>(tab));
}
static void ClearContent() {
    TearDownActiveTab();
    s_ui.direct = false;
    // 分组列表自身也有控件（不归旧页签生命周期管理）。
    lv_obj_clean(s_ui.content_page);
    lv_obj_set_style_pad_row(s_ui.content_page, 0, 0);
    s_ui.active_tab = -1;
    lv_obj_scroll_to_y(s_ui.content_page, 0, LV_ANIM_OFF);
    VkKey_SetScreenFactory(kScreenId, SettingsScreen::Create);
}
static void ShowGroup(int group) {
    if (group < 0 || group >= 6) return;
    ClearContent();
    s_ui.group = group;
    lv_label_set_text(s_ui.heading, Text(kGroups[group].zh, kGroups[group].en));
    switch (group) {
        case 0:
            ShowTab(TabIndexOf(SettingsTab::kConversation));
            lv_obj_scroll_to_y(s_ui.content_page, 0, LV_ANIM_OFF);
            s_ui.direct = true;
            break;
        case 1:
            // One page in this group: open it directly, with the group's name as the title.
            ShowTab(TabIndexOf(SettingsTab::kNetwork));
            lv_obj_scroll_to_y(s_ui.content_page, 0, LV_ANIM_OFF);
            s_ui.direct = true;
            break;
        case 2:
            // Front light, refresh and the app grid style are one page now.
            ShowTab(TabIndexOf(SettingsTab::kFrontlight));
            lv_obj_scroll_to_y(s_ui.content_page, 0, LV_ANIM_OFF);
            s_ui.direct = true;
            break;
        case 3: {
            auto* note = lv_label_create(s_ui.content_page);
            lv_label_set_text(note, Text("字号、字体、边距与行距在阅读中的排版菜单设置。扩展字库放入 SD 卡 metalio/e-ink/fonts/。", "Adjust font, size and spacing in the reader. Extended fonts belong in metalio/e-ink/fonts/ on the SD card."));
            lv_obj_set_width(note, lv_pct(100));
            lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
            AddRow(Text("打开阅读", "Open reading"), nullptr, OpenReading, 0);
            break;
        }
        case 4: {
            // Volume and key haptics on the page itself; Bluetooth
            // and the page turner have screens of their own behind two rows.
            SettingsValetLabel(s_ui.content_page, Text("音量", "Volume"));
            if (auto* codec = Board::GetInstance().GetAudioCodec()) {
                auto* box = lv_obj_create(s_ui.content_page);
                lv_obj_remove_style_all(box);
                lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
                lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
                AddLevel(box, kGlyphSpeaker, "", codec->output_volume(), false);
            }
            // Whether replies are spoken is set on the assistant page: for the official
            // service it is a server-side preference with its own request, not a local switch.
            SettingsValetGap(s_ui.content_page);
            SettingsHapticTab_BuildValet(s_ui.content_page);
            SettingsValetGap(s_ui.content_page);
            AddTab(SettingsTab::kBluetooth, Text("耳机、音箱、键盘与语音", "Headset, speaker, keys and voice"));
            AddTab(SettingsTab::kPager, Text("正文里加 / 减音量键也能翻页", "Volume keys also turn pages"));
            break;
        }
        case 5: {
            // What the device is and how it is doing, then one row per thing to manage,
            // each showing its current value.
            char detail[96];
            const char* battery = "--";
            if (s_status_battery && lv_obj_is_valid(s_status_battery)) battery = lv_label_get_text(s_status_battery);
            const bool card = SdCardManager::GetInstance().IsMounted();
            std::snprintf(detail, sizeof(detail), "%s %s · %s", Text("电量", "Battery"), battery,
                          card ? Text("SD 卡已插入", "SD card in") : Text("没有 SD 卡", "No SD card"));
            SettingsValetCard(s_ui.content_page, "Metalio E-Ink4 Plus", detail, true, true);
            SettingsValetGap(s_ui.content_page, 26);
            auto* rule = lv_obj_create(s_ui.content_page);
            lv_obj_remove_style_all(rule);
            lv_obj_set_size(rule, lv_pct(100), 3);
            lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
            const auto& power = PowerPolicy::GetInstance();
            char line[96];
            std::snprintf(line, sizeof(line), Text("空闲时 %d MHz · 5 分钟待机，30 分钟关机", "%d MHz idle · standby 5 min, off 30 min"),
                          power.GetIdleCpuMhz());
            AddTab(SettingsTab::kPower, line);
            std::snprintf(line, sizeof(line), Text("充电电流 %d mA", "Charging current %d mA"), MetalioPower_GetChargeCurrentMa());
            AddTab(SettingsTab::kCharging, line);
            AddTab(SettingsTab::kLanguage, std::strcmp(Lang::Current(), "zh-CN") == 0 ? "简体中文" : "English");
            AddTab(SettingsTab::kLog, Text("记录并上传运行日志", "Record and upload logs"));
            AddTab(SettingsTab::kTest, Text("屏幕、触摸、电池、声音", "Screen, touch, battery, audio"));
            const auto* app = esp_app_get_description();
            std::snprintf(line, sizeof(line), "Valet %s", app ? app->version : "");
            AddTab(SettingsTab::kAbout, line);
            break;
        }
    }
}
static void ShowOverview() {
    ClearContent();
    s_ui.group = -1;
    lv_label_set_text(s_ui.heading, Text("设置", "Settings"));
    auto* quick = AddQuickBox();
    AddLevel(quick, kGlyphSun, Text("前光", "Light"), Frontlight::GetInstance().target_brightness(), true);
    if (auto* codec = Board::GetInstance().GetAudioCodec())
        AddLevel(quick, kGlyphSpeaker, Text("音量", "Volume"), codec->output_volume(), false);
    // A rule over the first row, as in the design.
    auto* rule = lv_obj_create(s_ui.content_page);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, lv_pct(100), 3);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(rule, 16, 0);
    const auto provider = chat_cloud::ActiveProvider();
    const char* service = provider == chat_cloud::Provider::Custom ? Text("自建", "Custom") :
        provider == chat_cloud::Provider::Baiwen ? Text("百问", "Baiwen") :
        provider == chat_cloud::Provider::Xiaozhi ? Text("小智", "Xiaozhi") : Text("网易", "NetEase");
    const char* descriptions[] = {
        service, SettingsNetworkTab_CurrentName(), Text("前光、刷新与主题", "Light, refresh and theme"),
        Text("字体、排版与缺字", "Fonts, layout and missing glyphs"),
        Text("声音输出、耳机与翻页器", "Audio, headphones and page turners"),
        Text("语言、电量与设备管理", "Language, power and device management"),
    };
    static constexpr Glyph kGroupGlyphs[] = {kGlyphWave, kGlyphNet, kGlyphSun, kGlyphBook, kGlyphSpeaker, kGlyphDevice};
    for (int i = 0; i < 6; ++i) AddRow(Text(kGroups[i].zh, kGroups[i].en), descriptions[i], OpenGroup, i, kGroupGlyphs[i]);
}
static bool NavigateUp() {
    // A page opened directly from the overview goes back to the overview.
    if (s_ui.content_built && s_ui.direct) { ShowOverview(); return true; }
    if (s_ui.content_built) { ShowGroup(s_ui.group < 0 ? 5 : s_ui.group); return true; }
    if (s_ui.group >= 0) { ShowOverview(); return true; }
    return false;
}
static void BackClicked(lv_event_t*) {
    if (!NavigateUp()) ScreenRequestBack();
}

static void OnScreenDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_settings_scr) {
        return;
    }
    SettingsLogUpload_Teardown();
    TearDownActiveTab();
    s_ui = {};
    s_settings_scr = nullptr;
    s_status_battery = nullptr;
}

static bool OnSettingsKey(const char* key) {
    if (key && strcmp(key, "vk_home") == 0 && BluetoothScreen::DismissOverlay()) return true;
    if (SettingsLogUpload_OnVkKey(key)) return true;
    if (!key) return false;
    if (std::strcmp(key, "vk_home") == 0) return NavigateUp();
    if (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_next") == 0) {
        auto* page = s_ui.content_page;
        const bool previous = std::strcmp(key, "vk_prev") == 0;
        const int y = lv_obj_get_scroll_y(page);
        if (previous && y == 0) return NavigateUp();
        // Scroll only as far as there is content: an unclamped step pushed a
        // list that already fit the page off the top, leaving it blank.
        lv_obj_update_layout(page);
        const int room = previous ? y : lv_obj_get_scroll_bottom(page);
        const int delta = std::min<int>(lv_obj_get_height(page) * 3 / 4, std::max(room, 0));
        if (delta > 0) lv_obj_scroll_by(page, 0, previous ? delta : -delta, LV_ANIM_OFF);
        return true;
    }
    return false;
}
static bool OnSettingsLongKey(const char* key) {
    if (key && strcmp(key, "vk_home") == 0 && BluetoothScreen::DismissOverlay()) return true;
    if (!SettingsLogUpload_IsActive()) return false;
    SettingsLogUpload_Teardown();
    if (key && std::strcmp(key, "vk_home") == 0) ScreenRequestHome();
    return true;
}

static lv_obj_t* CreateInternal() {
    ++s_settings_generation;
    if (s_settings_scr) {
        SettingsLogUpload_Teardown();
        TearDownActiveTab();
    }
    s_ui = {};

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_settings_scr = scr;
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, OnScreenDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    s_status_battery = status.battery_pct_label;
    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kSettingsBodyPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, kSettingsBodyPad, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 0, 0);
    auto* header = lv_obj_create(body);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 94);
    lv_obj_set_style_border_width(header, 3, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_black(), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    auto* back = lv_obj_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 96, 90);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(back);
    lv_obj_add_event_cb(back, BackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_align(AddGlyph(back, kGlyphBack, 44), LV_ALIGN_LEFT_MID, 28, 0);
    s_ui.heading = lv_label_create(header);
    lv_obj_set_style_text_font(s_ui.heading, StrongFont(), 0);
    lv_obj_set_width(s_ui.heading, LV_HOR_RES - 140);
    lv_label_set_long_mode(s_ui.heading, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ui.heading, LV_ALIGN_LEFT_MID, 96, 0);

    lv_obj_t* content = lv_obj_create(body);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(content, 33, 0);
    lv_obj_set_style_pad_top(content, 30, 0);
    lv_obj_set_style_pad_bottom(content, 16, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 0, 0);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    s_ui.content_page = content;

    const SettingsTab open_tab = s_pending_tab;
    s_pending_tab = SettingsTab::kNetwork;

    ScreenSetIsHome(false);
    VkKeyScreenDesc keys{FactoryForTab(open_tab)};
    keys.on_key = OnSettingsKey;
    keys.on_key_long_press = OnSettingsLongKey;
    VkKey_AttachScreen(scr, kScreenId, keys);
    if (s_open_overview) ShowOverview();
    else {
        s_ui.group = open_tab == SettingsTab::kConversation ? 0 : open_tab == SettingsTab::kNetwork ? 1 :
            (open_tab == SettingsTab::kBluetooth || open_tab == SettingsTab::kPager) ? 4 : 5;
        ShowTab(TabIndexOf(open_tab));
        const bool whole_group = open_tab == SettingsTab::kConversation || open_tab == SettingsTab::kNetwork;
        s_ui.direct = whole_group;
        lv_label_set_text(s_ui.heading, whole_group ? Text(kGroups[s_ui.group].zh, kGroups[s_ui.group].en)
                                                    : kTabs[s_ui.active_tab].name());
    }
    s_open_overview = true;
    ESP_LOGI(TAG, "settings ready tabs=%d open=%d haptic=%d", kTabCount, (int)open_tab,
             HapticIsEnabled() ? 1 : 0);
    return scr;
}

lv_obj_t* SettingsScreen::Create() {
    s_pending_tab = SettingsTab::kNetwork;
    s_open_overview = true;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreateAssistant() {
    s_open_overview = false;
    s_pending_tab = SettingsTab::kConversation;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreateNetwork() {
    s_open_overview = false;
    s_pending_tab = SettingsTab::kNetwork;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreatePager() {
    s_open_overview = false;
    s_pending_tab = SettingsTab::kPager;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreateBluetooth() {
    s_open_overview = false;
    s_pending_tab = SettingsTab::kBluetooth;
    return CreateInternal();
}
lv_obj_t* SettingsScreen::CreateTest() {
    s_open_overview = false;
    s_pending_tab = SettingsTab::kTest;
    return CreateInternal();
}

void SettingsScreen::ReloadAfterLanguageChange() {
    s_pending_tab = SettingsTab::kLanguage;
    s_open_overview = false;
    ScreenLoadReplace(CreateInternal());
}

void SettingsScreen_OnEnterStandby() {
    valet::CloseSetupSession();
    SettingsLogUpload_Teardown();
}

void SettingsScreen_OnResumeFromStandby() {
    SettingsFrontlightTab_Refresh();
}
