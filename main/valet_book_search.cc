#include "valet_book_search.h"

#include <cstdio>
#include <cstdlib>

namespace valet::booksearch {
namespace {
constexpr size_t kAnchorMatch = 48;
bool Space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
bool Continuation(unsigned char c) { return (c & 0xc0) == 0x80; }
// 全角空格（U+3000）也当空白
size_t SpaceAt(const std::string& s, size_t i) {
    if (i < s.size() && Space(static_cast<unsigned char>(s[i]))) return 1;
    if (s.compare(i, 3, "\xe3\x80\x80") == 0) return 3;
    return 0;
}
std::string StripSpaces(const std::string& s, size_t max) {
    std::string out;
    for (size_t i = 0; i < s.size() && out.size() < max;) {
        if (const size_t n = SpaceAt(s, i)) { i += n; continue; }
        out.push_back(s[i++]);
    }
    while (!out.empty() && Continuation(static_cast<unsigned char>(out.back()))) out.pop_back();   // 截在字中间
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xc0) out.pop_back();
    return out;
}
}  // namespace

std::string FormatPosition(int chapter, int paragraph) {
    char text[32];
    std::snprintf(text, sizeof(text), "c%d.p%d", chapter, paragraph);
    return text;
}

Position ParsePosition(const std::string& text) {
    Position p;
    const char* s = text.c_str();
    if (*s == 'c') ++s;
    char* end = nullptr;
    const long chapter = std::strtol(s, &end, 10);
    if (end == s || *end != '.') return {};
    s = end + 1;
    if (*s == 'p') ++s;
    const long paragraph = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || chapter < 0 || paragraph < 0 || chapter > 100000 || paragraph > 10000000) return {};
    p.chapter = int(chapter); p.paragraph = int(paragraph);
    return p;
}

std::string NormalizeQuery(const std::string& query) {
    size_t begin = 0, end = query.size();
    while (begin < end && SpaceAt(query, begin)) begin += SpaceAt(query, begin);
    while (end > begin && Space(static_cast<unsigned char>(query[end - 1]))) --end;
    if (end - begin > kMaxQuery || end == begin) return {};
    std::string out = query.substr(begin, end - begin);
    for (char& c : out) c = Lower(c);
    return out;
}

size_t Find(const std::string& paragraph, const std::string& q) {
    if (q.empty() || paragraph.size() < q.size()) return std::string::npos;
    for (size_t i = 0; i + q.size() <= paragraph.size(); ++i) {
        if (Continuation(static_cast<unsigned char>(paragraph[i]))) continue;   // 只从字的开头比
        size_t k = 0;
        while (k < q.size() && Lower(paragraph[i + k]) == q[k]) ++k;
        if (k == q.size()) return i;
    }
    return std::string::npos;
}

std::string ClipUtf8(const std::string& text, size_t max) {
    if (text.size() <= max) return text;
    while (max && Continuation(static_cast<unsigned char>(text[max]))) --max;
    return text.substr(0, max);
}

std::string Excerpt(const std::string& paragraph, size_t at, size_t length, size_t max) {
    if (paragraph.size() <= max) return paragraph;
    static const char kMore[] = "\xe2\x80\xa6";   // …
    const size_t room = max > 6 ? max - 6 : max;
    size_t begin = at + length / 2 > room / 2 ? at + length / 2 - room / 2 : 0;
    if (begin + room > paragraph.size()) begin = paragraph.size() - room;
    while (begin && Continuation(static_cast<unsigned char>(paragraph[begin]))) --begin;
    std::string out = ClipUtf8(paragraph.substr(begin), room);
    const bool cut_tail = begin + out.size() < paragraph.size();
    return (begin ? kMore : "") + out + (cut_tail ? kMore : "");
}

bool ParagraphJoiner::Feed(const std::string& text, bool continuation) {
    if (continuation && open_) { current_ += text; return false; }
    const bool ended = open_;
    if (ended) { done_.swap(current_); done_index_ = index_; }
    current_ = text; ++index_; open_ = true;
    return ended;
}

bool ParagraphJoiner::Flush() {
    if (!open_) return false;
    done_.swap(current_); done_index_ = index_; current_.clear(); open_ = false;
    return true;
}

bool ParagraphHasAnchor(const std::string& paragraph, const std::string& anchor) {
    const std::string a = StripSpaces(anchor, kAnchorMatch);
    if (a.size() < 6) return false;   // 太短（两个字以内）的行认不准
    return StripSpaces(paragraph, paragraph.size()).find(a) != std::string::npos;
}

}  // namespace valet::booksearch
