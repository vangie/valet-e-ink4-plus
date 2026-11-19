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
#include <cstring>
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
};

static SettingsShellUi s_ui;
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
static void AdjustQuick(lv_event_t* event) {
    const intptr_t action = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    const uint32_t generation = s_settings_generation;
    if (!Application::GetInstance().Schedule([action, generation]() {
        if (action == 1 || action == -1) {
            auto& light = Frontlight::GetInstance();
            light.SetBrightness(std::clamp<int>(light.target_brightness() + action * 10, 0, 100), true);
        } else if (auto* codec = Board::GetInstance().GetAudioCodec()) {
            codec->SetOutputVolume(std::clamp<int>(codec->output_volume() + (action > 0 ? 10 : -10), 0, 100));
        }
        auto* expected = new uint32_t(generation);
        if (!ScreenLvAsync([](void* context) {
            const uint32_t expected = *static_cast<uint32_t*>(context);
            delete static_cast<uint32_t*>(context);
            if (s_settings_scr && expected == s_settings_generation &&
                s_ui.group == -1 && !s_ui.content_built) ShowOverview();
        }, expected)) delete expected;
    }, Application::TaskDomain::Storage)) {
        ScreenShowToast(Text("操作繁忙，请重试", "Busy. Try again."));
    }
}
static void AddQuick(const char* name, int value, intptr_t action) {
    auto* row = lv_obj_create(s_ui.content_page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), UiSy(40));
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    auto* label = lv_label_create(row);
    lv_label_set_text_fmt(label, "%s  %d", name, value);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    for (int i = 0; i < 2; ++i) {
        auto* button = lv_button_create(row);
        lv_obj_set_size(button, UiSx(48), UiSy(38));
        lv_obj_align(button, LV_ALIGN_RIGHT_MID, -UiSx(i == 0 ? 58 : 0), 0);
        auto* symbol = lv_label_create(button);
        lv_label_set_text(symbol, i == 0 ? "-" : "+");
        lv_obj_center(symbol);
        SettingsStyleSelectable(button, symbol, false);
        lv_obj_add_event_cb(button, AdjustQuick, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(i == 0 ? -action : action));
    }
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
static void AddRow(const char* title, const char* detail, lv_event_cb_t callback, intptr_t data) {
    auto* row = SettingsCreateSelectableOption(s_ui.content_page, title, callback, data);
    lv_obj_set_height(row, UiSy(detail && *detail ? 76 : 52));
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, UiSx(1), 0);
    lv_obj_set_style_radius(row, 0, 0);
    auto* label = lv_obj_get_child(row, 0);
    // The shared option re-centres its label whenever the row is resized, which
    // put the title on top of the detail line. These rows place it themselves.
    // The fit callback is a header inline with one copy per source file, so it
    // is found by the label it was registered with, not by its address.
    for (uint32_t i = lv_obj_get_event_count(row); i-- > 0;) {
        if (lv_event_dsc_get_user_data(lv_obj_get_event_dsc(row, i)) == label) lv_obj_remove_event(row, i);
    }
    lv_obj_set_width(label, lv_pct(92));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(label, detail && *detail ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, 0);
    if (detail && *detail) {
        auto* sub = lv_label_create(row);
        lv_label_set_text(sub, detail);
        lv_obj_set_width(sub, lv_pct(92));
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sub, fontpack_lv_font_ui(), 0);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_clear_flag(sub, LV_OBJ_FLAG_CLICKABLE);
    }
}
static void AddTab(SettingsTab tab, const char* detail = nullptr) {
    AddRow(kTabs[TabIndexOf(tab)].name(), detail, OpenTab, static_cast<intptr_t>(tab));
}
static void ClearContent() {
    TearDownActiveTab();
    // 分组列表自身也有控件（不归旧页签生命周期管理）。
    lv_obj_clean(s_ui.content_page);
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
        case 0: AddTab(SettingsTab::kConversation, Text("选择服务，管理自建助理", "Service and custom assistants")); break;
        case 1: AddTab(SettingsTab::kNetwork); break;
        case 2: AddTab(SettingsTab::kFrontlight); AddTab(SettingsTab::kTheme); break;
        case 3: {
            auto* note = lv_label_create(s_ui.content_page);
            lv_label_set_text(note, Text("字号、字体、边距与行距在阅读中的排版菜单设置。扩展字库放入 SD 卡 metalio/e-ink/fonts/。", "Adjust font, size and spacing in the reader. Extended fonts belong in metalio/e-ink/fonts/ on the SD card."));
            lv_obj_set_width(note, lv_pct(100));
            lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
            AddRow(Text("打开阅读", "Open reading"), nullptr, OpenReading, 0);
            break;
        }
        case 4: AddTab(SettingsTab::kBluetooth); AddTab(SettingsTab::kPager); AddTab(SettingsTab::kHaptic); break;
        case 5:
            AddTab(SettingsTab::kLanguage); AddTab(SettingsTab::kPower); AddTab(SettingsTab::kCharging);
            AddTab(SettingsTab::kLog); AddTab(SettingsTab::kTest); AddTab(SettingsTab::kAbout); break;
    }
}
static void ShowOverview() {
    ClearContent();
    s_ui.group = -1;
    lv_label_set_text(s_ui.heading, Text("设置", "Settings"));
    AddQuick(Text("前光", "Light"), Frontlight::GetInstance().target_brightness(), 1);
    if (auto* codec = Board::GetInstance().GetAudioCodec()) AddQuick(Text("音量", "Volume"), codec->output_volume(), 2);
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
    for (int i = 0; i < 6; ++i) AddRow(Text(kGroups[i].zh, kGroups[i].en), descriptions[i], OpenGroup, i);
}
static bool NavigateUp() {
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
    auto* header = lv_obj_create(body);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), UiSy(48));
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    auto* back = lv_button_create(header);
    lv_obj_set_size(back, UiSx(64), UiSy(44));
    lv_obj_add_event_cb(back, BackClicked, LV_EVENT_CLICKED, nullptr);
    auto* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<"); // the UI font pack carries no LVGL symbol glyphs
    lv_obj_center(back_label);
    SettingsStyleSelectable(back, back_label, false);
    s_ui.heading = lv_label_create(header);
    lv_obj_set_width(s_ui.heading, LV_HOR_RES - UiSx(110));
    lv_obj_align(s_ui.heading, LV_ALIGN_LEFT_MID, UiSx(78), 0);

    lv_obj_t* content = lv_obj_create(body);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(content, kSettingsContentPad, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, kSettingsOptionGap, 0);
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
        lv_label_set_text(s_ui.heading, kTabs[s_ui.active_tab].name());
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
