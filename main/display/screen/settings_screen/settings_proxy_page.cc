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

enum Field : intptr_t { kFieldAddress = 0, kFieldLogin = 1, kFieldMethod = 2, kFieldSni = 3, kFieldProtocol = 4 };
// Segment values: which control, and which choice.
enum Choice : intptr_t { kWifiOff = 0, kWifiOn = 1, kCellOff = 10, kCellOn = 11, kKindHttp = 20, kKindSocks = 21, kKindMore = 22,
                         kCertCheck = 30, kCertSkip = 31 };

struct Ui {
    lv_obj_t* page = nullptr;
    lv_obj_t* wifi_seg = nullptr;
    lv_obj_t* cell_seg = nullptr;
    lv_obj_t* kind_seg = nullptr;
    lv_obj_t* address_detail = nullptr;
    lv_obj_t* login_detail = nullptr;
    lv_obj_t* login_title = nullptr;   // 「账号和密码」，SS 和 Trojan 时是「密码」
    lv_obj_t* method_detail = nullptr; // SS only: the cipher row (its detail label)
    lv_obj_t* protocol_detail = nullptr; // 「更多协议」 only: the protocol row (its detail label)
    lv_obj_t* sni_detail = nullptr;    // Trojan only: the server-name row (its detail label)
    lv_obj_t* cert = nullptr;          // Trojan only: the certificate check (label, choice, note)
    lv_obj_t* cert_seg = nullptr;
    lv_obj_t* picker = nullptr;        // the cipher list, an overlay on the screen
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
    return kind == net_proxy::Kind::kSocks5 ? "SOCKS5" : kind == net_proxy::Kind::kShadowsocks ? "SS"
         : kind == net_proxy::Kind::kTrojan ? "Trojan" : "HTTP";
}
// The protocols under 「更多协议」, in the order of their list.
struct Protocol { const char* name; net_proxy::Kind kind; };
constexpr Protocol kProtocols[] = {{"SS", net_proxy::Kind::kShadowsocks}, {"Trojan", net_proxy::Kind::kTrojan}};
constexpr int kProtocolCount = sizeof(kProtocols) / sizeof(kProtocols[0]);

const char* ProtocolName(net_proxy::Kind kind) {
    for (const auto& p : kProtocols) if (p.kind == kind) return p.name;
    return "";
}
bool IsSs(const Prefs& prefs) { return prefs.kind == net_proxy::Kind::kShadowsocks; }
bool IsTrojan(const Prefs& prefs) { return prefs.kind == net_proxy::Kind::kTrojan; }
std::string MethodOf(const Prefs& prefs) { return prefs.method.empty() ? valet::ss::kDefaultMethod : prefs.method; }
void SetShown(lv_obj_t* obj, bool shown) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != shown) return;
    if (shown) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
int KindIndex(net_proxy::Kind kind) {
    // HTTP, SOCKS5, then 「更多协议」 for every protocol picked from a list (SS, Trojan).
    return kind == net_proxy::Kind::kSocks5 ? 1 : kind == net_proxy::Kind::kShadowsocks || kind == net_proxy::Kind::kTrojan ? 2 : 0;
}
// SS and Trojan: one box, the password. Others: user name and password. (The server-name editor
// also has one box, which is not a password.)
int PasswordBox() { return s_ui.field == kFieldSni ? -1 : s_ui.boxes == 1 ? 0 : 1; }

std::string AddressText(const Prefs& prefs) {
    if (!prefs.HasAddress()) return Text("还没填。点这里输入", "Not set. Tap to enter");
    return prefs.host + ":" + std::to_string(prefs.port);
}
std::string LoginText(const Prefs& prefs) {
    if (prefs.PasswordOnly()) {
        if (!prefs.password.empty()) return Text("已填", "Set");
        return IsSs(prefs) ? Text("还没填。SS 必须有密码", "Not set. SS needs one") : Text("还没填。Trojan 必须有密码", "Not set. Trojan needs one");
    }
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
    SetText(s_ui.login_title, prefs.PasswordOnly() ? Text("密码", "Password") : Text("账号和密码", "Login"));
    if (s_ui.protocol_detail) {
        lv_obj_t* row = lv_obj_get_parent(s_ui.protocol_detail);
        SetShown(row, prefs.PasswordOnly());
        SetText(s_ui.protocol_detail, ProtocolName(prefs.kind));
        // The first row shown carries the rule on top.
        lv_obj_set_style_border_side(lv_obj_get_parent(s_ui.address_detail), static_cast<lv_border_side_t>(
            LV_BORDER_SIDE_BOTTOM | (prefs.PasswordOnly() ? LV_BORDER_SIDE_NONE : LV_BORDER_SIDE_TOP)), 0);
    }
    if (s_ui.sni_detail) {
        SetShown(lv_obj_get_parent(s_ui.sni_detail), IsTrojan(prefs));
        SetText(s_ui.sni_detail, prefs.sni.empty() ? Text("和地址一样", "Same as the address") : prefs.sni.c_str());
    }
    SetShown(s_ui.cert, IsTrojan(prefs));
    SettingsValetSelect(s_ui.cert_seg, prefs.insecure ? 1 : 0);
    if (s_ui.method_detail) {
        SetShown(lv_obj_get_parent(s_ui.method_detail), IsSs(prefs));
        SetText(s_ui.method_detail, MethodOf(prefs).c_str());
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
                    problem = Text("开头只认 http://、socks5://、ss:// 和 trojan://，也可以不写", "Only http://, socks5://, ss:// and trojan:// are accepted in front; or leave it out");
                    break;
                case net_proxy::ParseResult::kUnsupportedTransport:
                    problem = Text("设备只支持普通 TCP 的 Trojan，不支持 ws、grpc 这些传输方式。", "The device supports Trojan over plain TCP only, not ws, grpc and the like.");
                    break;
                default:
                    problem = Text("没看懂地址。例如 192.168.0.10:7890", "That is not an address, e.g. 192.168.0.10:7890");
                    break;
            }
            if (!problem && parsed.kind == net_proxy::Kind::kShadowsocks && !valet::ss::Supported(parsed.method))
                problem = Text("这条 ss:// 链接的加密方式设备不支持（SS2022 也不支持）。", "The device does not support this ss:// link's cipher (nor SS2022).");
            if (!problem) {
                prefs.kind = parsed.kind;
                if (prefs.PasswordOnly()) prefs.more_kind = prefs.kind;
                if (parsed.kind == net_proxy::Kind::kShadowsocks) prefs.method = parsed.method;
                // A trojan:// link brings its server name and certificate setting; a bare address keeps them.
                if (parsed.kind == net_proxy::Kind::kTrojan && Trimmed(s_edit[0]).find("://") != std::string::npos) {
                    prefs.sni = parsed.sni;
                    prefs.insecure = parsed.insecure;
                }
                prefs.host = parsed.host;
                prefs.port = parsed.port;
                prefs.user = parsed.user;
                prefs.password = parsed.password;
            }
        }
    } else if (s_ui.field == kFieldSni) {
        // Trojan's server name. Empty, or the same as the address = use the address.
        const std::string sni = Trimmed(s_edit[0]);
        if (sni.find_first_of(" /:@") != std::string::npos) problem = Text("服务器名是一个域名，例如 example.com", "The server name is a domain, e.g. example.com");
        else prefs.sni = sni == prefs.host ? "" : sni;
    } else if (s_ui.boxes == 1) {
        // SS and Trojan: just the password. Empty = no password; the proxy then stays off.
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
    s_ui.boxes = field == kFieldLogin && !prefs.PasswordOnly() ? 2 : 1;
    s_ui.active = 0;
    s_edit[0][0] = s_edit[1][0] = '\0';
    if (field == kFieldAddress) {
        if (prefs.HasAddress()) std::snprintf(s_edit[0], sizeof(s_edit[0]), "%s:%d", prefs.host.c_str(), prefs.port);
    } else if (field == kFieldSni) {
        std::snprintf(s_edit[0], sizeof(s_edit[0]), "%s", prefs.sni.c_str());
    } else if (prefs.PasswordOnly()) {
        std::snprintf(s_edit[0], sizeof(s_edit[0]), "%s", prefs.password.c_str());
    } else {
        std::snprintf(s_edit[0], sizeof(s_edit[0]), "%s", prefs.user.c_str());
        std::snprintf(s_edit[1], sizeof(s_edit[1]), "%s", prefs.password.c_str());
    }
    s_password_kept = field == kFieldLogin && !prefs.password.empty();

    // Dots over the page, a black title bar on the panel: plainly a layer above the page.
    // A tap beside the panel does nothing (it would lose what was typed); 取消 in the bar closes.
    auto* mask = ScreenPopupMask(lv_screen_active());
    s_ui.editor = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t*) {
        s_ui.editor = s_ui.edit_hint = s_ui.keyboard = nullptr;
        for (int i = 0; i < 2; ++i) s_ui.edit_box[i] = s_ui.edit_name[i] = s_ui.edit_text[i] = s_ui.edit_cursor[i] = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    auto* sheet = lv_obj_create(mask);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, lv_pct(100), UiSx(260) + 330 + (s_ui.boxes - 1) * 94);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    const char* title = field == kFieldAddress ? Text("地址", "Address")
                      : field == kFieldSni ? Text("服务器名（SNI）", "Server name (SNI)")
                      : s_ui.boxes == 1 ? Text("密码", "Password") : Text("账号和密码", "Login");
    ScreenPopupTitleBar(sheet, title, Strong(), Text("取消", "Cancel"), Small(), [](lv_event_t*) {
        // The bar is inside the editor: delete it after this event has returned.
        lv_async_call([](void*) { CloseEditor(); }, nullptr);
    });
    auto* panel = lv_obj_create(sheet);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_pad_hor(panel, 33, 0);
    lv_obj_set_style_pad_top(panel, 20, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 16, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.edit_hint = lv_label_create(panel);
    lv_label_set_text(s_ui.edit_hint, field == kFieldAddress
        ? Text("代理的地址和端口，例如 192.168.0.10:7890；也可以输整条 ss:// 或 trojan:// 链接。清空再确认 = 不要代理。",
               "Proxy address and port, e.g. 192.168.0.10:7890, or a whole ss:// or trojan:// link. Empty = no proxy.")
        : field == kFieldSni ? Text("TLS 里用的服务器名，和节点设置里的 sni 一样。清空 = 用地址。",
                                    "The server name used in TLS, as the node's sni. Empty = the address.")
        : s_ui.boxes == 1 ? Text("服务器的密码。存着的显示成星号，一输入就换成新的。",
                                 "The server's password. A stored one shows as stars; typing replaces it.")
        : Text("点一个框再输入。存着的密码显示成星号，一输入就换成新的。", "Tap a box, then type. A stored password shows as stars; typing replaces it."));
    lv_obj_set_width(s_ui.edit_hint, lv_pct(100));
    lv_obj_set_height(s_ui.edit_hint, 74);   // two lines, so the boxes below do not move when the text changes
    lv_label_set_long_mode(s_ui.edit_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.edit_hint, Small(), 0);
    lv_obj_set_style_text_color(s_ui.edit_hint, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(s_ui.edit_hint, 6, 0);

    if (field == kFieldAddress || field == kFieldSni) AddEditBox(panel, 0, nullptr);
    else if (s_ui.boxes == 1) AddEditBox(panel, 0, Text("密码", "Password"));
    else {
        AddEditBox(panel, 0, Text("账号", "User"));
        AddEditBox(panel, 1, Text("密码", "Password"));
    }

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
    lv_obj_update_layout(sheet);
    RefreshEdit();
}

// ── Picker ─────────────────────────────────────────────────────────────────
// One popup list for the protocol under 「更多协议」 and for SS's cipher: every choice in one list,
// with no judgement on them (the server decides which one is used). Dots over the page and a
// black title bar mark it as a layer above the page. A list longer than the panel scrolls with a
// scroll bar always shown and settles on whole rows; a short one gets a panel just big enough.
// Tapping a row picks it and closes the list; so does 关闭, a tap beside it or the back key.
constexpr int32_t kPickerRowH = 92;
enum class PickerKind : uint8_t { kProtocol, kCipher };
PickerKind s_picker_kind = PickerKind::kCipher;
const char* PickerItem(int index) {
    if (s_picker_kind == PickerKind::kCipher) return valet::ss::MethodName(index);
    return index >= 0 && index < kProtocolCount ? kProtocols[index].name : nullptr;
}

void ClosePicker() {
    if (s_ui.picker) lv_obj_delete(s_ui.picker);   // its delete handler clears the pointer
}

void AddPickerRow(lv_obj_t* list, int index, bool selected) {
    auto* row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), kPickerRowH);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    HapticAttachClick(row);
    auto* label = lv_label_create(row);
    lv_label_set_text(label, PickerItem(index));
    lv_obj_set_style_text_font(label, selected ? Strong() : fontpack_lv_font_ui(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    if (selected) {
        auto* mark = lv_label_create(row);
        lv_label_set_text(mark, Text("在用", "In use"));
        lv_obj_set_style_text_font(mark, Small(), 0);
        lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -24, 0);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(row, [](lv_event_t* e) {
        const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
        Prefs prefs = valet::proxy::Get();
        if (s_picker_kind == PickerKind::kCipher) prefs.method = valet::ss::MethodName(index);
        else prefs.kind = prefs.more_kind = kProtocols[index].kind;
        ClosePicker();
        Save(prefs);
    }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}

void OpenPicker(PickerKind kind) {
    if (s_ui.picker || s_ui.editor) return;
    s_picker_kind = kind;
    const Prefs prefs = valet::proxy::Get();
    const std::string current = kind == PickerKind::kCipher ? MethodOf(prefs) : ProtocolName(prefs.kind);
    int count = 0;
    while (PickerItem(count)) ++count;

    auto* mask = ScreenPopupMask(lv_screen_active());
    s_ui.picker = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t* e) {
        if (lv_event_get_target(e) == lv_event_get_current_target(e)) ClosePicker();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(mask, [](lv_event_t*) { s_ui.picker = nullptr; }, LV_EVENT_DELETE, nullptr);

    auto* sheet = lv_obj_create(mask);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, lv_pct(100), lv_pct(76));
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ScreenPopupTitleBar(sheet, kind == PickerKind::kCipher ? Text("加密方式", "Cipher") : Text("协议", "Protocol"), Strong(),
                        Text("关闭", "Close"), Small(), [](lv_event_t*) { lv_async_call([](void*) { ClosePicker(); }, nullptr); });

    auto* panel = lv_obj_create(sheet);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_pad_hor(panel, 33, 0);
    lv_obj_set_style_pad_top(panel, 16, 0);
    lv_obj_set_style_pad_bottom(panel, 16, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    auto* note = lv_label_create(panel);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(note, Small(), 0);

    // The list: no flinging or bouncing (each frame is a refresh on ink).
    auto* list = lv_obj_create(panel);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_right(list, 24, 0);   // room for the scroll bar
    lv_obj_set_style_margin_top(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_clear_flag(list, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_set_style_width(list, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 0, LV_PART_SCROLLBAR);
    int selected = 0;
    for (int i = 0; i < count; ++i) {
        const bool on = current == PickerItem(i);
        AddPickerRow(list, i, on);
        if (on) selected = i;
    }
    lv_obj_update_layout(sheet);
    const int32_t fits = LV_MAX(1, lv_obj_get_content_height(list) / kPickerRowH);
    const bool scrolls = count > fits;
    lv_label_set_text(note, scrolls ? Text("要和服务器的设置一样。列表可以上下拖。", "Must match the server. Drag the list up and down.")
                                    : Text("要和服务器的设置一样。", "Must match the server."));
    lv_obj_set_flex_grow(list, 0);
    if (!scrolls) {
        // Few choices: a panel just big enough, no scroll bar.
        lv_obj_set_height(list, count * kPickerRowH);
        lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_grow(panel, 0);
        lv_obj_set_height(panel, LV_SIZE_CONTENT);
        lv_obj_set_height(sheet, LV_SIZE_CONTENT);
        lv_obj_update_layout(sheet);
        return;
    }
    // A whole number of rows tall, so no row is ever cut at the top or the bottom.
    lv_obj_set_height(list, fits * kPickerRowH);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ON);
    // When a drag ends, settle on whole rows.
    lv_obj_add_event_cb(list, [](lv_event_t* e) {
        auto* l = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        const int32_t y = lv_obj_get_scroll_y(l);
        const int32_t end = y + lv_obj_get_scroll_bottom(l);
        const int32_t whole = LV_CLAMP(0, (y + kPickerRowH / 2) / kPickerRowH * kPickerRowH, end);
        if (whole != y) lv_obj_scroll_to_y(l, whole, LV_ANIM_OFF);
    }, LV_EVENT_SCROLL_END, nullptr);
    lv_obj_update_layout(list);
    // Open with the current one in the middle where possible.
    const int32_t end = lv_obj_get_scroll_bottom(list);
    lv_obj_scroll_to_y(list, LV_CLAMP(0, (selected - fits / 2) * kPickerRowH, end), LV_ANIM_OFF);
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
    if (turning_on && prefs.PasswordOnly() && prefs.password.empty()) {
        ScreenShowToast(IsSs(prefs) ? Text("先填 SS 的密码", "Enter the SS password first") : Text("先填 Trojan 的密码", "Enter the Trojan password first"));
        OpenEditor(kFieldLogin);
        return;
    }
    switch (choice) {
        case kWifiOff: case kWifiOn: prefs.on_wifi = choice == kWifiOn; break;
        case kCellOff: case kCellOn: prefs.on_cell = choice == kCellOn; break;
        case kKindHttp: prefs.kind = net_proxy::Kind::kHttp; break;
        case kKindSocks: prefs.kind = net_proxy::Kind::kSocks5; break;
        // Back to the protocol used last; which one is changed in the 「协议」 row.
        case kKindMore: if (!prefs.PasswordOnly()) prefs.kind = prefs.more_kind; break;
        case kCertCheck: case kCertSkip: prefs.insecure = choice == kCertSkip; break;
    }
    Save(prefs);
}

void OnRow(lv_event_t* e) {
    const auto field = static_cast<Field>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (field == kFieldMethod) { OpenPicker(PickerKind::kCipher); return; }
    if (field == kFieldProtocol) { OpenPicker(PickerKind::kProtocol); return; }
    OpenEditor(field);
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
    if (s_ui.picker) { ClosePicker(); return true; }
    if (!s_ui.editor) return false;
    CloseEditor();
    return true;
}

void SettingsProxyPage_Reset() {
    CloseEditor();
    ClosePicker();
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
    static const intptr_t kKindValues[] = {kKindHttp, kKindSocks, kKindMore};
    static const intptr_t kCertValues[] = {kCertCheck, kCertSkip};

    SettingsValetLabel(page, Text("连 Wi-Fi 时", "On Wi-Fi"));
    s_ui.wifi_seg = SettingsValetSegments(page, choices, 2, OnChoice, kWifiValues);
    SettingsValetGap(page, 30);
    SettingsValetLabel(page, Text("用 4G 时", "On 4G"));
    s_ui.cell_seg = SettingsValetSegments(page, choices, 2, OnChoice, kCellValues);
    SettingsValetNote(page, Text("4G 下连不到家里局域网里的代理。",
                                 "On 4G only a proxy with a public address can be reached."));
    SettingsValetGap(page, 30);
    SettingsValetLabel(page, Text("代理类型", "Proxy type"));
    const char* const kinds[] = {"HTTP", "SOCKS5", Text("更多协议", "More")};
    s_ui.kind_seg = SettingsValetSegments(page, kinds, 3, OnChoice, kKindValues);
    SettingsValetGap(page, 30);
    s_ui.protocol_detail = AddRow(page, Text("协议", "Protocol"), kFieldProtocol, true);   // 「更多协议」 only
    s_ui.address_detail = AddRow(page, Text("地址", "Address"), kFieldAddress, true);
    s_ui.login_detail = AddRow(page, Text("账号和密码", "Login"), kFieldLogin, false, &s_ui.login_title);
    s_ui.method_detail = AddRow(page, Text("加密方式", "Cipher"), kFieldMethod, false);   // SS only
    s_ui.sni_detail = AddRow(page, Text("服务器名（SNI）", "Server name (SNI)"), kFieldSni, false);   // Trojan only
    // Trojan only: whether the server's certificate is checked against the server name.
    s_ui.cert = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.cert);
    lv_obj_set_size(s_ui.cert, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui.cert, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_ui.cert, LV_OBJ_FLAG_SCROLLABLE);
    SettingsValetGap(s_ui.cert, 30);
    SettingsValetLabel(s_ui.cert, Text("检查证书", "Check the certificate"));
    const char* const cert_choices[] = {Text("检查", "Check"), Text("不检查", "Skip")};
    s_ui.cert_seg = SettingsValetSegments(s_ui.cert, cert_choices, 2, OnChoice, kCertValues);
    SettingsValetNote(s_ui.cert, Text("要和节点的设置一样：Surge 里写着 skip-cert-verify=true 的选「不检查」。",
                                      "Match the node: choose Skip where Surge says skip-cert-verify=true."));
    SettingsValetGap(page, 30);
    auto* test = SettingsValetButton(page, "", OnTest);
    s_ui.test_label = lv_obj_get_child(test, 0);
    s_ui.result = SettingsValetNote(page, "");
    SettingsValetNote(page, Text("不走代理的：对时、局域网里的地址、上传日志。",
                                 "Not proxied: time sync, local addresses, log upload."));
    SettingsValetGap(page);
    Refresh();
}
