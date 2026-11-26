#include "opds_client.h"

// 只为解 &amp; 这类实体；单独声明，免得把带 LVGL 的 reader 头拉进来（这样也能在电脑上单测）
namespace reader { std::string DecodeHtmlEntities(const std::string& in); }

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace valet::opds {
namespace {
constexpr const char* kAcquisition = "http://opds-spec.org/acquisition";
constexpr const char* kImage = "http://opds-spec.org/image";
constexpr const char* kThumb = "http://opds-spec.org/image/thumbnail";

bool EndsWith(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

std::string Trim(const std::string& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// 属性值：name="v" 或 name='v'
bool Attr(const std::string& tag, const char* name, std::string& out) {
    const std::string key = std::string(" ") + name + "=";
    size_t at = 0;
    while ((at = tag.find(key, at)) != std::string::npos) {
        // 前面必须是空白，避免匹配到 xxx:href=
        if (at > 0 && !std::isspace(static_cast<unsigned char>(tag[at]))) { at += key.size(); continue; }
        size_t v = at + key.size();
        if (v >= tag.size()) return false;
        const char quote = tag[v];
        if (quote != '"' && quote != '\'') return false;
        const size_t close = tag.find(quote, v + 1);
        if (close == std::string::npos) return false;
        out = tag.substr(v + 1, close - v - 1);
        return true;
    }
    // 标签可能以 name= 开头（去掉前缀 <link 后的字符串）
    const std::string head = std::string(name) + "=";
    if (tag.compare(0, head.size(), head) == 0) {
        size_t v = head.size();
        if (v >= tag.size()) return false;
        const char quote = tag[v];
        if (quote != '"' && quote != '\'') return false;
        const size_t close = tag.find(quote, v + 1);
        if (close == std::string::npos) return false;
        out = tag.substr(v + 1, close - v - 1);
        return true;
    }
    return false;
}

// 取 <tag ...> ... </tag> 的第一个文本内容（不递归）
std::string Inner(const std::string& xml, const std::string& open, const std::string& close, size_t from, size_t to) {
    const size_t at = xml.find(open, from);
    if (at == std::string::npos || at >= to) return {};
    const size_t begin = at + open.size();
    const size_t end = xml.find(close, begin);
    if (end == std::string::npos || end > to) return {};
    return xml.substr(begin, end - begin);
}

std::string StripTags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out.push_back(c);
    }
    return out;
}

void AddAcquisition(Entry& entry, const std::string& href, const std::string& type,
                    const std::string& length, const std::string& base) {
    if (href.empty()) return;
    Acquisition file;
    file.url = href;
    file.type = type;
    file.ext = ExtFromType(type, href);
    if (!length.empty()) file.length = std::strtoull(length.c_str(), nullptr, 10);
    if (file.ext.empty() && file.length == 0) return;   // 认不出的格式也不要紧，别塞满列表
    for (const auto& existing : entry.files) {
        if (existing.url == file.url) return;
    }
    entry.files.push_back(std::move(file));
    (void)base;
}

// 带属性的开始标签（<content type="xhtml">）：从标签的 > 之后开始取内容
std::string InnerTag(const std::string& xml, const std::string& open, const std::string& close) {
    const size_t at = xml.find(open);
    if (at == std::string::npos) return {};
    const size_t tag_end = xml.find('>', at);
    if (tag_end == std::string::npos) return {};
    if (xml.compare(tag_end - 1, 2, "/>") == 0) return {};
    const size_t end = xml.find(close, tag_end);
    if (end == std::string::npos) return {};
    return xml.substr(tag_end + 1, end - tag_end - 1);
}

void ParseEntry(const std::string& xml, const std::string& base, Entry& entry, std::string& search_out) {
    // 标题、作者、简介
    entry.title = CleanText(StripTags(Inner(xml, "<title>", "</title>", 0, xml.size())), kMaxTitle);
    if (entry.title.empty()) {
        std::string t;
        const size_t at = xml.find("<title");
        if (at != std::string::npos) {
            const size_t tag_end = xml.find('>', at);
            const size_t close = xml.find("</title>", tag_end == std::string::npos ? at : tag_end);
            if (tag_end != std::string::npos && close != std::string::npos) {
                std::string tag = xml.substr(at + 6, tag_end - at - 6);
                Attr(tag, "type", t);
                entry.title = CleanText(StripTags(xml.substr(tag_end + 1, close - tag_end - 1)), kMaxTitle);
            }
        }
    }
    const size_t author_at = xml.find("<author>");
    if (author_at != std::string::npos) {
        entry.author = CleanText(StripTags(Inner(xml, "<name>", "</name>", author_at, xml.size())), kMaxAuthor);
    }
    // 先解 HTML 实体（&amp; 等），再去标签，最后清理空白
    entry.summary = CleanText(StripTags(reader::DecodeHtmlEntities(InnerTag(xml, "<content", "</content>"))), kMaxSummary);

    // 链：封面、缩略图、下载、下一页、搜索模板
    size_t pos = 0;
    while ((pos = xml.find("<link", pos)) != std::string::npos) {
        const size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos + 5, end - pos - 5);
        pos = end;
        std::string rel, href, type, length;
        Attr(tag, "rel", rel);
        Attr(tag, "href", href);
        Attr(tag, "type", type);
        Attr(tag, "length", length);
        if (href.empty()) continue;
        if (rel == kImage) { entry.cover = ResolveUrl(base, href); continue; }
        if (rel == kThumb) { entry.thumbnail = ResolveUrl(base, href); continue; }
        if (rel.compare(0, std::strlen(kAcquisition), kAcquisition) == 0) {
            AddAcquisition(entry, ResolveUrl(base, href), type, length, base);
            continue;
        }
        if (rel == "search" && type.find("atom") != std::string::npos && search_out.empty() &&
            href.find("{searchTerms}") != std::string::npos) {
            search_out = ResolveUrl(base, href);
            continue;
        }
        // 目录项（导航 feed 里的「最近添加」「分类」等）：指向另一页 Atom
        if (entry.nav.empty() && type.find("atom+xml") != std::string::npos && rel != "self" &&
            rel != "search" && rel != "alternate" && rel != "related") {
            entry.nav = ResolveUrl(base, href);
        }
    }
    if (entry.id.empty()) {
        entry.id = Inner(xml, "<id>", "</id>", 0, xml.size());
        if (entry.id.empty() && !entry.files.empty()) entry.id = entry.files.front().url;
    }
}

} // namespace

const Acquisition* Entry::Best() const {
    for (const char* want : {"epub", "txt"}) {
        for (const auto& file : files) if (file.ext == want) return &file;
    }
    return nullptr;
}

std::string ResolveUrl(const std::string& base, const std::string& href) {
    if (href.empty()) return {};
    if (href.compare(0, 7, "http://") == 0 || href.compare(0, 8, "https://") == 0) return href;
    // scheme://host 部分
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return href;
    if (href.compare(0, 2, "//") == 0) return base.substr(0, scheme_end + 1) + href;
    const size_t host_start = scheme_end + 3;
    if (href[0] == '/') {
        const size_t path = base.find('/', host_start);
        return (path == std::string::npos ? base : base.substr(0, path)) + href;
    }
    // 相对路径：接在 base 的目录后面
    size_t cut = base.find_first_of("?#", host_start);
    if (cut == std::string::npos) cut = base.size();
    const size_t slash = base.rfind('/', cut);
    if (slash == std::string::npos || slash < host_start) return base + "/" + href;
    return base.substr(0, slash + 1) + href;
}

std::string ExtFromType(const std::string& type, const std::string& href) {
    if (type == "application/epub+zip") return "epub";
    if (type == "text/plain") return "txt";
    if (type.find("mobipocket") != std::string::npos) {
        // 地址常带结尾斜杠（/opds/download/1185/azw3/），先去掉再比
        std::string path = href;
        while (!path.empty() && path.back() == '/') path.pop_back();
        return EndsWith(path, "/azw3") ? "azw3" : "mobi";
    }
    if (type == "application/pdf") return "pdf";
    // 没有 type 时按地址后缀认
    std::string lower = href;
    while (!lower.empty() && lower.back() == '/') lower.pop_back();
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* ext : {".epub", ".txt", ".mobi", ".azw3", ".pdf"}) {
        if (EndsWith(lower, ext)) return std::string(ext + 1);
    }
    return {};
}

std::string CleanText(const std::string& text, size_t max) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '\n' || u == '\r' || u == '\t') { space = true; continue; }
        if (u < 0x20) continue;
        if (space && !out.empty() && out.back() != ' ') out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    std::string trimmed = Trim(out);
    trimmed = reader::DecodeHtmlEntities(trimmed);
    if (trimmed.size() <= max) return trimmed;
    // 按 UTF-8 边界截断
    size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(trimmed[end]) & 0xC0) == 0x80) --end;
    return trimmed.substr(0, end) + "…";
}

bool ParseFeed(const std::string& xml, const std::string& base, Feed& out) {
    out = Feed{};
    if (xml.empty() || xml.size() > kMaxFeedBytes) return false;
    if (xml.find("<feed") == std::string::npos && xml.find("<rss") == std::string::npos) return false;
    const size_t head_end = xml.find("<entry");
    out.title = CleanText(StripTags(Inner(xml, "<title>", "</title>", 0, head_end == std::string::npos ? xml.size() : head_end)), kMaxTitle);
    // feed 级的 next / search
    {
        size_t pos = 0;
        while ((pos = xml.find("<link", pos)) != std::string::npos) {
            if (head_end != std::string::npos && pos > head_end) break;
            const size_t end = xml.find('>', pos);
            if (end == std::string::npos) break;
            std::string tag = xml.substr(pos + 5, end - pos - 5);
            pos = end;
            std::string rel, href, type;
            Attr(tag, "rel", rel);
            Attr(tag, "href", href);
            Attr(tag, "type", type);
            if (href.empty()) continue;
            if (rel == "next" && out.next.empty()) out.next = ResolveUrl(base, href);
            if (rel == "search" && out.search_template.empty() &&
                href.find("{searchTerms}") != std::string::npos) {
                out.search_template = ResolveUrl(base, href);
            }
        }
    }
    size_t pos = 0;
    while (out.entries.size() < static_cast<size_t>(kMaxEntries) &&
           (pos = xml.find("<entry", pos)) != std::string::npos) {
        const size_t body = xml.find('>', pos);
        const size_t close = xml.find("</entry>", body == std::string::npos ? pos : body);
        if (body == std::string::npos || close == std::string::npos) break;
        Entry entry;
        std::string link_search;
        ParseEntry(xml.substr(body, close - body), base, entry, link_search);
        pos = close + 8;
        if (entry.title.empty()) continue;
        if (!entry.files.empty()) entry.nav.clear();
        if (out.search_template.empty() && !link_search.empty()) out.search_template = link_search;
        out.entries.push_back(std::move(entry));
    }
    return true;
}

std::string SearchUrl(const std::string& search_template, const std::string& query) {
    if (search_template.empty() || query.empty()) return {};
    std::string encoded;
    for (unsigned char c : query) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) { encoded.push_back(static_cast<char>(c)); continue; }
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%%%02X", c);
        encoded += hex;
    }
    std::string url = search_template;
    const std::string token = "{searchTerms}";
    const size_t at = url.find(token);
    if (at == std::string::npos) return {};
    url.replace(at, token.size(), encoded);
    return url;
}

} // namespace valet::opds
