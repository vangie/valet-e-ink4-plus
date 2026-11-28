#include "screen_keyboard.h"
#include "screen_pinyin_dict.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "epd_refresh.h"
#include "ui_scale.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
enum class KbMode : uint8_t { kEn = 0, kSym = 1, kMore = 2 }; // 字母、数字符号、扩展符号
constexpr int kKbCols = 10; // 每行键位上限
constexpr int kKbRows = 4; // 固定四行
constexpr int kKbKeysCap = kKbCols * kKbRows; // 键位容量
constexpr lv_coord_t kKbGap = UiSx(7); // 按键间距
constexpr int kCandSlots = 7; // 每页显示的候选汉字数
constexpr lv_coord_t kCandBarH = UiSy(54); // 候选词栏高度

enum class KbAction : uint8_t {
    kChar = 0, // 输入字符
    kBackspace, // 删除末尾字符
    kShift, // 切换大小写
    kModeEn, // 切换字母
    kModeSym, // 切换数字与常用符号
    kModeMore, // 切换扩展符号
    kToggleLang, // 切换中/英文
    kConfirm, // 确认当前输入
    kSpace, // 输入空格
    kEmpty, // 不创建按键
};

struct KbKeyDesc {
    const char* label = nullptr;  // 显示
    KbAction action = KbAction::kChar; // 按键动作
};

struct KeyFeedback {
    uint32_t tick = 0; // 按下时间。
    uint32_t sequence = 0; // 按下时的落墨序号。
};

void Strip(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// 将 UTF-8 字符串拆分为单字列表
void SplitUtf8Chars(const char* s, std::vector<std::string>& out) {
    if (!s) return;
    while (*s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        out.emplace_back(s, len);
        s += len;
    }
}

struct Keyboard {
    lv_obj_t* root = nullptr; // 键盘容器。
    lv_obj_t* keys[kKbKeysCap] = {}; // 按键对象。
    KbMode mode = KbMode::kEn; // 当前字符布局。
    bool upper = false; // 大写状态。
    bool allow_zh = false; // 是否显示中/英切换。
    bool zh_mode = false; // 当前是否为中文拼音模式。
    bool is_assoc = false; // 当前候选栏展示的是否为选字后的联想字。
    std::string py_buf; // 当前正在输入的拼音串。
    std::vector<std::string> cands; // 当前匹配的候选字列表。
    int cand_page = 0; // 当前候选字页码。
    lv_obj_t* cand_bar = nullptr; // 顶部拼音与候选词栏。
    lv_obj_t* py_label = nullptr; // 拼音字母显示。
    lv_obj_t* cand_btns[kCandSlots] = {}; // 候选字按钮。
    lv_obj_t* prev_btn = nullptr; // 上一页候选。
    lv_obj_t* next_btn = nullptr; // 下一页候选。

    lv_timer_t* timer = nullptr; // 按下反馈计时器。
    KeyFeedback key_feedback[kKbKeysCap]; // 各键落墨状态。
    KbAction pending_action = KbAction::kEmpty; // 待反馈完成的操作。
    ScreenKeyboardCallback callback = nullptr; // 输入事件回调。
    void* context = nullptr; // 调用方上下文。
    void Stop();
    void Tick();
    void Feedback(lv_event_t* e);
    static const lv_image_dsc_t* PressedImage();
    void Fill(KbKeyDesc out[kKbKeysCap]);
    void Pressed(lv_event_t* e);
    void Clicked(lv_event_t* e);
    void Apply(KbAction action);
    static void AddIcon(lv_obj_t* btn, KbAction action);
    void UpdatePinyinCandidates();
    void UpdateAssocCandidates(const std::string& last_utf8_word);
    void RefreshCandBar();
    void PickCandidate(int slot);
    void ClearPinyin();
    void Layout();
    void Rebuild();
};

void Keyboard::Stop() {
    if (timer != nullptr) {
        lv_timer_delete(timer);
        timer = nullptr;
    }
    pending_action = KbAction::kEmpty;
}

void Keyboard::Tick() {
    if (root == nullptr ||
        !lv_obj_is_visible(root)) {
        Stop();
        return;
    }
    const auto stats = EpdGetStats();
    bool pending = false;
    for (int i = 0; i < kKbKeysCap; ++i) {
        lv_obj_t* key = keys[i];
        if (key == nullptr || !lv_obj_has_state(key, LV_STATE_USER_1)) continue;
        const auto& feedback = key_feedback[i];
        if (lv_obj_has_state(key, LV_STATE_PRESSED) ||
            lv_tick_elaps(feedback.tick) < 30 ||
            static_cast<int32_t>(stats.completed - feedback.sequence) <= 0) {
            pending = true;
        } else {
            lv_obj_remove_state(key, LV_STATE_USER_1);
        }
    }
    if (!pending) {
        const KbAction action = pending_action;
        Stop();
        Apply(action);
    }
}

void Keyboard::Feedback(lv_event_t* e) {
    auto* key = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (int i = 0; i < kKbKeysCap; ++i) {
        if (keys[i] == key) {
            key_feedback[i] = {lv_tick_get(), EpdGetStats().submitted};
            break;
        }
    }
    lv_obj_add_state(key, LV_STATE_USER_1);
    lv_obj_invalidate(key);
    if (timer == nullptr) {
        timer = lv_timer_create([](lv_timer_t* t) { static_cast<Keyboard*>(lv_timer_get_user_data(t))->Tick(); }, 30, this);
    }
}

const lv_image_dsc_t* Keyboard::PressedImage() {
    static const uint8_t pixels[] = {
        0x00, 0xFF, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0xFF, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    static const lv_image_dsc_t image = [] {
        lv_image_dsc_t dsc = {};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_L8;
        dsc.header.w = 4;
        dsc.header.h = 4;
        dsc.header.stride = 4;
        dsc.data_size = sizeof(pixels);
        dsc.data = pixels;
        return dsc;
    }();
    return &image;
}

void Keyboard::UpdatePinyinCandidates() {
    cands.clear();
    cand_page = 0;
    is_assoc = false;
    if (py_buf.empty()) return;
    constexpr size_t kPhraseCount = sizeof(kPinyinPhrases) / sizeof(kPinyinPhrases[0]);
    constexpr size_t kDictCount = sizeof(kPinyinDict) / sizeof(kPinyinDict[0]);

    // 1. 多字词全拼精确匹配（如 "santi" -> "三体"、"luxun" -> "鲁迅"）放在最前面
    for (size_t i = 0; i < kPhraseCount; ++i) {
        if (py_buf == kPinyinPhrases[i].py) {
            cands.emplace_back(kPinyinPhrases[i].word);
        }
    }
    // 2. 多字词前缀匹配（输入 ≥ 4 个字母时，如 "honglou" -> "红楼梦"）
    if (py_buf.size() >= 4) {
        for (size_t i = 0; i < kPhraseCount && cands.size() < 3; ++i) {
            if (std::strncmp(kPinyinPhrases[i].py, py_buf.c_str(), py_buf.size()) == 0 &&
                py_buf != kPinyinPhrases[i].py) {
                if (std::find(cands.begin(), cands.end(), kPinyinPhrases[i].word) == cands.end()) {
                    cands.emplace_back(kPinyinPhrases[i].word);
                }
            }
        }
    }
    // 3. 单字精确匹配完整音节
    for (size_t i = 0; i < kDictCount; ++i) {
        if (py_buf == kPinyinDict[i].py) {
            SplitUtf8Chars(kPinyinDict[i].chars, cands);
            break;
        }
    }
    // 4. 若当前是连拼且无完整单字音节（如 "sant"），取最长合法首音节的单字兜底，并支持前缀匹配
    for (size_t i = 0; i < kDictCount && cands.size() < 42; ++i) {
        if (std::strncmp(kPinyinDict[i].py, py_buf.c_str(), py_buf.size()) == 0 &&
            py_buf != kPinyinDict[i].py) {
            std::vector<std::string> sub;
            SplitUtf8Chars(kPinyinDict[i].chars, sub);
            const size_t take = std::min<size_t>(sub.size(), 4);
            for (size_t j = 0; j < take; ++j) {
                if (std::find(cands.begin(), cands.end(), sub[j]) == cands.end()) {
                    cands.push_back(sub[j]);
                }
            }
        }
    }
}

void Keyboard::UpdateAssocCandidates(const std::string& last_utf8_word) {
    cands.clear();
    cand_page = 0;
    is_assoc = false;
    if (last_utf8_word.empty()) return;
    // 取刚上屏的最后一个 UTF-8 汉字查找下一字联想
    std::vector<std::string> chars;
    SplitUtf8Chars(last_utf8_word.c_str(), chars);
    if (chars.empty()) return;
    const std::string& tail_ch = chars.back();
    constexpr size_t kAssocCount = sizeof(kPinyinAssoc) / sizeof(kPinyinAssoc[0]);
    for (size_t i = 0; i < kAssocCount; ++i) {
        if (tail_ch == kPinyinAssoc[i].ch) {
            SplitUtf8Chars(kPinyinAssoc[i].nexts, cands);
            is_assoc = !cands.empty();
            break;
        }
    }
}

void Keyboard::RefreshCandBar() {
    if (!cand_bar) return;
    if (!allow_zh || !zh_mode || mode != KbMode::kEn) {
        lv_obj_add_flag(cand_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(cand_bar, LV_OBJ_FLAG_HIDDEN);
    if (py_label) {
        if (!py_buf.empty()) {
            lv_label_set_text(py_label, py_buf.c_str());
        } else if (is_assoc && !cands.empty()) {
            lv_label_set_text(py_label, "联想");
        } else {
            lv_label_set_text(py_label, "拼音");
        }
    }
    const int total = static_cast<int>(cands.size());
    const int start = cand_page * kCandSlots;
    for (int i = 0; i < kCandSlots; ++i) {
        if (!cand_btns[i]) continue;
        auto* lbl = lv_obj_get_child(cand_btns[i], 0);
        if (start + i < total) {
            if (lbl) lv_label_set_text(lbl, cands[static_cast<size_t>(start + i)].c_str());
            lv_obj_clear_flag(cand_btns[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            if (lbl) lv_label_set_text(lbl, "");
            lv_obj_add_flag(cand_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (prev_btn) {
        if (cand_page > 0) lv_obj_clear_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (next_btn) {
        if (start + kCandSlots < total) lv_obj_clear_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void Keyboard::PickCandidate(int slot) {
    const int idx = cand_page * kCandSlots + slot;
    if (idx < 0 || idx >= static_cast<int>(cands.size())) return;
    const std::string chosen = cands[static_cast<size_t>(idx)];
    py_buf.clear();
    UpdateAssocCandidates(chosen);
    RefreshCandBar();
    callback(ScreenKeyboardEvent::kInsert, chosen.c_str(), context);
}

void Keyboard::ClearPinyin() {
    py_buf.clear();
    cands.clear();
    cand_page = 0;
    is_assoc = false;
    RefreshCandBar();
}

void Keyboard::Fill(KbKeyDesc out[kKbKeysCap]) {
    for (int i = 0; i < kKbKeysCap; ++i) {
        out[i] = {"", KbAction::kEmpty};
    }
    auto put = [&](int index, const char* label, KbAction action) {
        out[index] = {label, action};
    };
    auto chars = [&](int row, const char* text) {
        static char labels[kKbKeysCap][2];
        for (int c = 0; text[c] != '\0' && c < kKbCols; ++c) {
            const int index = row * kKbCols + c;
            labels[index][0] = text[c];
            labels[index][1] = '\0';
            put(index, labels[index], KbAction::kChar);
        }
    };
    if (mode == KbMode::kEn) {
        const bool show_upper = !zh_mode && upper;
        chars(0, show_upper ? "QWERTYUIOP" : "qwertyuiop");
        chars(1, show_upper ? "ASDFGHJKL" : "asdfghjkl");
        chars(2, show_upper ? " ZXCVBNM" : " zxcvbnm");
        put(20, "", KbAction::kShift);
        put(28, "", KbAction::kBackspace);
        put(30, "?123", KbAction::kModeSym);
        if (allow_zh) {
            put(31, "", KbAction::kToggleLang);
            put(32, zh_mode ? "空格" : "Space", KbAction::kSpace);
            put(33, "", KbAction::kConfirm);
        } else {
            put(31, "Space", KbAction::kSpace);
            put(32, "", KbAction::kConfirm);
        }
    } else {
        const bool more = mode == KbMode::kMore;
        chars(0, more ? "[]{}<>^*+=" : "1234567890");
        chars(1, more ? "_\\|~\x60%" : "-/:;()$&@");
        chars(2, ".,?!'\"#");
        put(27, "", KbAction::kBackspace);
        put(30, "abc", KbAction::kModeEn);
        put(31, more ? "123" : "#+=", more ? KbAction::kModeSym : KbAction::kModeMore);
        put(32, "Space", KbAction::kSpace);
        put(33, "", KbAction::kConfirm);
    }
}

void Keyboard::Pressed(lv_event_t* e) {
    const auto action = static_cast<KbAction>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* lbl = (btn != nullptr) ? lv_obj_get_child(btn, 0) : nullptr;
    const char* label = (lbl != nullptr) ? lv_label_get_text(lbl) : nullptr;

    switch (action) {
        case KbAction::kBackspace:
            if (allow_zh && zh_mode && !py_buf.empty()) {
                py_buf.pop_back();
                UpdatePinyinCandidates();
                RefreshCandBar();
            } else {
                if (is_assoc) ClearPinyin();
                callback(ScreenKeyboardEvent::kBackspace, "", context);
            }
            break;
        case KbAction::kSpace:
            if (allow_zh && zh_mode && !py_buf.empty()) {
                if (!cands.empty()) {
                    PickCandidate(0);
                } else {
                    const std::string raw = py_buf;
                    ClearPinyin();
                    callback(ScreenKeyboardEvent::kInsert, raw.c_str(), context);
                }
            } else {
                callback(ScreenKeyboardEvent::kInsert, " ", context);
            }
            break;
        case KbAction::kChar:
            if (label != nullptr) {
                if (allow_zh && zh_mode && mode == KbMode::kEn && label[0] >= 'a' && label[0] <= 'z') {
                    if (py_buf.size() < 12) {
                        py_buf.push_back(label[0]);
                        UpdatePinyinCandidates();
                        RefreshCandBar();
                    }
                } else {
                    callback(ScreenKeyboardEvent::kInsert, label, context);
                }
            }
            break;
        default:
            break;
    }
}

void Keyboard::Clicked(lv_event_t* e) {
    const auto action = static_cast<KbAction>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
    if (timer != nullptr) {
        pending_action = action;
        return;
    }
    Apply(action);
}

void Keyboard::Apply(KbAction action) {
    switch (action) {
        case KbAction::kShift:
            if (allow_zh && zh_mode) {
                // 在中文模式下按 Shift：若当前有拼音串，直接把原始英文串上屏；否则切换到英文大写
                if (!py_buf.empty()) {
                    const std::string raw = py_buf;
                    ClearPinyin();
                    callback(ScreenKeyboardEvent::kInsert, raw.c_str(), context);
                    return;
                }
                zh_mode = false;
                upper = true;
            } else {
                upper = !upper;
            }
            Rebuild();
            break;
        case KbAction::kToggleLang:
            if (!py_buf.empty()) {
                const std::string raw = py_buf;
                ClearPinyin();
                callback(ScreenKeyboardEvent::kInsert, raw.c_str(), context);
            }
            zh_mode = !zh_mode;
            upper = false;
            Rebuild();
            break;
        case KbAction::kModeEn:
            mode = KbMode::kEn;
            Rebuild();
            break;
        case KbAction::kModeSym:
            ClearPinyin();
            mode = KbMode::kSym;
            Rebuild();
            break;
        case KbAction::kModeMore:
            ClearPinyin();
            mode = KbMode::kMore;
            Rebuild();
            break;
        case KbAction::kConfirm:
            if (allow_zh && zh_mode && !py_buf.empty()) {
                if (!cands.empty()) {
                    PickCandidate(0);
                } else {
                    const std::string raw = py_buf;
                    ClearPinyin();
                    callback(ScreenKeyboardEvent::kInsert, raw.c_str(), context);
                }
            }
            callback(ScreenKeyboardEvent::kConfirm, "", context);
            break;
        default:
            break;
    }
}

void Keyboard::AddIcon(lv_obj_t* btn, KbAction action) {
    static const lv_point_precise_t check[] = {{0, UiSx(11)}, {UiSx(8), UiSx(19)}, {UiSx(25), 0}};
    static const lv_point_precise_t shift[] = {
        {0, UiSx(12)}, {UiSx(12), 0}, {UiSx(24), UiSx(12)},
        {UiSx(17), UiSx(12)}, {UiSx(17), UiSx(24)},
        {UiSx(7), UiSx(24)}, {UiSx(7), UiSx(12)}, {0, UiSx(12)}};
    static const lv_point_precise_t erase[] = {
        {0, UiSx(10)}, {UiSx(9), 0}, {UiSx(29), 0},
        {UiSx(29), UiSx(20)}, {UiSx(9), UiSx(20)}, {0, UiSx(10)}};
    static const lv_point_precise_t cross_a[] = {{UiSx(14), UiSx(5)}, {UiSx(23), UiSx(15)}};
    static const lv_point_precise_t cross_b[] = {{UiSx(23), UiSx(5)}, {UiSx(14), UiSx(15)}};
    auto line = [&](lv_obj_t* parent, const lv_point_precise_t* points, int count) {
        lv_obj_t* obj = lv_line_create(parent);
        lv_line_set_points(obj, points, count);
        lv_obj_set_style_line_color(obj, lv_color_black(), 0);
        lv_obj_set_style_line_width(obj, UiSx(2), 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        return obj;
    };
    lv_obj_t* icon = nullptr;
    if (action == KbAction::kConfirm) {
        icon = line(btn, check, 3);
    } else if (action == KbAction::kShift) {
        icon = line(btn, shift, 8);
    } else if (action == KbAction::kBackspace) {
        icon = line(btn, erase, 6);
        lv_obj_t* a = line(icon, cross_a, 2);
        lv_obj_t* b = line(icon, cross_b, 2);
        lv_obj_set_pos(a, 0, 0);
        lv_obj_set_pos(b, 0, 0);
    }
    if (icon != nullptr) {
        lv_obj_center(icon);
    }
}

void Keyboard::Layout() {
    if (root == nullptr) {
        return;
    }
    const int w = lv_obj_get_content_width(root);
    const int h = lv_obj_get_content_height(root);
    const bool has_bar = allow_zh && zh_mode && mode == KbMode::kEn;
    const int top_off = has_bar ? (kCandBarH + kKbGap) : 0;
    // 4 行按键始终保持恒定高度（不因候选栏显隐而挤压按键），仅在有候选栏时整体把弹窗往上顶
    const int avail_h = h - top_off;
    const int key_h = (avail_h - kKbGap * (kKbRows - 1)) / kKbRows;
    if (w <= 0 || key_h <= 0) {
        return;
    }
    if (cand_bar) {
        lv_obj_set_size(cand_bar, w, kCandBarH);
        lv_obj_set_pos(cand_bar, 0, 0);
    }
    for (int r = 0; r < kKbRows; ++r) {
        int count = 0;
        int weights[kKbCols] = {};
        int total = 0;
        for (int c = 0; c < kKbCols; ++c) {
            if (keys[r * kKbCols + c] == nullptr) {
                break;
            }
            int weight = 100;
            if (r == 2 && mode == KbMode::kEn && (c == 0 || c == 8)) {
                weight = 148;
            } else if (r == 2 && c == 7 && mode != KbMode::kEn) {
                weight = 148;
            } else if (r == 3) {
                const int space = (mode == KbMode::kEn && !allow_zh) ? 1 : 2;
                weight = c < space ? 165 : c == space ? 420 : 190;
            }
            weights[count++] = weight;
            total += weight;
        }
        if (count == 0) {
            continue;
        }
        const bool inset = r == 1 || (r == 2 && mode != KbMode::kEn);
        const int row_w = inset ? w * 905 / 1000 : w;
        const int usable = row_w - kKbGap * (count - 1);
        if (usable <= 0) {
            continue;
        }
        int sum = 0;
        for (int c = 0; c < count; ++c) {
            const int left = usable * sum / total;
            sum += weights[c];
            lv_obj_t* key = keys[r * kKbCols + c];
            lv_obj_set_size(key, usable * sum / total - left, key_h);
            lv_obj_set_pos(key, (w - row_w) / 2 + left + c * kKbGap, top_off + r * (key_h + kKbGap));
        }
    }
    callback(ScreenKeyboardEvent::kLayout, "", context);
}

void Keyboard::Rebuild() {
    if (root == nullptr) {
        return;
    }
    Stop();
    lv_obj_clean(root);
    cand_bar = nullptr;
    py_label = nullptr;
    prev_btn = nullptr;
    next_btn = nullptr;
    for (int i = 0; i < kCandSlots; ++i) cand_btns[i] = nullptr;
    for (int i = 0; i < kKbKeysCap; ++i) {
        keys[i] = nullptr;
    }

    const lv_font_t* font = fontpack_lv_font_get(30, 2);
    if (font == nullptr) font = fontpack_lv_font_ui();
    const lv_font_t* small_font = fontpack_lv_font_get(25, 2);
    if (small_font == nullptr) small_font = font;

    // 拼音候选词栏
    if (allow_zh && zh_mode && mode == KbMode::kEn) {
        cand_bar = lv_obj_create(root);
        Strip(cand_bar);
        lv_obj_set_style_bg_color(cand_bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cand_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cand_bar, lv_color_black(), 0);
        lv_obj_set_style_border_width(cand_bar, 1, 0);
        lv_obj_set_style_radius(cand_bar, UiSx(7), 0);
        lv_obj_set_style_pad_hor(cand_bar, UiSx(10), 0);
        lv_obj_set_flex_flow(cand_bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cand_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(cand_bar, UiSx(6), 0);

        py_label = lv_label_create(cand_bar);
        lv_obj_set_style_text_font(py_label, small_font, 0);
        lv_obj_set_style_text_color(py_label, lv_color_black(), 0);
        lv_obj_set_width(py_label, UiSx(96));
        lv_label_set_long_mode(py_label, LV_LABEL_LONG_CLIP);

        for (int i = 0; i < kCandSlots; ++i) {
            auto* cbtn = lv_obj_create(cand_bar);
            Strip(cbtn);
            cand_btns[i] = cbtn;
            lv_obj_set_height(cbtn, kCandBarH - UiSy(8));
            lv_obj_set_flex_grow(cbtn, 1);
            lv_obj_set_style_radius(cbtn, UiSx(5), 0);
            lv_obj_add_flag(cbtn, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(cbtn);
            lv_obj_set_user_data(cbtn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            lv_obj_add_event_cb(cbtn, [](lv_event_t* e) {
                auto* kb = static_cast<Keyboard*>(lv_event_get_user_data(e));
                const int slot = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
                kb->PickCandidate(slot);
            }, LV_EVENT_CLICKED, this);
            auto* clbl = lv_label_create(cbtn);
            lv_obj_set_style_text_font(clbl, font, 0);
            lv_obj_set_style_text_color(clbl, lv_color_black(), 0);
            lv_obj_center(clbl);
            lv_obj_clear_flag(clbl, LV_OBJ_FLAG_CLICKABLE);
        }

        auto make_page_btn = [&](const char* txt, int delta) {
            auto* pbtn = lv_obj_create(cand_bar);
            Strip(pbtn);
            lv_obj_set_size(pbtn, UiSx(40), kCandBarH - UiSy(10));
            lv_obj_set_style_border_color(pbtn, lv_color_black(), 0);
            lv_obj_set_style_border_width(pbtn, 1, 0);
            lv_obj_set_style_radius(pbtn, UiSx(5), 0);
            lv_obj_add_flag(pbtn, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(pbtn);
            lv_obj_set_user_data(pbtn, reinterpret_cast<void*>(static_cast<intptr_t>(delta)));
            lv_obj_add_event_cb(pbtn, [](lv_event_t* e) {
                auto* kb = static_cast<Keyboard*>(lv_event_get_user_data(e));
                const int d = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
                const int max_page = (static_cast<int>(kb->cands.size()) - 1) / kCandSlots;
                kb->cand_page = std::clamp(kb->cand_page + d, 0, std::max(0, max_page));
                kb->RefreshCandBar();
            }, LV_EVENT_CLICKED, this);
            auto* plbl = lv_label_create(pbtn);
            lv_label_set_text(plbl, txt);
            lv_obj_set_style_text_font(plbl, small_font, 0);
            lv_obj_set_style_text_color(plbl, lv_color_black(), 0);
            lv_obj_center(plbl);
            lv_obj_clear_flag(plbl, LV_OBJ_FLAG_CLICKABLE);
            return pbtn;
        };
        prev_btn = make_page_btn("<", -1);
        next_btn = make_page_btn(">", 1);
        RefreshCandBar();
    }

    const int nkeys = kKbKeysCap;
    KbKeyDesc layout[kKbKeysCap];
    Fill(layout);

    // 4 行按键区域固定高度 UiSx(260)；当中文候选栏显示时，root 高度向上扩展 (kCandBarH + kKbGap)，
    // 使 4 行按键的宽高与间距在中/英切换时完全不变，只把上方弹窗往上顶一段。
    const bool has_bar = allow_zh && zh_mode && mode == KbMode::kEn;
    lv_obj_set_height(root, UiSx(260) + (has_bar ? (kCandBarH + kKbGap) : 0));

    for (int i = 0; i < nkeys; ++i) {
        const KbKeyDesc& d = layout[i];
        if (d.action == KbAction::kEmpty) {
            continue;
        }
        lv_obj_t* btn = lv_obj_create(root);
        Strip(btn);
        keys[i] = btn;
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, UiSx(7), 0);
        lv_obj_set_style_bg_image_src(btn, PressedImage(), LV_STATE_USER_1);
        lv_obj_set_style_bg_image_tiled(btn, true, LV_STATE_USER_1);
        lv_obj_set_style_bg_image_opa(btn, LV_OPA_COVER, LV_STATE_USER_1);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) { static_cast<Keyboard*>(lv_event_get_user_data(e))->Feedback(e); }, LV_EVENT_PRESSED, this);
        HapticAttachClick(btn);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<uintptr_t>(d.action)));
        if (d.action == KbAction::kChar || d.action == KbAction::kSpace ||
            d.action == KbAction::kBackspace) {
            lv_obj_add_event_cb(btn, [](lv_event_t* e) { static_cast<Keyboard*>(lv_event_get_user_data(e))->Pressed(e); }, LV_EVENT_PRESSED, this);
        } else if (d.action == KbAction::kShift || d.action == KbAction::kModeEn ||
                   d.action == KbAction::kModeSym || d.action == KbAction::kModeMore ||
                   d.action == KbAction::kToggleLang || d.action == KbAction::kConfirm) {
            lv_obj_add_event_cb(btn, [](lv_event_t* e) { static_cast<Keyboard*>(lv_event_get_user_data(e))->Clicked(e); }, LV_EVENT_CLICKED, this);
        }
        if (d.action == KbAction::kBackspace) {
            lv_obj_add_event_cb(btn, [](lv_event_t* e) {
                auto* kb = static_cast<Keyboard*>(lv_event_get_user_data(e));
                kb->ClearPinyin();
                kb->callback(ScreenKeyboardEvent::kClear, "", kb->context);
            }, LV_EVENT_LONG_PRESSED, this);
        }

        if (d.action == KbAction::kToggleLang) {
            // 斜切反色双字按钮：左半「中」、右半「英」，当前激活的一侧黑底白字且字号更大
            // 先用圆角矩形画黑底（确保外侧两个圆角完整），再用白色梯形覆盖未激活的一侧，最后叠上圆角黑框
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_add_event_cb(btn, [](lv_event_t* e) {
                if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
                auto* kb = static_cast<Keyboard*>(lv_event_get_user_data(e));
                lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
                lv_layer_t* layer = lv_event_get_layer(e);
                if (!kb || !obj || !layer) return;
                lv_area_t a;
                lv_obj_get_coords(obj, &a);
                const lv_coord_t bw = a.x2 - a.x1;
                const lv_coord_t mid_x = a.x1 + bw / 2;
                const lv_coord_t slant = UiSx(8);
                const lv_coord_t top_x = mid_x + slant;
                const lv_coord_t bot_x = mid_x - slant;
                const lv_coord_t rad = UiSx(7);

                // 1. 先在激活侧画带圆角的黑色底座，保证左右外侧圆角与相邻按键完全一致
                lv_draw_rect_dsc_t blk;
                lv_draw_rect_dsc_init(&blk);
                blk.bg_color = lv_color_black();
                blk.bg_opa = LV_OPA_COVER;
                blk.radius = rad;
                lv_draw_rect(layer, &blk, &a);

                // 2. 在非激活侧覆盖白色梯形（外侧延伸 1px 清除黑色底，随后第 4 步统一画圆角外框）
                lv_point_precise_t p[4];
                if (kb->zh_mode) {
                    // 中文激活（左黑右白）：把右半梯形涂白
                    p[0] = {static_cast<lv_value_precise_t>(top_x), static_cast<lv_value_precise_t>(a.y1)};
                    p[1] = {static_cast<lv_value_precise_t>(a.x2 + 1), static_cast<lv_value_precise_t>(a.y1)};
                    p[2] = {static_cast<lv_value_precise_t>(a.x2 + 1), static_cast<lv_value_precise_t>(a.y2 + 1)};
                    p[3] = {static_cast<lv_value_precise_t>(bot_x), static_cast<lv_value_precise_t>(a.y2 + 1)};
                } else {
                    // 英文激活（左白右黑）：把左半梯形涂白
                    p[0] = {static_cast<lv_value_precise_t>(a.x1 - 1), static_cast<lv_value_precise_t>(a.y1)};
                    p[1] = {static_cast<lv_value_precise_t>(top_x), static_cast<lv_value_precise_t>(a.y1)};
                    p[2] = {static_cast<lv_value_precise_t>(bot_x), static_cast<lv_value_precise_t>(a.y2 + 1)};
                    p[3] = {static_cast<lv_value_precise_t>(a.x1 - 1), static_cast<lv_value_precise_t>(a.y2 + 1)};
                }
                lv_draw_triangle_dsc_t tri;
                lv_draw_triangle_dsc_init(&tri);
                tri.color = lv_color_white();
                tri.opa = LV_OPA_COVER;
                tri.p[0] = p[0];
                tri.p[1] = p[1];
                tri.p[2] = p[2];
                lv_draw_triangle(layer, &tri);
                tri.p[0] = p[0];
                tri.p[1] = p[2];
                tri.p[2] = p[3];
                lv_draw_triangle(layer, &tri);

                // 3. 斜切分割线
                lv_draw_line_dsc_t ld;
                lv_draw_line_dsc_init(&ld);
                ld.color = lv_color_black();
                ld.opa = LV_OPA_COVER;
                ld.width = 1;
                ld.p1 = {static_cast<lv_value_precise_t>(top_x), static_cast<lv_value_precise_t>(a.y1 + 1)};
                ld.p2 = {static_cast<lv_value_precise_t>(bot_x), static_cast<lv_value_precise_t>(a.y2 - 1)};
                lv_draw_line(layer, &ld);

                // 4. 在最上层绘制完整的圆角矩形边框，使左右两侧四个角都是干净对称的圆角
                lv_draw_rect_dsc_t brd;
                lv_draw_rect_dsc_init(&brd);
                brd.bg_opa = LV_OPA_TRANSP;
                brd.border_color = lv_color_black();
                brd.border_width = 1;
                brd.border_opa = LV_OPA_COVER;
                brd.radius = rad;
                lv_draw_rect(layer, &brd, &a);
            }, LV_EVENT_DRAW_MAIN, this);

            lv_obj_t* lbl_zh = lv_label_create(btn);
            lv_label_set_text(lbl_zh, "中");
            lv_obj_set_style_text_font(lbl_zh, zh_mode ? font : small_font, 0);
            lv_obj_set_style_text_color(lbl_zh, zh_mode ? lv_color_white() : lv_color_black(), 0);
            lv_obj_align(lbl_zh, LV_ALIGN_CENTER, -UiSx(23), 0);
            lv_obj_clear_flag(lbl_zh, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* lbl_en = lv_label_create(btn);
            lv_label_set_text(lbl_en, "英");
            lv_obj_set_style_text_font(lbl_en, zh_mode ? small_font : font, 0);
            lv_obj_set_style_text_color(lbl_en, zh_mode ? lv_color_black() : lv_color_white(), 0);
            lv_obj_align(lbl_en, LV_ALIGN_CENTER, UiSx(23), 0);
            lv_obj_clear_flag(lbl_en, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, d.label != nullptr ? d.label : "");
            if (font != nullptr) {
                lv_obj_set_style_text_font(lbl, font, 0);
            }
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            AddIcon(btn, d.action);
            if (d.action == KbAction::kShift && !zh_mode && upper) {
                lv_obj_set_style_border_width(btn, UiSx(2), 0);
            }
        }
    }
    Layout();
}
} // namespace

lv_obj_t* ScreenKeyboardCreate(lv_obj_t* parent, ScreenKeyboardCallback callback, void* context) {
    auto* kb = new (std::nothrow) Keyboard;
    if (kb == nullptr) return nullptr;
    kb->root = lv_obj_create(parent);
    kb->callback = callback;
    kb->context = context;
    Strip(kb->root);
    lv_obj_set_user_data(kb->root, kb);
    lv_obj_set_width(kb->root, lv_pct(100));
    lv_obj_set_height(kb->root, UiSx(260));
    lv_obj_set_style_bg_color(kb->root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(kb->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(kb->root, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(kb->root, 1, 0);
    lv_obj_set_style_border_color(kb->root, lv_color_black(), 0);
    lv_obj_set_style_pad_top(kb->root, UiSy(8), 0);
    lv_obj_add_event_cb(kb->root, [](lv_event_t* e) {
        auto* kb = static_cast<Keyboard*>(lv_event_get_user_data(e));
        if (lv_event_get_target_obj(e) != kb->root) return;
        if (lv_event_get_code(e) == LV_EVENT_DELETE) {
            kb->Stop();
            delete kb;
        } else if (lv_event_get_code(e) == LV_EVENT_SIZE_CHANGED) kb->Layout();
    }, LV_EVENT_ALL, kb);
    return kb->root;
}

void ScreenKeyboardReset(lv_obj_t* root) {
    if (root == nullptr) return;
    auto* kb = static_cast<Keyboard*>(lv_obj_get_user_data(root));
    kb->mode = KbMode::kEn;
    kb->upper = false;
    kb->py_buf.clear();
    kb->cands.clear();
    kb->cand_page = 0;
    kb->Rebuild();
}

void ScreenKeyboardEnableChinese(lv_obj_t* root, bool default_zh) {
    if (root == nullptr) return;
    auto* kb = static_cast<Keyboard*>(lv_obj_get_user_data(root));
    kb->allow_zh = true;
    kb->zh_mode = default_zh;
    kb->mode = KbMode::kEn;
    kb->upper = false;
    kb->py_buf.clear();
    kb->cands.clear();
    kb->cand_page = 0;
    kb->Rebuild();
}

bool ScreenKeyboardHasCandidateBar(lv_obj_t* root) {
    if (root == nullptr) return false;
    auto* kb = static_cast<Keyboard*>(lv_obj_get_user_data(root));
    return kb != nullptr && kb->allow_zh && kb->zh_mode && kb->mode == KbMode::kEn;
}

void ScreenKeyboardShowSymbols(lv_obj_t* root) {
    if (root == nullptr) return;
    auto* kb = static_cast<Keyboard*>(lv_obj_get_user_data(root));
    kb->mode = KbMode::kSym;
    kb->upper = false;
    kb->py_buf.clear();
    kb->cands.clear();
    kb->cand_page = 0;
    kb->Rebuild();
}

void ScreenKeyboardPause(lv_obj_t* root) {
    if (root == nullptr) return;
    auto* kb = static_cast<Keyboard*>(lv_obj_get_user_data(root));
    kb->Stop();
    for (auto* key : kb->keys) if (key != nullptr) lv_obj_remove_state(key, LV_STATE_USER_1);
}
