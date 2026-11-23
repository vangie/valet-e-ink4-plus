#include "assets/lang_config.h"
#pragma GCC optimize("O1")

// 快捷面板（从状态栏往下拉）：
//  - 上面两块：网络（点一下在 自动 → 4G → Wi-Fi 之间轮换）、代理（点一下开 / 关当前这条网的代理）；长按进对应的设置页。
//  - 下面两排：前光、色温，各 10 格，点格子调到那一档；前光的灯泡图标就是开关。
//    图标跟着档位变：灯泡 关 / 暗 / 亮，温度计 冷 / 中 / 暖。音量有实体键，黑色深度调一次就不动，都不放这里。
// 面板打开时整屏按灰度画（深灰块、平滑的图标），关掉后回到黑白。

#include "screen_frontlight.h"
#include "screen_common.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "standby_screen/standby_screen.h"
#include "settings_screen/settings_screen.h"
#include "settings_screen/settings_frontlight_tab.h"
#include "dual_network_board.h"
#include "epd_content_policy.h"
#include "fontpack_lvgl.h"
#include "frontlight.h"
#include "haptic_feedback.h"
#include "home_icon_source.h"
#include "lv_adapter_epdiy.h"
#include "settings.h"
#include "status_icons.h"
#include "valet_net/proxy.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_station.h>

#define TAG "QuickPanel"

namespace {
constexpr int kDefaultBrightness = 40;   // 零亮度重新开启时使用的亮度
constexpr int kLevels = 10;              // 每排几格
// 尺寸（面板像素），照模拟界面 × 2.125
constexpr int kPad = 22, kTileH = 94, kTileGap = 16, kTileBorder = 2;
constexpr int kRowH = 52, kRowGap = 10, kRowIconW = 52, kBlockH = 34, kBlockGap = 5, kValueW = 64;
constexpr int kGripW = 64, kGripH = 6;
const lv_color_t kDark = lv_color_hex(0x4a4a48);   // 开着的块、亮着的格子：深灰，不用纯黑
const lv_color_t kDim = lv_color_hex(0x9a9a98);    // 没设置的代理：浅灰边

const char* T(const char* zh, const char* en) { return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en; }

struct Tile {
    lv_obj_t* box = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* sub = nullptr;
    lv_obj_t* corner = nullptr;
    std::string drawn;   // 上次画出来的内容；没变就不动对象，免得墨水屏反复刷
};
struct Row {
    lv_obj_t* row = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* blocks[kLevels] = {};
    lv_obj_t* value = nullptr;
};
struct PanelUi {
    lv_obj_t* root = nullptr;          // 全屏：圆点遮住下面的页面，点空白处收起
    EpdStatusBar previous_status;      // 原页面状态栏绑定
    lv_obj_t* panel_status_label = nullptr;
    Tile link, proxy;
    Row light, warm;
    lv_timer_t* timer = nullptr;       // 网络状态刷新
};
PanelUi s_ui;
std::atomic<bool> s_switching{false};    // 正在换上网方式（后台任务）
std::atomic<bool> s_save_running{false};
std::atomic<uint32_t> s_saved_value{0};  // 低三个字节依次保存亮度、暖光比例和通道组合
bool s_light_dirty = false;              // LVGL 上下文持有，保存任务创建失败后跨面板保留

// ── 前光保存（与原面板相同：后台写 NVS） ──
void SaveLightTask(void*) {
    for (;;) {
        const uint32_t value = s_saved_value.load();
        {
            Settings settings("frontlight", true);
            settings.SetInt("brightness", value & 0xff);
            settings.SetInt("warmth", (value >> 8) & 0xff);
            settings.SetInt("cct", (value >> 16) & 0xff);
        }
        s_save_running.store(false);
        if (value == s_saved_value.load() || s_save_running.exchange(true)) break;
    }
    vTaskDelete(nullptr);
}

void SaveLight() {
    if (!s_light_dirty) return;
    auto& light = Frontlight::GetInstance();
    s_saved_value.store(light.target_brightness() | (light.warmth() << 8) |
                        (static_cast<uint32_t>(light.cct()) << 16));
    if (!s_save_running.exchange(true) &&
        xTaskCreate(SaveLightTask, "quick_fl_save", 4096, nullptr, 5, nullptr) != pdPASS) {
        s_save_running.store(false);
        ESP_LOGW(TAG, "frontlight save task failed");
        return;
    }
    s_light_dirty = false;
}

bool LightIsOn() {
    auto& light = Frontlight::GetInstance();
    return light.cct() != FrontlightCct::kOff && light.target_brightness() > 0;
}

// ── 小部件 ──
lv_obj_t* Box(lv_obj_t* parent, int x, int y, int width, int height) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

lv_obj_t* IconImage(lv_obj_t* parent) {
    auto* image = lv_image_create(parent);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    return image;
}

void SetIcon(lv_obj_t* image, ValetIcon icon, lv_color_t color) {
    if (lv_image_get_src(image) != &valet_icons[icon]) lv_image_set_src(image, &valet_icons[icon]);
    lv_obj_set_style_image_recolor(image, color, 0);
}

void SetText(lv_obj_t* label, const char* text) {
    const char* cur = lv_label_get_text(label);
    if (!cur || std::strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

// ── 网络、代理两块 ──
DualNetworkBoard* Dual() { return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance()); }

bool ProxyUsable(const valet::proxy::Prefs& p) {
    return p.HasAddress() && p.kind != net_proxy::Kind::kNone && (!p.PasswordOnly() || !p.password.empty());
}

const char* ProxyKindName(net_proxy::Kind kind) {
    switch (kind) {
        case net_proxy::Kind::kHttp: return "HTTP";
        case net_proxy::Kind::kSocks5: return "SOCKS5";
        case net_proxy::Kind::kShadowsocks: return "SS";
        case net_proxy::Kind::kTrojan: return "Trojan";
        default: return "";
    }
}

// on：深灰底白字；dim：浅灰边（还没设置）；否则白底黑边
void PaintTile(Tile& tile, ValetIcon icon, const char* title, const char* sub, const char* corner, bool on, bool dim) {
    char key[160];
    std::snprintf(key, sizeof(key), "%d|%s|%s|%s|%d|%d", int(icon), title, sub, corner, on, dim);
    if (tile.drawn == key) return;
    tile.drawn = key;
    const lv_color_t fg = on ? lv_color_white() : lv_color_black();
    lv_obj_set_style_bg_color(tile.box, on ? kDark : lv_color_white(), 0);
    lv_obj_set_style_border_color(tile.box, on ? kDark : dim ? kDim : lv_color_black(), 0);
    SetIcon(tile.icon, icon, fg);
    for (auto* label : {tile.title, tile.sub, tile.corner}) lv_obj_set_style_text_color(label, fg, 0);
    SetText(tile.title, title);
    SetText(tile.sub, sub);
    SetText(tile.corner, corner);
    lv_obj_align(tile.icon, LV_ALIGN_LEFT_MID, 18 + (56 - int(valet_icons[icon].header.w)) / 2, 0);   // 图标在左边 56 宽的格里居中
}

void RefreshTiles(lv_timer_t* = nullptr) {
    if (!s_ui.root) return;
    auto* dual = Dual();
    const bool cell = dual && dual->GetNetworkType() == NetworkType::CELLULAR;
    const bool auto_mode = dual && dual->GetNetworkMode() == NetworkMode::AUTO;
    std::string sub;
    if (s_switching.load()) {
        sub = T("正在换…", "Switching…");
    } else if (cell) {
        sub = Board::GetInstance().IsNetworkReady() ? T("已连上", "Connected") : T("正在连", "Connecting");
    } else {
        auto& wifi = WifiStation::GetInstance();
        wifi_ap_record_t ap{};
        if (wifi.IsUserEnabled() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) sub = reinterpret_cast<const char*>(ap.ssid);
        else if (wifi.IsUserEnabled() && wifi.IsLinkInProgress()) sub = T("正在连", "Connecting");
        // 空闲省电时网络会先睡下，要用时再连：这时写记着的那个网，不写「没连上」
        else if (wifi.IsUserEnabled() && !wifi.GetSsid().empty()) sub = wifi.GetSsid();
        else sub = T("没连上", "Offline");
    }
    PaintTile(s_ui.link, cell ? VALET_ICON_Q_CELL : VALET_ICON_Q_WIFI, cell ? "4G" : "Wi-Fi", sub.c_str(),
              auto_mode ? T("自动", "Auto") : "", true, false);

    const auto prefs = valet::proxy::Get();
    const bool usable = ProxyUsable(prefs);
    const bool on = usable && (valet::proxy::OnWifi() ? prefs.on_wifi : prefs.on_cell);
    PaintTile(s_ui.proxy, VALET_ICON_Q_PROXY, T("代理", "Proxy"), usable ? ProxyKindName(prefs.kind) : T("还没设置", "Not set up"),
              !usable ? "" : on ? T("开启", "On") : T("关闭", "Off"), on, !usable);
}

struct ModeJob { NetworkMode mode; };
void SetModeTask(void* arg) {
    auto* job = static_cast<ModeJob*>(arg);
    auto* dual = Dual();
    if (dual) dual->SetNetworkMode(job->mode);
    delete job;
    s_switching.store(false);
    ScreenLvAsync([](void*) { RefreshTiles(); });
    vTaskDelete(nullptr);
}

// 自动 → 4G → Wi-Fi → 自动。换的时候不重启；通话中由网络层自己挡住。
void OnLinkClicked(lv_event_t*) {
    auto* dual = Dual();
    if (!dual || s_switching.load()) return;
    const auto mode = dual->GetNetworkMode();
    const auto next = mode == NetworkMode::AUTO ? NetworkMode::CELLULAR
                    : mode == NetworkMode::CELLULAR ? NetworkMode::WIFI : NetworkMode::AUTO;
    s_switching.store(true);
    RefreshTiles();
    ScreenShowToast(next == NetworkMode::AUTO ? T("上网方式：自动", "Network: Auto")
                    : next == NetworkMode::CELLULAR ? T("上网方式：4G", "Network: 4G") : T("上网方式：Wi-Fi", "Network: Wi-Fi"));
    auto* job = new ModeJob{next};
    // 内部内存的栈：切换会停 Wi-Fi 驱动、写 NVS
    if (xTaskCreate(SetModeTask, "quick_net", 6144, job, 5, nullptr) != pdPASS) {
        delete job;
        s_switching.store(false);
        RefreshTiles();
    }
}

void OnProxyClicked(lv_event_t*) {
    auto prefs = valet::proxy::Get();
    if (!ProxyUsable(prefs)) {
        ScreenFrontlightHide();
        ScreenNavigateTo(SettingsScreen::CreateProxy);
        return;
    }
    // 只开关现在这条网的代理，另一条网的开关不动
    bool& on = valet::proxy::OnWifi() ? prefs.on_wifi : prefs.on_cell;
    on = !on;
    if (!valet::proxy::Update(prefs)) return;
    ScreenShowToast(on ? T("代理已开启", "Proxy on") : T("代理已关闭", "Proxy off"));
    RefreshTiles();
}

void OpenSettings(lv_event_t* e) {
    const bool proxy = lv_event_get_user_data(e) != nullptr;
    ScreenFrontlightHide();
    ScreenNavigateTo(proxy ? SettingsScreen::CreateProxy : SettingsScreen::CreateNetwork);
}

void BuildTile(Tile& tile, lv_obj_t* panel, int x, int y, int w, lv_event_cb_t on_click, bool proxy) {
    tile.box = Box(panel, x, y, w, kTileH);
    lv_obj_set_style_bg_opa(tile.box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile.box, kTileBorder, 0);
    lv_obj_add_flag(tile.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tile.box, LV_OBJ_FLAG_EVENT_BUBBLE);
    HapticAttachClick(tile.box);
    // 长按进设置；短按才切换（长按之后不再当作一次点击）
    lv_obj_add_event_cb(tile.box, on_click, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(tile.box, OpenSettings, LV_EVENT_LONG_PRESSED, proxy ? &s_ui : nullptr);
    tile.icon = IconImage(tile.box);
    const auto* big = fontpack_lv_font_get(30, 2);
    const auto* small = fontpack_lv_font_get(25, 2);
    constexpr int kTextX = 86;
    tile.title = Label(tile.box, big);
    lv_obj_set_pos(tile.title, kTextX, 12);
    tile.sub = Label(tile.box, small);
    lv_label_set_long_mode(tile.sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tile.sub, w - kTextX - 12);
    lv_obj_set_pos(tile.sub, kTextX, 52);
    tile.corner = Label(tile.box, small);
    lv_obj_align(tile.corner, LV_ALIGN_TOP_RIGHT, -14, 8);
}

// ── 前光、色温两排 ──
void RefreshRows() {
    if (!s_ui.root) return;
    auto& light = Frontlight::GetInstance();
    const bool on = LightIsOn();
    const int bright = light.target_brightness(), warm = light.warmth();
    const auto paint = [](Row& row, int value, bool off) {
        // 开着就至少亮一格（亮度 2 也是开着）
        const int lit = off ? 0 : std::max(value > 0 ? 1 : 0, (value + 5) / 10);
        for (int i = 0; i < kLevels; ++i) {
            const bool fill = i < lit;
            lv_obj_set_style_bg_color(row.blocks[i], fill ? kDark : lv_color_white(), 0);
            lv_obj_set_style_border_color(row.blocks[i], fill ? kDark : lv_color_black(), 0);
        }
    };
    paint(s_ui.light, bright, !on);
    paint(s_ui.warm, warm, false);
    char text[8];
    std::snprintf(text, sizeof(text), "%d", bright);
    SetText(s_ui.light.value, on ? text : T("关", "Off"));
    std::snprintf(text, sizeof(text), "%d", warm);
    SetText(s_ui.warm.value, text);
    SetIcon(s_ui.light.icon, !on ? VALET_ICON_ROW_BULB_OFF : bright <= 50 ? VALET_ICON_ROW_BULB_MIN : VALET_ICON_ROW_BULB_MAX,
            lv_color_black());
    SetIcon(s_ui.warm.icon, warm < 40 ? VALET_ICON_ROW_THERMO_COLD : warm > 60 ? VALET_ICON_ROW_THERMO_WARM
                                                                                : VALET_ICON_ROW_THERMO_MID, lv_color_black());
    // 前光关着时色温那排变淡：调了也看不出来
    lv_obj_set_style_opa(s_ui.warm.row, on ? LV_OPA_COVER : LV_OPA_50, 0);
}

void ToggleLight(lv_event_t*) {
    auto& light = Frontlight::GetInstance();
    if (LightIsOn()) {
        light.SetCct(FrontlightCct::kOff);
    } else {
        light.SetWarmth(light.warmth());
        if (light.target_brightness() == 0) light.SetBrightness(kDefaultBrightness, false);
    }
    s_light_dirty = true;
    RefreshRows();
    SaveLight();
}

// 点第 n 格调到 n×10；再点一下已经是这一档的格子，退一档（点最左边那格可以调到 0 / 关）
void OnBlockClicked(lv_event_t* e) {
    const int code = int(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const bool is_light = code >= 100;
    const int value = (code % 100) * 10;
    auto& light = Frontlight::GetInstance();
    if (is_light) {
        const bool on = LightIsOn();
        const int next = !on ? value : light.target_brightness() == value ? value - 10 : value;
        if (next <= 0) {
            light.SetCct(FrontlightCct::kOff);
        } else {
            light.SetBrightness(uint8_t(next), false);
            if (light.cct() == FrontlightCct::kOff) light.SetWarmth(light.warmth());
        }
    } else {
        light.SetWarmth(uint8_t(light.warmth() == value ? value - 10 : value));
        if (light.target_brightness() == 0) light.SetBrightness(kDefaultBrightness, false);
    }
    s_light_dirty = true;
    RefreshRows();
    SaveLight();
}

void BuildRow(Row& row, lv_obj_t* panel, int y, int width, bool is_light) {
    row.row = Box(panel, kPad, y, width - 2 * kPad, kRowH);
    auto* icon_box = Box(row.row, 0, 0, kRowIconW, kRowH);
    row.icon = IconImage(icon_box);
    lv_obj_center(row.icon);
    if (is_light) {
        lv_obj_add_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(icon_box);
        lv_obj_add_event_cb(icon_box, ToggleLight, LV_EVENT_CLICKED, nullptr);
    }
    const int blocks_x = kRowIconW + 14;
    const int blocks_w = width - 2 * kPad - blocks_x - kValueW - 8;
    const int block_w = (blocks_w - (kLevels - 1) * kBlockGap) / kLevels;
    for (int i = 0; i < kLevels; ++i) {
        auto* block = Box(row.row, blocks_x + i * (block_w + kBlockGap), (kRowH - kBlockH) / 2, block_w, kBlockH);
        lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(block, 2, 0);
        lv_obj_add_flag(block, LV_OBJ_FLAG_CLICKABLE);
        // 格子小，点的范围往上下各扩一点
        lv_obj_set_ext_click_area(block, 8);
        HapticAttachClick(block);
        lv_obj_add_event_cb(block, OnBlockClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(intptr_t((is_light ? 100 : 0) + i + 1)));
        row.blocks[i] = block;
    }
    row.value = Label(row.row, fontpack_lv_font_get(25, 2));
    lv_obj_set_width(row.value, kValueW);
    lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(row.value, LV_ALIGN_RIGHT_MID, 0, 0);
}
} // namespace

bool ScreenFrontlightIsVisible() {
    return s_ui.root != nullptr;
}

bool ScreenFrontlightOwnsDrag(int, int) {
    return false;   // 没有滑条了：格子是点的
}

void ScreenFrontlightHide() {
    if (!s_ui.root) return;
    SaveLight();
    if (auto* display = LvAdapterEpdiy::Instance()) display->RequestFullOnNextCommit();
    lv_obj_delete(s_ui.root);
    SettingsFrontlightTab_Refresh();
}

void ScreenFrontlightShow() {
    if (!HomeIconDecoderReady()) return;
    if (OtaUpgradeScreen::IsActive() || StandbyScreen::IsActive() ||
        s_ui.root || !Frontlight::GetInstance().initialized()) return;
    auto* root = Box(lv_layer_top(), 0, 0, LV_HOR_RES, LV_VER_RES);
    s_ui.root = root;
    ScreenApplyPopupDots(root);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, [](lv_event_t*) {
        if (s_ui.timer) lv_timer_delete(s_ui.timer);
        if (auto* display = LvAdapterEpdiy::Instance(); display &&
            display->GetBoundStatusBar().status_label == s_ui.panel_status_label) {
            auto status = s_ui.previous_status;
            if (status.status_label && !lv_obj_is_valid(status.status_label)) status = {};
            display->BindStatusBar(status.status_label, status.notification_label, status.network_label,
                                   status.battery_label, status.battery_pct_label, status.mute_label);
        }
        s_ui = {};
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(root, [](lv_event_t*) { ScreenFrontlightHide(); }, LV_EVENT_CLICKED, nullptr);
    if (auto* display = LvAdapterEpdiy::Instance()) s_ui.previous_status = display->GetBoundStatusBar();
    const auto status = ScreenCreateStatusBar(root);
    s_ui.panel_status_label = status.status_label;
    lv_obj_set_style_bg_color(status.bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(status.bar, LV_OPA_COVER, 0);
    if (auto* display = LvAdapterEpdiy::Instance()) display->UpdateStatusBar(true);

    const int width = LV_HOR_RES;
    const int tiles_y = kPad;
    const int rows_y = tiles_y + kTileH + 18;
    const int grip_y = rows_y + 2 * kRowH + kRowGap + 16;
    const int height = grip_y + kGripH + 16;
    auto* panel = Box(root, 0, ScreenStatusBarHeight(), width, height);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   // 点面板里的空白不收起
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    // 面板开着时整屏按灰度画
    EpdSetContentRole(panel, EpdContentRole::GrayPanel);

    const int tile_w = (width - 2 * kPad - kTileGap) / 2;
    BuildTile(s_ui.link, panel, kPad, tiles_y, tile_w, OnLinkClicked, false);
    BuildTile(s_ui.proxy, panel, kPad + tile_w + kTileGap, tiles_y, tile_w, OnProxyClicked, true);
    BuildRow(s_ui.light, panel, rows_y, width, true);
    BuildRow(s_ui.warm, panel, rows_y + kRowH + kRowGap, width, false);
    auto* grip = Box(panel, (width - kGripW) / 2, grip_y, kGripW, kGripH);
    lv_obj_set_style_bg_color(grip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grip, kGripH / 2, 0);

    RefreshTiles();
    RefreshRows();
    s_ui.timer = lv_timer_create(RefreshTiles, 1000, nullptr);
    if (auto* display = LvAdapterEpdiy::Instance()) display->RequestFullOnNextCommit();
}
