#include "settings_proxy_page.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "screen_keyboard.h"
#include "settings_common.h"
#include "valet_net/proxy.h"
#include "valet_net/shadowsocks.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
using valet::proxy::Prefs;

enum Field : intptr_t { kFieldAddress = 0, kFieldLogin = 1 };
// Segment values: which control, and which choice.
enum Choice : intptr_t { kWifiOff = 0, kWifiOn = 1, kCellOff = 10, kCellOn = 11, kKindHttp = 20, kKindSocks = 21, kKindSs = 22,
                       kMethod0 = 30 };   // kMethod0 + index into kMethodChoices

struct Ui {
    lv_obj_t* page = nullptr;
    lv_obj_t* wifi_seg = nullptr;
    lv_obj_t* cell_seg = nullptr;
    lv_obj_t* kind_seg = nullptr;
    lv_obj_t* address_detail = nullptr;
    lv_obj_t* login_detail = nullptr;
    lv_obj_t* login_title = nullptr;   // 「账号和密码」，Shadowsocks 时是「密码」
    lv_obj_t* method_gap = nullptr;    // SS only: the cipher
    lv_obj_t* method_label = nullptr;
    lv_obj_t* method_seg = nullptr;
    lv_obj_t* method_note = nullptr;
    lv_obj_t* test_label = nullptr;
    lv_obj_t* result = nullptr;
    // Text editor (an overlay on the screen)
    lv_obj_t* editor = nullptr;
    lv_obj_t* edit_hint = nullptr;
    lv_obj_t* edit_box[2] = {};     // the address; or user name and password
    lv_obj_t* edit_name[2] = {};    // caption inside a box
    lv_obj_t* edit_text[2] = {};
    lv_obj_t* edit_cursor[2] = {};
    lv_obj_t* keyboard = nullptr;
    Field field = kFieldAddress;
    int boxes = 1;
    int active = 0;                 // the box being typed into
};
Ui s_ui;
bool s_alive = false;
uint32_t s_generation = 0;
char s_edit[2][160] = {};
bool s_password_kept = false;      // the password box still holds the stored password, shown as stars
std::string s_result;               // last test result, kept while the page is open
std::atomic<bool> s_testing{false};

const char* Text(const char* zh, const char* en) {
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
}
const lv_font_t* Strong() {
    const auto* font = fontpack_lv_font_get(30, 4);
    return font ? font : fontpack_lv_font_ui();
}
const lv_font_t* Small() {
    const auto* font = fontpack_lv_font_get(25, 2);
    return font ? font : fontpack_lv_font_ui();
}
void SetText(lv_obj_t* label, const char* text) {
    // Setting the same text again still redraws, and a redraw is a panel refresh.
    if (label && std::strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}
const char* KindName(net_proxy::Kind kind) {
    return kind == net_proxy::Kind::kSocks5 ? "SOCKS5" : kind == net_proxy::Kind::kShadowsocks ? "SS" : "HTTP";
}
bool IsSs(const Prefs& prefs) { return prefs.kind == net_proxy::Kind::kShadowsocks; }
// The ciphers offered as buttons; the others (aes-*-ctr, aes-128/192-cfb, xchacha…, chacha20-ietf)
// come in with an ss:// link.
constexpr const char* kMethodChoices[] = {"aes-256-gcm", "aes-128-gcm", "chacha20-ietf-poly1305", "aes-256-cfb"};
std::string MethodOf(const Prefs& prefs) { return prefs.method.empty() ? valet::ss::kDefaultMethod : prefs.method; }
int MethodIndex(const Prefs& prefs) {
    const std::string method = MethodOf(prefs);
    for (int i = 0; i < 4; ++i) if (method == kMethodChoices[i]) return i;
    return -1;
}
void SetShown(lv_obj_t* obj, bool shown) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != shown) return;
    if (shown) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
int KindIndex(net_proxy::Kind kind) { return kind == net_proxy::Kind::kSocks5 ? 1 : kind == net_proxy::Kind::kShadowsocks ? 2 : 0; }
// Shadowsocks: one box, the password. Others: user name and password.
int PasswordBox() { return s_ui.boxes == 1 ? 0 : 1; }

std::string AddressText(const Prefs& prefs) {
    if (!prefs.HasAddress()) return Text("还没填。点这里输入", "Not set. Tap to enter");
    return prefs.host + ":" + std::to_string(prefs.port);
}
std::string LoginText(const Prefs& prefs) {
    if (IsSs(prefs)) return prefs.password.empty() ? Text("还没填。SS 必须有密码", "Not set. SS needs one")
                                                   : Text("已填", "Set");
    if (prefs.user.empty()) return Text("不用。代理要账号时点这里填", "None. Tap if the proxy asks for one");
    // The password is not shown.
    return prefs.user + (prefs.password.empty() ? Text("，没有密码", ", no password") : Text("，密码已填", ", password set"));
}

void Refresh() {
    if (!s_alive) return;
    const Prefs prefs = valet::proxy::Get();
    SettingsValetSelect(s_ui.wifi_seg, prefs.on_wifi ? 1 : 0);
    SettingsValetSelect(s_ui.cell_seg, prefs.on_cell ? 1 : 0);
    SettingsValetSelect(s_ui.kind_seg, KindIndex(prefs.kind));
    SetText(s_ui.login_title, IsSs(prefs) ? Text("密码", "Password") : Text("账号和密码", "Login"));
    SetShown(s_ui.method_gap, IsSs(prefs));
    SetShown(s_ui.method_label, IsSs(prefs));
    SetShown(s_ui.method_seg, IsSs(prefs));
    SetShown(s_ui.method_note, IsSs(prefs));
    if (IsSs(prefs)) {
        SettingsValetSelect(s_ui.method_seg, MethodIndex(prefs));
        const std::string note = MethodIndex(prefs) >= 0
            ? Text("要和服务器的设置一样。其他方式可以在地址里输整条 ss:// 链接。", "Must match the server. Other ciphers: enter a whole ss:// link as the address.")
            : std::string(Text("现在用的是 ", "In use: ")) + MethodOf(prefs) + Text("（来自 ss:// 链接）", " (from an ss:// link)");
        SetText(s_ui.method_note, note.c_str());
    }
    SetText(s_ui.address_detail, AddressText(prefs).c_str());
    SetText(s_ui.login_detail, LoginText(prefs).c_str());
    SetText(s_ui.test_label, s_testing.load() ? Text("正在测试…", "Testing…") : Text("测试这个代理", "Test this proxy"));
    SetText(s_ui.result, s_result.c_str());
}

void Save(const Prefs& prefs) {
    if (!valet::proxy::Update(prefs)) {
        ScreenShowToast(Text("操作繁忙，请重试", "Busy. Try again."));
        return;
    }
    s_result.clear();   // a result belongs to the settings it was measured with
    Refresh();
}

// ── Text editor ────────────────────────────────────────────────────────────
// One box for the address; two for the login (user name and password), each its own box.
// The box being typed into has the heavier frame and the cursor.
void RefreshEdit() {
    const auto* font = fontpack_lv_font_ui();
    for (int i = 0; i < s_ui.boxes; ++i) {
        if (!s_ui.edit_text[i]) continue;
        // Show the end of a text that is too long for the box; no scrolling animation on ink.
        const char* tail = i == PasswordBox() && s_password_kept ? "******" : s_edit[i];
        int width = lv_obj_get_content_width(s_ui.edit_box[i]) - UiSx(12);
        if (s_ui.edit_name[i]) width -= lv_obj_get_width(s_ui.edit_name[i]) + 14;
        lv_point_t size;
        while (true) {
            lv_text_get_size(&size, tail, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (width <= 0 || size.x <= width || *tail == '\0') break;
            do { ++tail; } while ((static_cast<unsigned char>(*tail) & 0xC0) == 0x80);
        }
        SetText(s_ui.edit_text[i], tail);
        lv_obj_set_width(s_ui.edit_text[i], size.x > 0 ? size.x : 1);
        // Restyle only on a change: a restyle redraws, and a redraw is a panel refresh.
        const bool active = i == s_ui.active;
        const int frame = active ? 4 : 2;
        if (lv_obj_get_style_border_width(s_ui.edit_box[i], LV_PART_MAIN) != frame) lv_obj_set_style_border_width(s_ui.edit_box[i], frame, 0);
        if (lv_obj_has_flag(s_ui.edit_cursor[i], LV_OBJ_FLAG_HIDDEN) == active) {
            if (active) lv_obj_remove_flag(s_ui.edit_cursor[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_ui.edit_cursor[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CloseEditor() {
    if (s_ui.editor) lv_obj_delete(s_ui.editor);   // its delete handler clears the pointers
}

std::string Trimmed(const char* text) {
    std::string out = text;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

void ConfirmEdit() {
    Prefs prefs = valet::proxy::Get();
    const char* problem = nullptr;
    if (s_ui.field == kFieldAddress) {
        const std::string text = Trimmed(s_edit[0]);
        if (text.empty()) {
            // No address means no proxy: the switches go off with it.
            prefs.host.clear();
            prefs.port = 0;
            prefs.on_wifi = prefs.on_cell = false;
        } else {
            net_proxy::Config parsed;
            parsed.kind = prefs.kind;
            parsed.user = prefs.user;
            parsed.password = prefs.password;
            switch (net_proxy::Parse(text, &parsed)) {
                case net_proxy::ParseResult::kOk: break;
                case net_proxy::ParseResult::kBadPort:
                    problem = Text("端口写在冒号后面，1 到 65535。例如 192.168.0.10:7890", "Put the port after a colon, 1 to 65535, e.g. 192.168.0.10:7890");
                    break;
                case net_proxy::ParseResult::kUnsupportedScheme:
                    problem = Text("开头只认 http://、socks5:// 和 ss://，也可以不写", "Only http://, socks5:// and ss:// are accepted in front; or leave it out");
                    break;
                default:
                    problem = Text("没看懂地址。例如 192.168.0.10:7890", "That is not an address, e.g. 192.168.0.10:7890");
                    break;
            }
            if (!problem && parsed.kind == net_proxy::Kind::kShadowsocks && !valet::ss::Supported(parsed.method))
                problem = Text("这条 ss:// 链接的加密方式设备不支持（SS2022 也不支持）。", "The device does not support this ss:// link's cipher (nor SS2022).");
            if (!problem) {
                prefs.kind = parsed.kind;
                if (parsed.kind == net_proxy::Kind::kShadowsocks) prefs.method = parsed.method;
                prefs.host = parsed.host;
                prefs.port = parsed.port;
                prefs.user = parsed.user;
                prefs.password = parsed.password;
            }
        }
    } else if (s_ui.boxes == 1) {
        // Shadowsocks: just the password. Empty = no password; the proxy then stays off.
        prefs.user.clear();
        prefs.password = s_password_kept ? prefs.password : std::string(s_edit[0]);
    } else {
        // The confirm key in the user-name box goes on to the password box; in the password box it saves.
        if (s_ui.active == 0) {
            s_ui.active = 1;
            RefreshEdit();
            return;
        }
        const std::string user = Trimmed(s_edit[0]);
        const std::string password = s_edit[1];   // as typed: a password may begin or end with a space
        if (user.empty() && !password.empty()) {
            problem = Text("有密码就要有账号。两个都清空 = 不用账号。", "A password needs a user name. Empty both = no login.");
            s_ui.active = 0;
            RefreshEdit();
        } else {
            prefs.user = user;
            prefs.password = user.empty() ? "" : password;
        }
    }
    if (problem) {
        lv_label_set_text(s_ui.edit_hint, problem);
        return;
    }
    CloseEditor();
    Save(prefs);
}

// One input box; `name` (may be null) is a small caption inside it, in front of the text.
void AddEditBox(lv_obj_t* panel, int index, const char* name) {
    auto* box = lv_obj_create(panel);
    s_ui.edit_box[index] = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), 78);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(box, 16, 0);
    lv_obj_set_style_pad_column(box, 3, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    // Tapping a box makes it the one being typed into.
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, [](lv_event_t* e) {
        const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
        if (index == s_ui.active) return;
        s_ui.active = index;
        RefreshEdit();
    }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    if (name) {
        s_ui.edit_name[index] = lv_label_create(box);
        lv_label_set_text(s_ui.edit_name[index], name);
        lv_obj_set_style_text_font(s_ui.edit_name[index], Small(), 0);
        lv_obj_set_style_text_color(s_ui.edit_name[index], lv_color_black(), 0);
        lv_obj_set_style_margin_right(s_ui.edit_name[index], 14, 0);
    }
    s_ui.edit_text[index] = lv_label_create(box);
    lv_label_set_text(s_ui.edit_text[index], "");
    lv_label_set_long_mode(s_ui.edit_text[index], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_ui.edit_text[index], fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.edit_text[index], lv_color_black(), 0);
    // A drawn, static cursor: nothing blinks on ink.
    static const lv_point_precise_t cursor_points[] = {{0, 0}, {8, 0}, {4, 0}, {4, 40}, {0, 40}, {8, 40}};
    s_ui.edit_cursor[index] = lv_line_create(box);
    lv_line_set_points(s_ui.edit_cursor[index], cursor_points, 6);
    lv_obj_set_style_line_width(s_ui.edit_cursor[index], 2, 0);
    lv_obj_set_style_line_color(s_ui.edit_cursor[index], lv_color_black(), 0);
    lv_obj_add_flag(s_ui.edit_cursor[index], LV_OBJ_FLAG_HIDDEN);
}

void OpenEditor(Field field) {
    if (s_ui.editor) return;
    s_ui.field = field;
    const Prefs prefs = valet::proxy::Get();
    s_ui.boxes = field == kFieldLogin && !IsSs(prefs) ? 2 : 1;
    s_ui.active = 0;
    s_edit[0][0] = s_edit[1][0] = '\0';
    if (field == kFieldAddress) {
        if (prefs.HasAddress()) std::snprintf(s_edit[0], sizeof(s_edit[0]), "%s:%d", prefs.host.c_str(), prefs.port);
    } else if (IsSs(prefs)) {
        std::snprintf(s_edit[0], sizeof(s_edit[0]), "%s", prefs.password.c_str());
    } else {
        std::snprintf(s_edit[0], sizeof(s_edit[0]), "%s", prefs.user.c_str());
        std::snprintf(s_edit[1], sizeof(s_edit[1]), "%s", prefs.password.c_str());
    }
    s_password_kept = field == kFieldLogin && !prefs.password.empty();

    auto* mask = lv_obj_create(lv_screen_active());
    s_ui.editor = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);   // taps beside the panel do not reach the page
    lv_obj_add_event_cb(mask, [](lv_event_t*) {
        s_ui.editor = s_ui.edit_hint = s_ui.keyboard = nullptr;
        for (int i = 0; i < 2; ++i) s_ui.edit_box[i] = s_ui.edit_name[i] = s_ui.edit_text[i] = s_ui.edit_cursor[i] = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    auto* panel = lv_obj_create(mask);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), UiSx(260) + 330 + (s_ui.boxes - 1) * 94);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 3, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_hor(panel, 33, 0);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 16, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    s_ui.edit_hint = lv_label_create(panel);
    lv_label_set_text(s_ui.edit_hint, field == kFieldAddress
        ? Text("代理的地址和端口，例如 192.168.0.10:7890；也可以输整条 ss:// 链接。清空再确认 = 不要代理。",
               "Proxy address and port, e.g. 192.168.0.10:7890, or a whole ss:// link. Empty = no proxy.")
        : s_ui.boxes == 1 ? Text("SS 服务器的密码。存着的显示成星号，一输入就换成新的。",
                                 "The SS server's password. A stored one shows as stars; typing replaces it.")
        : Text("点一个框再输入。存着的密码显示成星号，一输入就换成新的。", "Tap a box, then type. A stored password shows as stars; typing replaces it."));
    lv_obj_set_width(s_ui.edit_hint, lv_pct(100));
    lv_obj_set_height(s_ui.edit_hint, 74);   // two lines, so the boxes below do not move when the text changes
    lv_label_set_long_mode(s_ui.edit_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.edit_hint, Small(), 0);
    lv_obj_set_style_text_color(s_ui.edit_hint, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(s_ui.edit_hint, 6, 0);

    if (field == kFieldAddress) AddEditBox(panel, 0, nullptr);
    else {
        AddEditBox(panel, 0, Text("账号", "User"));
        AddEditBox(panel, 1, Text("密码", "Password"));
    }

    auto* cancel = SettingsValetButton(panel, Text("取消", "Cancel"), [](lv_event_t*) {
        // The button is inside the editor: delete it after this event has returned.
        lv_async_call([](void*) { CloseEditor(); }, nullptr);
    });
    lv_obj_set_height(cancel, 66);

    s_ui.keyboard = ScreenKeyboardCreate(panel, [](ScreenKeyboardEvent event, const char* text, void*) {
        char* edit = s_edit[s_ui.active];
        if (s_ui.active == PasswordBox() && s_password_kept && event != ScreenKeyboardEvent::kConfirm && event != ScreenKeyboardEvent::kLayout) {
            // First key in the password box: the stored password goes, a new one starts.
            s_password_kept = false;
            edit[0] = '\0';
            if (event == ScreenKeyboardEvent::kBackspace) { RefreshEdit(); return; }
        }
        size_t length = std::strlen(edit);
        switch (event) {
            case ScreenKeyboardEvent::kInsert:
                if (length + std::strlen(text) < sizeof(s_edit[0])) std::strcat(edit, text);
                break;
            case ScreenKeyboardEvent::kBackspace:
                if (length > 0) {
                    do { --length; } while (length > 0 && (static_cast<unsigned char>(edit[length]) & 0xC0) == 0x80);
                    edit[length] = '\0';
                }
                break;
            case ScreenKeyboardEvent::kClear: edit[0] = '\0'; break;
            case ScreenKeyboardEvent::kConfirm: ConfirmEdit(); return;
            case ScreenKeyboardEvent::kLayout: break;
        }
        RefreshEdit();
    });
    if (!s_ui.keyboard) { CloseEditor(); return; }
    lv_obj_set_height(s_ui.keyboard, 0);
    lv_obj_set_flex_grow(s_ui.keyboard, 1);
    // An address is mostly digits, dots and a colon: start on that layout.
    if (field == kFieldAddress) ScreenKeyboardShowSymbols(s_ui.keyboard);
    else ScreenKeyboardReset(s_ui.keyboard);
    lv_obj_update_layout(panel);
    RefreshEdit();
}

// ── Page ───────────────────────────────────────────────────────────────────
void OnChoice(lv_event_t* e) {
    const auto choice = static_cast<Choice>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    Prefs prefs = valet::proxy::Get();
    const bool turning_on = (choice == kWifiOn && !prefs.on_wifi) || (choice == kCellOn && !prefs.on_cell);
    if (turning_on && !prefs.HasAddress()) {
        ScreenShowToast(Text("先填代理的地址", "Enter the proxy address first"));
        OpenEditor(kFieldAddress);
        return;
    }
    if (turning_on && IsSs(prefs) && prefs.password.empty()) {
        ScreenShowToast(Text("先填 SS 的密码", "Enter the SS password first"));
        OpenEditor(kFieldLogin);
        return;
    }
    switch (choice) {
        case kWifiOff: case kWifiOn: prefs.on_wifi = choice == kWifiOn; break;
        case kCellOff: case kCellOn: prefs.on_cell = choice == kCellOn; break;
        case kKindHttp: prefs.kind = net_proxy::Kind::kHttp; break;
        case kKindSocks: prefs.kind = net_proxy::Kind::kSocks5; break;
        case kKindSs: prefs.kind = net_proxy::Kind::kShadowsocks; break;
        default:
            if (choice >= kMethod0 && choice < kMethod0 + 4) prefs.method = kMethodChoices[choice - kMethod0];
            break;
    }
    Save(prefs);
}

void OnRow(lv_event_t* e) {
    OpenEditor(static_cast<Field>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

struct TestJob { Prefs prefs; uint32_t generation; };
void TestTask(void* context) {
    auto* job = static_cast<TestJob*>(context);
    const auto result = valet::proxy::Test(job->prefs);
    auto* done = new std::pair<uint32_t, std::string>(job->generation, valet::proxy::Describe(result));
    delete job;
    if (!ScreenLvAsync([](void* p) {
        auto* done = static_cast<std::pair<uint32_t, std::string>*>(p);
        s_testing.store(false);
        if (s_alive && done->first == s_generation) {
            s_result = done->second;
            Refresh();
        }
        delete done;
    }, done)) {
        delete done;
        s_testing.store(false);
    }
    vTaskDelete(nullptr);
}

void OnTest(lv_event_t*) {
    const Prefs prefs = valet::proxy::Get();
    if (!prefs.HasAddress()) {
        ScreenShowToast(Text("先填代理的地址", "Enter the proxy address first"));
        return;
    }
    if (s_testing.exchange(true)) return;
    s_result.clear();
    Refresh();
    auto* job = new TestJob{prefs, s_generation};
    // Its own task: the request may wait many seconds, and the TLS handshake runs on this stack.
    if (xTaskCreate(TestTask, "proxy_test", 8192, job, 4, nullptr) != pdPASS) {
        delete job;
        s_testing.store(false);
        s_result = Text("启动不了测试，请重试。", "Could not start the test. Try again.");
        Refresh();
    }
}

// A ruled row: name, current value under it, and a mark that it opens something.
lv_obj_t* AddRow(lv_obj_t* page, const char* title, Field field, bool first, lv_obj_t** title_out = nullptr) {
    auto* row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 112);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_side(row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_BOTTOM | (first ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_NONE)), 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, OnRow, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(field)));
    auto* name = lv_label_create(row);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_font(name, Strong(), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, -20);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
    if (title_out) *title_out = name;
    auto* detail = lv_label_create(row);
    lv_label_set_text(detail, "");
    lv_obj_set_style_text_font(detail, Small(), 0);
    lv_obj_set_width(detail, lv_pct(100));
    lv_obj_set_style_pad_right(detail, 52, 0);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_LEFT_MID, 4, 22);
    lv_obj_clear_flag(detail, LV_OBJ_FLAG_CLICKABLE);
    auto* go = lv_label_create(row);
    lv_label_set_text(go, ">");
    lv_obj_set_style_text_font(go, Small(), 0);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_clear_flag(go, LV_OBJ_FLAG_CLICKABLE);
    return detail;
}
}  // namespace

std::string SettingsProxyPage_Summary() {
    const Prefs prefs = valet::proxy::Get();
    if (!prefs.HasAddress()) return Text("没设置。点这里填", "Not set. Tap to set up");
    const std::string where = std::string(KindName(prefs.kind)) + " " + prefs.host + ":" + std::to_string(prefs.port);
    if (valet::proxy::InForce()) return std::string(Text("在用 · ", "In use · ")) + where;
    if (!prefs.on_wifi && !prefs.on_cell) return std::string(Text("没开 · ", "Off · ")) + where;
    // Switched on for the other way of getting online only.
    return std::string(prefs.on_wifi ? Text("连 Wi-Fi 时才走 · ", "Only on Wi-Fi · ")
                                     : Text("用 4G 时才走 · ", "Only on 4G · ")) + where;
}

bool SettingsProxyPage_CloseEditor() {
    if (!s_ui.editor) return false;
    CloseEditor();
    return true;
}

void SettingsProxyPage_Reset() {
    CloseEditor();
    s_ui = {};
    s_alive = false;
    ++s_generation;   // a test still running will not write into the next page
    s_result.clear();
}

void SettingsProxyPage_Build(lv_obj_t* page) {
    s_ui = {};
    s_ui.page = page;
    s_alive = true;
    ++s_generation;
    lv_obj_set_style_pad_row(page, 0, 0);
    const char* const choices[] = {Text("直连", "Direct"), Text("走代理", "Use proxy")};
    static const intptr_t kWifiValues[] = {kWifiOff, kWifiOn};
    static const intptr_t kCellValues[] = {kCellOff, kCellOn};
    static const intptr_t kKindValues[] = {kKindHttp, kKindSocks, kKindSs};

    SettingsValetLabel(page, Text("连 Wi-Fi 时", "On Wi-Fi"));
    s_ui.wifi_seg = SettingsValetSegments(page, choices, 2, OnChoice, kWifiValues);
    SettingsValetGap(page, 30);
    SettingsValetLabel(page, Text("用 4G 时", "On 4G"));
    s_ui.cell_seg = SettingsValetSegments(page, choices, 2, OnChoice, kCellValues);
    SettingsValetNote(page, Text("4G 下连不到家里局域网里的代理。",
                                 "On 4G only a proxy with a public address can be reached."));
    SettingsValetGap(page, 30);
    SettingsValetLabel(page, Text("代理类型", "Proxy type"));
    static const char* const kKinds[] = {"HTTP", "SOCKS5", "SS"};
    s_ui.kind_seg = SettingsValetSegments(page, kKinds, 3, OnChoice, kKindValues);
    // SS only: which cipher. Shown and hidden by Refresh().
    s_ui.method_gap = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.method_gap);
    lv_obj_set_size(s_ui.method_gap, lv_pct(100), 30);
    s_ui.method_label = SettingsValetLabel(page, Text("加密方式", "Cipher"));
    static const char* const kMethodNames[] = {"AES-256-GCM", "AES-128-GCM", "ChaCha20-Poly1305", "AES-256-CFB"};
    static const intptr_t kMethodValues[] = {kMethod0, kMethod0 + 1, kMethod0 + 2, kMethod0 + 3};
    s_ui.method_seg = SettingsValetSegments(page, kMethodNames, 4, OnChoice, kMethodValues, 2);
    s_ui.method_note = SettingsValetNote(page, "");
    SettingsValetGap(page, 30);
    s_ui.address_detail = AddRow(page, Text("地址", "Address"), kFieldAddress, true);
    s_ui.login_detail = AddRow(page, Text("账号和密码", "Login"), kFieldLogin, false, &s_ui.login_title);
    SettingsValetGap(page, 30);
    auto* test = SettingsValetButton(page, "", OnTest);
    s_ui.test_label = lv_obj_get_child(test, 0);
    s_ui.result = SettingsValetNote(page, "");
    SettingsValetNote(page, Text("不走代理的：对时、局域网里的地址、上传日志。",
                                 "Not proxied: time sync, local addresses, log upload."));
    SettingsValetGap(page);
    Refresh();
}
