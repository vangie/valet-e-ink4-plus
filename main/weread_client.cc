#include "weread_client.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "book_cover_sidecar.h"
#include "http.h"
#include "power_policy.h"
#include "valet_net/proxy.h"
#include "reader/book_library.h"
#include "reader/book_storage_paths.h"
#include "reader/http_download_file.h"
#include "reader/image_util.h"
#include "reader/reader_image_budget.h"
#include "reader_sync.h"
#include "sd_paths.h"
#include "settings.h"
#include "SdCardManager.hpp"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_md5.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <utility>
#include <vector>

namespace valet::weread {
namespace {

constexpr const char* TAG = "WeRead";
constexpr const char* kNvsNs = "weread";
constexpr const char* kHost = "https://weread.qq.com";
constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
constexpr const char* kReaderToken = "3c5c8717f3daf09iop3423zafeqoi";
constexpr size_t kMaxResponseBytes = 3 * 1024 * 1024;

struct ActivationHold {
    ActivationHold() { Application::GetInstance().PauseActivationPolling(); }
    ~ActivationHold() { Application::GetInstance().ResumeActivationPolling(); }
};

struct JsonDeleter {
    void operator()(cJSON* v) const { cJSON_Delete(v); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

// ── PSRAM 缓冲（大响应不占内部 DRAM）────────────────────────────────────
struct PsBuffer {
    char* data = nullptr;
    size_t len = 0;
    size_t cap = 0;
    bool overflow = false;

    ~PsBuffer() { Reset(); }
    void Reset() {
        if (data) heap_caps_free(data);
        data = nullptr;
        len = 0;
        cap = 0;
        overflow = false;
    }
    bool Append(const void* src, size_t n) {
        if (n == 0) return true;
        if (len + n > kMaxResponseBytes) {
            overflow = true;
            return false;
        }
        if (len + n + 1 > cap) {
            size_t next_cap = std::max<size_t>(cap ? cap * 2 : 16384, len + n + 1);
            next_cap = std::min<size_t>(next_cap, kMaxResponseBytes + 1);
            char* next = static_cast<char*>(heap_caps_realloc(data, next_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!next) next = static_cast<char*>(heap_caps_realloc(data, next_cap, MALLOC_CAP_8BIT));
            if (!next) {
                overflow = true;
                return false;
            }
            data = next;
            cap = next_cap;
        }
        std::memcpy(data + len, src, n);
        len += n;
        data[len] = '\0';
        return true;
    }
    std::string ToString() const {
        return (data && len > 0) ? std::string(data, len) : std::string();
    }
};

// ── 凭据管理（NVS + 内存同步）──────────────────────────────────────────
std::mutex g_auth_mu;
struct AuthCreds {
    std::string vid;
    std::string skey;
    std::string rt;
    std::string gid;
    bool loaded = false;
};
AuthCreds g_auth;

void EnsureAuthLoadedLocked() {
    if (g_auth.loaded) return;
    Settings s(kNvsNs, false);
    g_auth.vid = s.GetString("vid");
    g_auth.skey = s.GetString("skey");
    g_auth.rt = s.GetString("rt");
    g_auth.gid = s.GetString("gid");
    g_auth.loaded = true;
}

void SaveAuthLocked() {
    Settings s(kNvsNs, true);
    s.SetString("vid", g_auth.vid);
    s.SetString("skey", g_auth.skey);
    s.SetString("rt", g_auth.rt);
    s.SetString("gid", g_auth.gid);
}

std::string BuildCookieHeader() {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    EnsureAuthLoadedLocked();
    std::string res;
    if (!g_auth.vid.empty()) res += "wr_vid=" + g_auth.vid;
    if (!g_auth.skey.empty()) res += (res.empty() ? "" : "; ") + std::string("wr_skey=") + g_auth.skey;
    if (!g_auth.gid.empty()) res += (res.empty() ? "" : "; ") + std::string("wr_gid=") + g_auth.gid;
    if (!g_auth.rt.empty()) res += (res.empty() ? "" : "; ") + std::string("wr_rt=") + g_auth.rt;
    return res;
}

void AbsorbSetCookies(const std::vector<std::string>& headers) {
    if (headers.empty()) return;
    std::lock_guard<std::mutex> lock(g_auth_mu);
    EnsureAuthLoadedLocked();
    bool changed = false;
    for (const auto& h : headers) {
        const size_t eq = h.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = h.substr(0, eq);
        const size_t semi = h.find(';', eq + 1);
        const std::string val = h.substr(eq + 1, semi == std::string::npos ? std::string::npos : (semi - eq - 1));
        if (val.empty()) continue;
        if (name == "wr_vid" && g_auth.vid != val) { g_auth.vid = val; changed = true; }
        else if (name == "wr_skey" && g_auth.skey != val) { g_auth.skey = val; changed = true; }
        else if (name == "wr_rt" && g_auth.rt != val) { g_auth.rt = val; changed = true; }
        else if (name == "wr_gid" && g_auth.gid != val) { g_auth.gid = val; changed = true; }
    }
    if (changed) SaveAuthLocked();
}

// ── 加密、签名与分片逆置换解密 ──────────────────────────────────────────
std::string Hex(const unsigned char* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string res(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        res[i * 2] = kHex[data[i] >> 4];
        res[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return res;
}

std::string Md5Hex(const void* data, size_t len) {
    unsigned char digest[16] = {};
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, static_cast<const uint8_t*>(data), static_cast<uint32_t>(len));
    esp_rom_md5_final(digest, &ctx);
    return Hex(digest, sizeof(digest));
}

std::string Md5Hex(const std::string& s) { return Md5Hex(s.data(), s.size()); }

std::string Sha256Hex(const std::string& s) {
    unsigned char digest[32] = {};
    size_t out_len = 0;
    psa_crypto_init();
    psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(s.data()), s.size(),
                     digest, sizeof(digest), &out_len);
    return Hex(digest, sizeof(digest));
}

std::string UrlEncode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
    return out;
}

std::string EscapeXml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string EncodeId(const std::string& value) {
    if (value.empty()) return {};
    const std::string digest = Md5Hex(value);
    const bool numeric = std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    std::vector<std::string> chunks;
    if (numeric) {
        for (size_t i = 0; i < value.size(); i += 9) {
            const auto num = std::strtoull(value.substr(i, 9).c_str(), nullptr, 10);
            char buf[24] = {};
            std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(num));
            chunks.emplace_back(buf);
        }
    } else {
        std::string bytes;
        char buf[4] = {};
        for (unsigned char c : value) {
            std::snprintf(buf, sizeof(buf), "%x", c);
            bytes += buf;
        }
        chunks.push_back(std::move(bytes));
    }
    std::string res = digest.substr(0, 3) + (numeric ? "3" : "4") + "2" + digest.substr(30);
    for (size_t i = 0; i < chunks.size(); ++i) {
        char len_buf[4] = {};
        std::snprintf(len_buf, sizeof(len_buf), "%02x", static_cast<unsigned>(chunks[i].size()));
        res += len_buf;
        res += chunks[i];
        if (i + 1 < chunks.size()) res.push_back('g');
    }
    if (res.size() < 20) res += digest.substr(0, 20 - res.size());
    res += Md5Hex(res).substr(0, 3);
    return res;
}

std::string Sign(const std::string& query) {
    int64_t left = 0x15051505;
    int64_t right = left;
    int i = static_cast<int>(query.size());
    while (i > 1) {
        left = (left ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 1])) << ((query.size() - i + 1) % 30))) & 0x7fffffff;
        right = (right ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 2])) << ((i - 1) % 30))) & 0x7fffffff;
        i -= 2;
    }
    char buf[24] = {};
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(left + right));
    return buf;
}

std::string SortedQuery(const std::vector<std::pair<std::string, std::string>>& params) {
    auto sorted = params;
    std::sort(sorted.begin(), sorted.end());
    std::string q;
    for (const auto& [k, v] : sorted) {
        if (k == "s") continue;
        if (!q.empty()) q.push_back('&');
        q += k + "=" + UrlEncode(v);
    }
    return q;
}

std::string ContentPayloadJson(const std::string& book_id, const std::string& chapter_uid, const std::string& psvts) {
    long ts = static_cast<long>(std::time(nullptr));
    if (ts < 1700000000L) ts = 1764000000L;
    if (EncodeId(std::to_string(ts)) == psvts) ++ts;
    const uint32_t rnd = esp_random() % 10000;
    std::vector<std::pair<std::string, std::string>> params = {
        {"b", EncodeId(book_id)},
        {"c", EncodeId(chapter_uid)},
        {"r", std::to_string(rnd * rnd)},
        {"ct", std::to_string(ts)},
        {"ps", psvts},
        {"pc", EncodeId(std::to_string(ts))},
        {"sc", "1"},
        {"prevChapter", "false"},
        {"st", "0"},
    };
    params.emplace_back("s", Sign(SortedQuery(params)));
    static constexpr const char* kNumKeys[] = {"ct", "sc", "st", "r"};
    std::string out = "{";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) out.push_back(',');
        const bool is_num = std::any_of(std::begin(kNumKeys), std::end(kNumKeys),
                                        [&](const char* k) { return params[i].first == k; });
        out += "\"" + params[i].first + "\":";
        out += is_num ? params[i].second : ("\"" + EscapeJson(params[i].second) + "\"");
    }
    out.push_back('}');
    return out;
}

std::vector<int> SwapPositions(const std::string& enc) {
    std::vector<int> pos;
    const int len = static_cast<int>(enc.size());
    if (len < 4) return pos;
    if (len < 11) return {0, 2};
    const int tail = std::min(4, (len + 9) / 10);
    std::string digits;
    for (int i = len - 1; i >= len - tail; --i) {
        const uint8_t val = static_cast<uint8_t>(enc[i]);
        uint32_t expanded = 0;
        for (int b = 0; b < 8; ++b) {
            if ((val >> b) & 1U) expanded += 1U << (b * 2);
        }
        digits += std::to_string(expanded);
    }
    const int mod = len - tail - 2;
    if (mod <= 0) return pos;
    const int width = static_cast<int>(std::to_string(mod).size());
    for (int i = 0; pos.size() < 10 && i + width < static_cast<int>(digits.size()); i += width) {
        pos.push_back(std::atoi(digits.substr(i, width).c_str()) % mod);
        pos.push_back(std::atoi(digits.substr(i + 1, width).c_str()) % mod);
    }
    return pos;
}

bool DecodeShards(const std::vector<std::string>& shards, std::string& output) {
    output.clear();
    std::string payload;
    for (const auto& s : shards) {
        if (s.size() <= 32) continue;
        std::string exp = Md5Hex(s.data() + 32, s.size() - 32);
        for (char& c : exp) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (s.compare(0, 32, exp) == 0) {
            payload.append(s, 32, std::string::npos);
        }
    }
    if (payload.size() < 2) return false;
    std::string enc = payload.substr(1);
    const auto pos = SwapPositions(enc);
    for (int i = static_cast<int>(pos.size()) - 1; i > 0; i -= 2) {
        for (int off = 1; off >= 0; --off) {
            const int a = pos[i] + off;
            const int b = pos[i - 1] + off;
            if (a >= 0 && b >= 0 && a < static_cast<int>(enc.size()) && b < static_cast<int>(enc.size())) {
                std::swap(enc[a], enc[b]);
            }
        }
    }
    std::string b64;
    b64.reserve(enc.size() + 4);
    for (char c : enc) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/') b64.push_back(c);
    }
    while (b64.size() % 4 != 0) b64.push_back('=');
    size_t req_len = 0;
    mbedtls_base64_decode(nullptr, 0, &req_len, reinterpret_cast<const unsigned char*>(b64.data()), b64.size());
    if (req_len == 0) return false;
    output.assign(req_len, '\0');
    size_t written = 0;
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(output.data()), output.size(), &written,
                              reinterpret_cast<const unsigned char*>(b64.data()), b64.size()) != 0) {
        output.clear();
        return false;
    }
    output.resize(written);
    return !output.empty();
}

std::string WebAppId() {
    std::string prefix;
    int words = 0;
    size_t start = 0;
    const std::string ua = kUserAgent;
    for (size_t i = 0; i <= ua.size() && words < 12; ++i) {
        if (i == ua.size() || ua[i] == ' ' || ua[i] == '\t') {
            if (i > start) {
                prefix += std::to_string((i - start) % 10);
                ++words;
            }
            start = i + 1;
        }
    }
    uint32_t hash = 0;
    for (uint8_t c : ua) hash = static_cast<uint32_t>((0x83ULL * hash + c) & 0x7fffffff);
    return "wb" + prefix + "h" + std::to_string(hash);
}

// ── HTTP 会话封装（支持 Keep-Alive 与 Set-Cookie 捕获）──────────────────
struct HttpResp {
    int status = -1;
    PsBuffer body;
    std::vector<std::string> set_cookies;
};

class HttpSession {
public:
    HttpSession() = default;
    ~HttpSession() { Close(); }

    void Close() {
        if (client_) {
            esp_http_client_cleanup(client_);
            client_ = nullptr;
        }
        active_ = nullptr;
    }

    bool Request(const char* method, const std::string& url, const std::string& post_body,
                 const std::string& referer, HttpResp& out, bool absorb_cookies = true, int timeout_ms = 12000) {
        if (RequestOnce(method, url, post_body, referer, out, absorb_cookies, timeout_ms)) return true;
        Close();
        vTaskDelay(pdMS_TO_TICKS(250));
        return RequestOnce(method, url, post_body, referer, out, absorb_cookies, timeout_ms);
    }

private:
    bool RequestOnce(const char* method, const std::string& url, const std::string& post_body,
                     const std::string& referer, HttpResp& out, bool absorb_cookies, int timeout_ms) {
        out.status = -1;
        out.body.Reset();
        out.set_cookies.clear();

        // 当请求不需要捕获 Set-Cookie（如获取扫码 UID、轮询扫码状态、搜索书籍）或设备启用了代理时，
        // 优先走 Board::GetInstance().GetNetwork()->CreateHttp(0)，确保代理与双网连接策略生效。
        if (!absorb_cookies || valet::proxy::InForce()) {
            auto* network = Board::GetInstance().GetNetwork();
            auto http = network ? network->CreateHttp(0) : nullptr;
            if (http) {
                http->SetTimeout(timeout_ms);
                http->SetKeepAlive(false);
                http->SetHeader("User-Agent", kUserAgent);
                http->SetHeader("Accept", "application/json, text/plain, */*");
                http->SetHeader("Origin", kHost);
                http->SetHeader("Referer", referer.empty() ? "https://weread.qq.com/" : referer);
                http->SetHeader("Connection", "close");
                const std::string cookie = BuildCookieHeader();
                if (!cookie.empty()) http->SetHeader("Cookie", cookie);
                if (std::strcmp(method, "POST") == 0) {
                    http->SetHeader("Content-Type", "application/json;charset=UTF-8");
                    std::string copy = post_body;
                    http->SetContent(std::move(copy));
                }
                if (http->Open(method, url)) {
                    out.status = http->GetStatusCode();
                    const std::string sc = http->GetResponseHeader("set-cookie");
                    if (!sc.empty()) out.set_cookies.push_back(sc);
                    char buf[2048];
                    while (true) {
                        const int n = http->Read(buf, sizeof(buf));
                        if (n <= 0) break;
                        if (!out.body.Append(buf, static_cast<size_t>(n))) break;
                    }
                    http->Close();
                    if (absorb_cookies && !out.set_cookies.empty()) {
                        AbsorbSetCookies(out.set_cookies);
                    }
                    if (!out.body.overflow && out.status >= 200 && out.status < 300) {
                        return true;
                    }
                } else {
                    http->Close();
                }
            }
        }

        if (!client_) {
            esp_http_client_config_t cfg = {};
            cfg.url = kHost;
            cfg.user_agent = kUserAgent;
            cfg.timeout_ms = timeout_ms;
            cfg.buffer_size = 4096;
            cfg.buffer_size_tx = 2048;
            cfg.keep_alive_enable = true;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.user_data = this;
            cfg.event_handler = [](esp_http_client_event_t* ev) -> esp_err_t {
                auto* self = static_cast<HttpSession*>(ev->user_data);
                auto* resp = self ? self->active_ : nullptr;
                if (!resp) return ESP_OK;
                if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && ev->header_value) {
                    if (strcasecmp(ev->header_key, "set-cookie") == 0) {
                        resp->set_cookies.emplace_back(ev->header_value);
                    }
                } else if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data && ev->data_len > 0) {
                    resp->body.Append(ev->data, static_cast<size_t>(ev->data_len));
                }
                return ESP_OK;
            };
            client_ = esp_http_client_init(&cfg);
            if (!client_) return false;
        }

        active_ = &out;
        esp_http_client_set_timeout_ms(client_, timeout_ms);
        esp_http_client_set_url(client_, url.c_str());
        esp_http_client_set_header(client_, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(client_, "Origin", kHost);
        const std::string cookie = BuildCookieHeader();
        if (!cookie.empty()) esp_http_client_set_header(client_, "Cookie", cookie.c_str());
        else esp_http_client_delete_header(client_, "Cookie");
        if (!referer.empty()) esp_http_client_set_header(client_, "Referer", referer.c_str());
        else esp_http_client_set_header(client_, "Referer", "https://weread.qq.com/");

        if (std::strcmp(method, "POST") == 0) {
            esp_http_client_set_method(client_, HTTP_METHOD_POST);
            esp_http_client_set_header(client_, "Content-Type", "application/json;charset=UTF-8");
            esp_http_client_set_post_field(client_, post_body.data(), static_cast<int>(post_body.size()));
        } else {
            esp_http_client_set_method(client_, HTTP_METHOD_GET);
            esp_http_client_delete_header(client_, "Content-Type");
            esp_http_client_set_post_field(client_, nullptr, 0);
        }

        const esp_err_t err = esp_http_client_perform(client_);
        active_ = nullptr;
        if (err != ESP_OK || out.body.overflow) {
            out.body.Reset();
            return false;
        }
        out.status = esp_http_client_get_status_code(client_);
        if (absorb_cookies && !out.set_cookies.empty()) {
            AbsorbSetCookies(out.set_cookies);
        }
        return out.status >= 200 && out.status < 300;
    }

    esp_http_client_handle_t client_ = nullptr;
    HttpResp* active_ = nullptr;
};

std::string JsonStr(cJSON* obj, const char* key) {
    cJSON* v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    if (cJSON_IsNumber(v)) return std::to_string(static_cast<long long>(v->valuedouble));
    return {};
}

int JsonInt(cJSON* obj, const char* key, int def = 0) {
    cJSON* v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring) return std::atoi(v->valuestring);
    return def;
}

int JsonErrCode(cJSON* obj) {
    const int a = JsonInt(obj, "errcode", 0);
    return a != 0 ? a : JsonInt(obj, "errCode", 0);
}

bool IsEmptyJsonObj(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) || c == '{' || c == '}'; }) &&
           s.find('{') != std::string::npos && s.find('}') != std::string::npos;
}

bool TryRenewSession(HttpSession& sess) {
    if (!IsLoggedIn()) return false;
    HttpResp resp;
    const std::string body = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    if (!sess.Request("POST", std::string(kHost) + "/web/login/renewal", body, "https://weread.qq.com/web/", resp, true)) {
        return false;
    }
    Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
    if (!root) return false;
    if (JsonInt(root.get(), "succ", 0) == 1 || !resp.set_cookies.empty()) {
        ESP_LOGI(TAG, "session renewed ok");
        return true;
    }
    return false;
}

// 从阅读页 HTML 中提取 psvts 以及（免登录兜底时）内嵌的 chapterInfos 数组
std::string ExtractPsvts(const char* html, size_t len) {
    const std::string_view sv(html, len);
    size_t pos = 0;
    while ((pos = sv.find("\"psvts\"", pos)) != std::string_view::npos) {
        size_t colon = sv.find(':', pos + 7);
        if (colon == std::string_view::npos) break;
        size_t val = colon + 1;
        while (val < len && std::isspace(static_cast<unsigned char>(sv[val]))) ++val;
        if (val < len && sv[val] == '"') {
            size_t end = sv.find('"', val + 1);
            if (end != std::string_view::npos && end > val + 1) {
                return std::string(sv.substr(val + 1, end - val - 1));
            }
        }
        pos += 7;
    }
    return {};
}

bool ParseChaptersFromJsonArray(cJSON* list, std::vector<ChapterItem>& out) {
    if (!cJSON_IsArray(list)) return false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        ChapterItem ch;
        ch.uid = JsonStr(item, "chapterUid");
        ch.title = JsonStr(item, "title");
        if (ch.title.empty()) ch.title = JsonStr(item, "text");
        ch.index = JsonInt(item, "chapterIdx", static_cast<int>(out.size()) + 1);
        ch.level = std::max(1, JsonInt(item, "level", 1));
        ch.word_count = JsonInt(item, "wordCount", 0);
        ch.paid = JsonInt(item, "paid", 0) != 0;
        if (!ch.uid.empty()) out.push_back(std::move(ch));
    }
    return !out.empty();
}

bool ExtractChaptersFromHtml(const char* html, size_t len, std::vector<ChapterItem>& out) {
    out.clear();
    const std::string_view sv(html, len);
    size_t pos = 0;
    while ((pos = sv.find("\"chapterInfos\"", pos)) != std::string_view::npos) {
        size_t bracket = sv.find('[', pos + 14);
        if (bracket == std::string_view::npos || bracket - pos > 32) {
            pos += 14;
            continue;
        }
        int depth = 0;
        bool in_str = false;
        bool esc = false;
        size_t end = bracket;
        for (size_t i = bracket; i < len; ++i) {
            const char c = sv[i];
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
            } else {
                if (c == '"') in_str = true;
                else if (c == '[') ++depth;
                else if (c == ']') {
                    if (--depth == 0) {
                        end = i + 1;
                        break;
                    }
                }
            }
        }
        if (end > bracket) {
            Json arr(cJSON_ParseWithLength(html + bracket, end - bracket));
            std::vector<ChapterItem> candidate;
            if (arr && ParseChaptersFromJsonArray(arr.get(), candidate) && !candidate.empty()) {
                // 优先选择带 title 字段的完整 chapterInfos（而非底部 AI 大纲简表）
                out = std::move(candidate);
                break;
            }
        }
        pos = bracket + 1;
    }
    return !out.empty();
}

// ── 轻量 ZIP / EPUB 打包器（直接流式写 SD 文件，支持 Store / Deflate）────
class ZipWriter {
public:
    ZipWriter() = default;
    ~ZipWriter() { Close(); }

    bool Open(const char* path) {
        Close();
        fp_ = std::fopen(path, "wb");
        return fp_ != nullptr;
    }

    void Close() {
        if (fp_) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
        entries_.clear();
    }

    bool AddStored(const std::string& name, const void* data, size_t len) {
        if (!fp_) return false;
        const uint32_t offset = static_cast<uint32_t>(std::ftell(fp_));
        const uint32_t crc = static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data), static_cast<uInt>(len)));
        if (!WriteLocalHeader(name, 0, crc, static_cast<uint32_t>(len), static_cast<uint32_t>(len))) return false;
        if (len > 0 && std::fwrite(data, 1, len, fp_) != len) return false;
        entries_.push_back({name, 0, crc, static_cast<uint32_t>(len), static_cast<uint32_t>(len), offset});
        return true;
    }

    bool AddDeflated(const std::string& name, const void* data, size_t len) {
        if (!fp_) return false;
        if (len < 128) return AddStored(name, data, len);

        const uint32_t crc = static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data), static_cast<uInt>(len)));
        z_stream zs{};
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return AddStored(name, data, len);
        }
        const size_t max_comp = deflateBound(&zs, static_cast<uLong>(len));
        uint8_t* comp_buf = static_cast<uint8_t*>(heap_caps_malloc(max_comp, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!comp_buf) comp_buf = static_cast<uint8_t*>(heap_caps_malloc(max_comp, MALLOC_CAP_8BIT));
        if (!comp_buf) {
            deflateEnd(&zs);
            return AddStored(name, data, len);
        }

        zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
        zs.avail_in = static_cast<uInt>(len);
        zs.next_out = comp_buf;
        zs.avail_out = static_cast<uInt>(max_comp);
        const int ret = deflate(&zs, Z_FINISH);
        const size_t comp_len = zs.total_out;
        deflateEnd(&zs);

        if (ret != Z_STREAM_END || comp_len >= len) {
            heap_caps_free(comp_buf);
            return AddStored(name, data, len);
        }

        const uint32_t offset = static_cast<uint32_t>(std::ftell(fp_));
        const bool ok = WriteLocalHeader(name, 8, crc, static_cast<uint32_t>(comp_len), static_cast<uint32_t>(len)) &&
                        std::fwrite(comp_buf, 1, comp_len, fp_) == comp_len;
        heap_caps_free(comp_buf);
        if (!ok) return false;
        entries_.push_back({name, 8, crc, static_cast<uint32_t>(comp_len), static_cast<uint32_t>(len), offset});
        return true;
    }

    bool Finish() {
        if (!fp_) return false;
        const uint32_t cd_offset = static_cast<uint32_t>(std::ftell(fp_));
        for (const auto& e : entries_) {
            if (!WriteU32(0x02014b50) || // Central directory file header signature
                !WriteU16(20) ||         // Version made by
                !WriteU16(20) ||         // Version needed to extract
                !WriteU16(0x0800) ||     // General purpose bit flag (UTF-8)
                !WriteU16(e.method) ||
                !WriteU16(0) ||          // Last mod file time
                !WriteU16(0) ||          // Last mod file date
                !WriteU32(e.crc) ||
                !WriteU32(e.comp_size) ||
                !WriteU32(e.uncomp_size) ||
                !WriteU16(static_cast<uint16_t>(e.name.size())) ||
                !WriteU16(0) ||          // Extra field length
                !WriteU16(0) ||          // File comment length
                !WriteU16(0) ||          // Disk number start
                !WriteU16(0) ||          // Internal file attributes
                !WriteU32(0) ||          // External file attributes
                !WriteU32(e.offset) ||
                std::fwrite(e.name.data(), 1, e.name.size(), fp_) != e.name.size()) {
                return false;
            }
        }
        const uint32_t cd_size = static_cast<uint32_t>(std::ftell(fp_)) - cd_offset;
        const uint16_t count = static_cast<uint16_t>(entries_.size());
        if (!WriteU32(0x06054b50) || // End of central directory signature
            !WriteU16(0) ||          // Number of this disk
            !WriteU16(0) ||          // Disk where central directory starts
            !WriteU16(count) ||
            !WriteU16(count) ||
            !WriteU32(cd_size) ||
            !WriteU32(cd_offset) ||
            !WriteU16(0)) {          // Comment length
            return false;
        }
        std::fflush(fp_);
        fsync(fileno(fp_));
        Close();
        return true;
    }

private:
    struct EntryRec {
        std::string name;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint32_t comp_size = 0;
        uint32_t uncomp_size = 0;
        uint32_t offset = 0;
    };

    bool WriteU16(uint16_t v) {
        const uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
        return std::fwrite(b, 1, 2, fp_) == 2;
    }
    bool WriteU32(uint32_t v) {
        const uint8_t b[4] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 24) & 0xFF),
        };
        return std::fwrite(b, 1, 4, fp_) == 4;
    }
    bool WriteLocalHeader(const std::string& name, uint16_t method, uint32_t crc, uint32_t comp, uint32_t uncomp) {
        return WriteU32(0x04034b50) &&
               WriteU16(20) &&
               WriteU16(0x0800) && // UTF-8 filename
               WriteU16(method) &&
               WriteU16(0) &&
               WriteU16(0) &&
               WriteU32(crc) &&
               WriteU32(comp) &&
               WriteU32(uncomp) &&
               WriteU16(static_cast<uint16_t>(name.size())) &&
               WriteU16(0) &&
               std::fwrite(name.data(), 1, name.size(), fp_) == name.size();
    }

    FILE* fp_ = nullptr;
    std::vector<EntryRec> entries_;
};

std::string WrapTxtAsXhtml(const std::string& title, const std::string& txt) {
    std::string out;
    out.reserve(txt.size() + 512);
    out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" + EscapeXml(title) + "</title></head><body>\n";
    if (!title.empty()) {
        out += "<h2>" + EscapeXml(title) + "</h2>\n";
    }
    size_t start = 0;
    while (start < txt.size()) {
        size_t end = txt.find('\n', start);
        if (end == std::string::npos) end = txt.size();
        std::string line = txt.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
        if (first < line.size()) {
            out += "<p>" + EscapeXml(line.substr(first)) + "</p>\n";
        }
        start = end + 1;
    }
    out += "</body></html>\n";
    return out;
}

valet::opds::Entry BookToOpdsEntry(const BookItem& b, bool from_shelf) {
    valet::opds::Entry e;
    e.id = "weread:" + b.book_id;
    e.title = valet::opds::CleanText(b.title, valet::opds::kMaxTitle);
    e.author = valet::opds::CleanText(b.author, valet::opds::kMaxAuthor);
    e.summary = valet::opds::CleanText(b.summary, valet::opds::kMaxSummary);
    e.cover = b.cover;
    e.thumbnail = b.cover;
    e.library = "微信读书";
    if (b.progress_x10 >= 0) {
        e.progress = static_cast<float>(b.progress_x10) / 1000.0f;
    }
    if (from_shelf && b.progress_x10 > 0) {
        char pbuf[32];
        std::snprintf(pbuf, sizeof(pbuf), "已读 %d%%", (b.progress_x10 + 5) / 10);
        if (!e.author.empty()) e.author += " · ";
        e.author += pbuf;
    }
    valet::opds::Acquisition acq;
    acq.url = "weread://" + b.book_id;
    acq.type = "application/epub+zip";
    acq.ext = "epub";
    acq.length = 0;
    e.files.push_back(std::move(acq));
    return e;
}

std::string WrMapPath(const std::string& book_path) {
    return reader::BookStoragePath(book_path.c_str(), ".wrmap");
}

}  // namespace

// ── 公开 API 实现 ────────────────────────────────────────────────────────

bool IsLoggedIn() {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    EnsureAuthLoadedLocked();
    return !g_auth.vid.empty() && (!g_auth.skey.empty() || !g_auth.rt.empty());
}

std::string LoggedVid() {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    EnsureAuthLoadedLocked();
    return g_auth.vid;
}

void Logout() {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    g_auth.vid.clear();
    g_auth.skey.clear();
    g_auth.rt.clear();
    g_auth.gid.clear();
    g_auth.loaded = true;
    SaveAuthLocked();
}

bool BeginQrLogin(std::string& uid, std::string& qr_url, std::string& err) {
    uid.clear();
    qr_url.clear();
    err.clear();
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady(15000)) {
        err = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }
    ActivationHold hold;
    HttpSession sess;
    HttpResp resp;
    if (!sess.Request("GET", std::string(kHost) + "/api/auth/getLoginUid", {}, "https://weread.qq.com/", resp, false, 6000)) {
        err = "无法连接微信读书获取二维码";
        return false;
    }
    Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
    if (!root) {
        err = "登录二维码响应异常";
        return false;
    }
    uid = JsonStr(root.get(), "uid");
    if (uid.empty()) uid = JsonStr(cJSON_GetObjectItemCaseSensitive(root.get(), "data"), "uid");
    if (uid.empty()) {
        err = "未获取到登录会话 ID";
        return false;
    }
    qr_url = std::string(kHost) + "/web/confirm?uid=" + uid;
    return true;
}

bool PollQrLogin(const std::string& uid, const std::string& otp, QrState& state, std::string& err) {
    state = QrState::kPending;
    err.clear();
    if (uid.empty()) {
        state = QrState::kError;
        err = "无效的登录会话";
        return false;
    }
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady(5000)) {
        return true; // 暂未就绪，下次继续轮询
    }
    ActivationHold hold;
    HttpSession sess;
    HttpResp resp;
    std::string url = std::string(kHost) + "/api/auth/getLoginInfo?uid=" + UrlEncode(uid) + "&otp";
    if (!otp.empty()) url += "=" + UrlEncode(otp);
    if (!sess.Request("GET", url, {}, "https://weread.qq.com/", resp, true, 6000)) {
        return true;
    }
    Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
    if (!root) return true;
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
    if (!cJSON_IsObject(data)) data = root.get();
    cJSON* succeed = cJSON_GetObjectItemCaseSensitive(data, "succeed");
    if (cJSON_IsTrue(succeed)) {
        {
            std::lock_guard<std::mutex> lock(g_auth_mu);
            EnsureAuthLoadedLocked();
            if (g_auth.vid.empty()) g_auth.vid = JsonStr(data, "webLoginVid");
            if (g_auth.vid.empty()) g_auth.vid = JsonStr(data, "vid");
            if (g_auth.skey.empty()) g_auth.skey = JsonStr(data, "accessToken");
            if (g_auth.rt.empty()) g_auth.rt = JsonStr(data, "refreshToken");
            SaveAuthLocked();
        }
        if (IsLoggedIn()) {
            state = QrState::kComplete;
            ESP_LOGI(TAG, "WeRead login succeeded vid=%s", LoggedVid().c_str());
            return true;
        }
        state = QrState::kError;
        err = "扫码确认成功，但未收到登录凭据";
        return false;
    }
    const std::string logic = JsonStr(data, "logicCode");
    if (logic == "NEED_OTP") state = QrState::kNeedOtp;
    else if (logic == "OTP_NOT_MATCH") state = QrState::kOtpMismatch;
    else if (logic == "LOGIN_TIMEOUT" || logic == "OTP_EXPIRED") state = QrState::kExpired;
    else if (!logic.empty()) {
        state = QrState::kError;
        err = "扫码登录失败：" + logic;
        return false;
    }
    return true;
}

bool SearchBooks(const std::string& query, std::vector<valet::opds::Entry>& out, std::string& err) {
    out.clear();
    err.clear();
    if (query.empty()) return false;
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }
    ActivationHold hold;
    HttpSession sess;
    HttpResp resp;
    const std::string url = std::string(kHost) + "/web/search/global?keyword=" + UrlEncode(query) + "&maxIdx=0&count=25";
    if (!sess.Request("GET", url, {}, "https://weread.qq.com/", resp, true)) {
        err = "微信读书搜索连接失败";
        return false;
    }
    Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
    if (!root) {
        err = "微信读书搜索解析失败";
        return false;
    }
    cJSON* books = cJSON_GetObjectItemCaseSensitive(root.get(), "books");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, books) {
        cJSON* info = cJSON_GetObjectItemCaseSensitive(item, "bookInfo");
        if (!cJSON_IsObject(info)) info = item;
        BookItem b;
        b.book_id = JsonStr(info, "bookId");
        b.title = JsonStr(info, "title");
        b.author = JsonStr(info, "author");
        b.cover = JsonStr(info, "cover");
        b.summary = JsonStr(info, "intro");
        b.format = JsonStr(info, "format");
        if (b.book_id.empty() || b.title.empty()) continue;
        // 排除有声书或非电子书条目
        if (b.book_id.rfind("CB_", 0) == 0 || b.book_id.rfind(" Audio", 0) == 0) continue;
        out.push_back(BookToOpdsEntry(b, false));
    }
    return true;
}

bool FetchShelf(std::vector<valet::opds::Entry>& out, std::string& err) {
    out.clear();
    err.clear();
    if (!IsLoggedIn()) {
        err = "尚未登录微信读书";
        return false;
    }
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }
    ActivationHold hold;
    HttpSession sess;
    HttpResp resp;
    const std::string url = std::string(kHost) + "/web/shelf/sync";
    if (!sess.Request("GET", url, {}, "https://weread.qq.com/web/shelf", resp, true)) {
        err = "微信读书书架加载失败";
        return false;
    }
    Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
    if (root && (JsonErrCode(root.get()) == -2012 || JsonErrCode(root.get()) == -2010)) {
        if (TryRenewSession(sess) && sess.Request("GET", url, {}, "https://weread.qq.com/web/shelf", resp, true)) {
            root.reset(cJSON_ParseWithLength(resp.body.data, resp.body.len));
        }
    }
    if (!root) {
        err = "书架数据解析失败";
        return false;
    }
    const int err_code = JsonErrCode(root.get());
    if (err_code == -2012 || err_code == -2010) {
        err = "微信读书登录已过期，请重新扫码";
        return false;
    }

    std::vector<BookItem> books;
    cJSON* list = cJSON_GetObjectItemCaseSensitive(root.get(), "books");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        BookItem b;
        b.book_id = JsonStr(item, "bookId");
        b.title = JsonStr(item, "title");
        b.author = JsonStr(item, "author");
        b.cover = JsonStr(item, "cover");
        b.summary = JsonStr(item, "intro");
        b.format = JsonStr(item, "format");
        if (!b.book_id.empty() && !b.title.empty()) {
            books.push_back(std::move(b));
        }
    }
    cJSON* progress = cJSON_GetObjectItemCaseSensitive(root.get(), "bookProgress");
    cJSON_ArrayForEach(item, progress) {
        const std::string id = JsonStr(item, "bookId");
        auto it = std::find_if(books.begin(), books.end(), [&](const BookItem& b) { return b.book_id == id; });
        if (it != books.end()) {
            const int pct = JsonInt(item, "progress", 0);
            it->progress_x10 = std::clamp(pct * 10, 0, 1000);
            it->chapter_uid = JsonStr(item, "chapterUid");
            it->chapter_offset = std::max(0, JsonInt(item, "chapterOffset", 0));
            it->update_time = static_cast<long long>(JsonInt(item, "updateTime", 0));
        }
    }
    std::stable_sort(books.begin(), books.end(), [](const BookItem& a, const BookItem& b) {
        return a.update_time > b.update_time;
    });
    out.reserve(books.size());
    for (const auto& b : books) {
        out.push_back(BookToOpdsEntry(b, true));
    }
    return true;
}

bool DownloadBookAsEpub(const std::string& book_id,
                        const std::string& fallback_title,
                        const std::string& fallback_author,
                        const std::string& cover_url,
                        const std::string& save_path,
                        std::string& err_out,
                        void (*on_progress)(int pct, void* user),
                        void* progress_user,
                        DownloadGate* gate,
                        int* downloaded_chapters_out,
                        int* total_chapters_out) {
    err_out.clear();
    if (downloaded_chapters_out) *downloaded_chapters_out = 0;
    if (total_chapters_out) *total_chapters_out = 0;

    if (book_id.empty() || save_path.empty()) {
        err_out = Lang::Strings::BOOK_BAD_ARG;
        return false;
    }
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (gate && gate->IsCancelled()) {
        err_out = Lang::Strings::BOOK_CANCELLED;
        return false;
    }
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted() || !SdEnsureAppLayout()) {
        err_out = Lang::Strings::BOOK_NO_SD_SHORT;
        return false;
    }
    ActivationHold hold;

    auto report = [&](int pct) {
        if (on_progress) on_progress(std::clamp(pct, 0, 100), progress_user);
    };
    report(2);

    HttpSession sess;
    HttpResp resp;

    // 1. 先加载阅读页 /web/reader/<EncodeId(bookId)>：
    //    既能提取动态令牌 psvts，也能在未登录（游客态）下直接从 __INITIAL_STATE__ 拿到完整 chapterInfos！
    const std::string reader_url = std::string(kHost) + "/web/reader/" + EncodeId(book_id);
    if (!sess.Request("GET", reader_url, {}, "https://weread.qq.com/", resp, true) || resp.body.len == 0) {
        err_out = "微信读书阅读页加载失败";
        return false;
    }
    std::string psvts = ExtractPsvts(resp.body.data, resp.body.len);
    std::vector<ChapterItem> chapters;
    ExtractChaptersFromHtml(resp.body.data, resp.body.len, chapters);
    resp.body.Reset();

    // 2. 若已登录或 HTML 未提取到目录，再尝试 POST /web/book/chapterInfos
    if (chapters.empty() || IsLoggedIn()) {
        const std::string ch_body = "{\"bookIds\":[\"" + EscapeJson(book_id) + "\"]}";
        if (sess.Request("POST", std::string(kHost) + "/web/book/chapterInfos", ch_body, reader_url, resp, true)) {
            Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
            if (root && (JsonErrCode(root.get()) == -2012 || JsonErrCode(root.get()) == -2010) && IsLoggedIn()) {
                if (TryRenewSession(sess) &&
                    sess.Request("POST", std::string(kHost) + "/web/book/chapterInfos", ch_body, reader_url, resp, true)) {
                    root.reset(cJSON_ParseWithLength(resp.body.data, resp.body.len));
                }
            }
            if (root) {
                cJSON* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
                cJSON* entry = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : nullptr;
                if (entry) {
                    cJSON* list = cJSON_GetObjectItemCaseSensitive(entry, "updated");
                    if (!cJSON_IsArray(list)) list = cJSON_GetObjectItemCaseSensitive(entry, "chapterInfos");
                    std::vector<ChapterItem> api_chapters;
                    if (ParseChaptersFromJsonArray(list, api_chapters) && !api_chapters.empty()) {
                        chapters = std::move(api_chapters);
                    }
                }
            }
        }
        resp.body.Reset();
    }

    if (psvts.empty()) {
        err_out = "未能获取微信读书阅读令牌";
        return false;
    }
    if (chapters.empty()) {
        err_out = "这本书没有可读取的章节目录";
        return false;
    }
    if (total_chapters_out) *total_chapters_out = static_cast<int>(chapters.size());
    report(8);

    // 3. 准备写临时 .epub.tmp
    const std::string tmp_path = save_path + ".tmp";
    ZipWriter zip;
    if (!zip.Open(tmp_path.c_str())) {
        err_out = Lang::Strings::BOOK_STORAGE_UNAVAIL;
        return false;
    }

    static constexpr const char* kMimeType = "application/epub+zip";
    static constexpr const char* kContainerXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "  <rootfiles>\n"
        "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
        "  </rootfiles>\n"
        "</container>\n";

    if (!zip.AddStored("mimetype", kMimeType, std::strlen(kMimeType)) ||
        !zip.AddDeflated("META-INF/container.xml", kContainerXml, std::strlen(kContainerXml))) {
        zip.Close();
        unlink(tmp_path.c_str());
        err_out = Lang::Strings::BOOK_STORAGE_UNAVAIL;
        return false;
    }

    // 4. 逐章拉取分片、逆置换解密并写入 OEBPS/Text/ch_N.xhtml
    struct PackedChapter {
        std::string uid;
        std::string title;
        std::string href;
        int index = 0;
    };
    std::vector<PackedChapter> packed;
    packed.reserve(chapters.size());

    const int total_ch = static_cast<int>(chapters.size());
    int consecutive_empty = 0;

    for (int i = 0; i < total_ch; ++i) {
        if (gate && gate->IsCancelled()) {
            zip.Close();
            unlink(tmp_path.c_str());
            err_out = Lang::Strings::BOOK_CANCELLED;
            return false;
        }
        const auto& ch = chapters[static_cast<size_t>(i)];
        const std::string ch_referer = std::string(kHost) + "/web/reader/" + EncodeId(book_id) + "k" + EncodeId(ch.uid);

        auto fetch_shard = [&](const char* ep, HttpResp& out_r) {
            const std::string payload = ContentPayloadJson(book_id, ch.uid, psvts);
            return sess.Request("POST", std::string(kHost) + ep, payload, ch_referer, out_r, true);
        };

        HttpResp r0;
        if (!fetch_shard("/web/book/chapter/e_0", r0) || r0.body.len == 0) {
            // 若是未购/付费受限章节，e_0 返回空或 {}
            if (++consecutive_empty >= 3 && !packed.empty()) {
                ESP_LOGI(TAG, "reached paid/locked chapter boundary at idx=%d (%s), stopping", i, ch.title.c_str());
                break;
            }
            continue;
        }
        const std::string s0 = r0.body.ToString();
        r0.body.Reset();

        if (IsEmptyJsonObj(s0)) {
            // 付费锁章：如果前面已经下到了免费章节，说明到达试读边界
            if (++consecutive_empty >= 2 && !packed.empty()) {
                ESP_LOGI(TAG, "reached trial boundary at chapter %d/%d (%s)", i + 1, total_ch, ch.title.c_str());
                break;
            }
            continue;
        }

        const bool is_txt = (!s0.empty() && s0.front() == '{' && s0.find("\"bookId\"") != std::string::npos);
        std::vector<std::string> shards;
        if (is_txt) {
            HttpResp t0, t1;
            if (!fetch_shard("/web/book/chapter/t_0", t0) || !fetch_shard("/web/book/chapter/t_1", t1)) {
                if (++consecutive_empty >= 2 && !packed.empty()) break;
                continue;
            }
            std::string st0 = t0.body.ToString();
            std::string st1 = t1.body.ToString();
            t0.body.Reset();
            t1.body.Reset();
            if (IsEmptyJsonObj(st0)) {
                if (++consecutive_empty >= 2 && !packed.empty()) break;
                continue;
            }
            shards = {std::move(st0), std::move(st1)};
        } else {
            HttpResp r1, r3;
            if (!fetch_shard("/web/book/chapter/e_1", r1) || !fetch_shard("/web/book/chapter/e_3", r3)) {
                if (++consecutive_empty >= 2 && !packed.empty()) break;
                continue;
            }
            shards = {s0, r1.body.ToString(), r3.body.ToString()};
            r1.body.Reset();
            r3.body.Reset();
        }

        std::string decoded;
        if (!DecodeShards(shards, decoded) || decoded.empty()) {
            if (++consecutive_empty >= 2 && !packed.empty()) break;
            continue;
        }
        consecutive_empty = 0;

        if (is_txt) {
            decoded = WrapTxtAsXhtml(ch.title, decoded);
        }

        char href_buf[32];
        std::snprintf(href_buf, sizeof(href_buf), "Text/ch_%d.xhtml", static_cast<int>(packed.size()));
        const std::string zip_entry = std::string("OEBPS/") + href_buf;
        if (!zip.AddDeflated(zip_entry, decoded.data(), decoded.size())) {
            zip.Close();
            unlink(tmp_path.c_str());
            err_out = Lang::Strings::BOOK_STORAGE_UNAVAIL;
            return false;
        }

        packed.push_back({ch.uid, ch.title.empty() ? ("第 " + std::to_string(packed.size() + 1) + " 章") : ch.title, href_buf, ch.index});
        report(10 + (i + 1) * 80 / std::max(1, total_ch));
        vTaskDelay(pdMS_TO_TICKS(40)); // 温和节流，避免触发微信读书风控与看门狗
    }

    // 如果这本书存在未解锁的后续付费章节，在末尾附一章温和小提示，让用户知道为何到此为止及如何解锁
    if (!packed.empty() && static_cast<int>(packed.size()) < total_ch) {
        const int locked_n = total_ch - static_cast<int>(packed.size());
        char tip_title[64];
        std::snprintf(tip_title, sizeof(tip_title), "试读结束（余 %d 章需解锁）", locked_n);
        std::string tip_body =
            "本书前 " + std::to_string(packed.size()) + " 章已下载完成。\n" +
            (IsLoggedIn()
                 ? "后续章节为微信读书付费或会员章节。在手机微信读书解锁或购买后，回到「书库」重新下载即可同步完整全本。"
                 : "当前尚未登录微信读书。在「书架 → 书库 → 微信读书」中扫码登录后，即可同步已购与会员可读章节。");
        std::string tip_xhtml = WrapTxtAsXhtml(tip_title, tip_body);
        char href_buf[32];
        std::snprintf(href_buf, sizeof(href_buf), "Text/ch_%d.xhtml", static_cast<int>(packed.size()));
        if (zip.AddDeflated(std::string("OEBPS/") + href_buf, tip_xhtml.data(), tip_xhtml.size())) {
            packed.push_back({"0", tip_title, href_buf, total_ch + 1});
        }
    }

    if (packed.empty()) {
        zip.Close();
        unlink(tmp_path.c_str());
        err_out = IsLoggedIn() ? "该书为付费书籍，请先在微信读书解锁后再下载"
                               : "该书需登录微信读书后方可阅读，请先在书库中扫码登录";
        return false;
    }

    // 5. 下载封面图片并内嵌到 EPUB (OEBPS/cover.jpg)，同时解码生成旁路封面 (.a2l8)
    bool has_cover = false;
    reader::RasterImage cover_img;
    if (!cover_url.empty()) {
        std::string big_cover = cover_url;
        const size_t s_pos = big_cover.rfind("/s_");
        if (s_pos != std::string::npos) {
            big_cover.replace(s_pos, 3, "/t6_");
        }
        HttpResp cresp;
        if ((sess.Request("GET", big_cover, {}, "https://weread.qq.com/", cresp, false) && cresp.body.len > 128) ||
            (big_cover != cover_url &&
             sess.Request("GET", cover_url, {}, "https://weread.qq.com/", cresp, false) && cresp.body.len > 128)) {
            const auto* cdata = reinterpret_cast<const uint8_t*>(cresp.body.data);
            if (reader::DecodeImageToL8(cdata, cresp.body.len,
                                        reader::kCoverSidecarTargetW, reader::kCoverSidecarTargetH, cover_img) &&
                !cover_img.empty()) {
                has_cover = zip.AddStored("OEBPS/cover.jpg", cdata, cresp.body.len);
            }
        }
    }

    // 6. 生成 OEBPS/toc.ncx 与 OEBPS/content.opf
    std::string ncx =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
        "  <head><meta name=\"dtb:uid\" content=\"weread-" + EscapeXml(book_id) + "\"/></head>\n"
        "  <docTitle><text>" + EscapeXml(fallback_title) + "</text></docTitle>\n"
        "  <navMap>\n";
    for (size_t i = 0; i < packed.size(); ++i) {
        const auto& p = packed[i];
        ncx += "    <navPoint id=\"navPoint-" + std::to_string(i + 1) + "\" playOrder=\"" + std::to_string(i + 1) + "\">\n"
               "      <navLabel><text>" + EscapeXml(p.title) + "</text></navLabel>\n"
               "      <content src=\"" + p.href + "\"/>\n"
               "    </navPoint>\n";
    }
    ncx += "  </navMap>\n</ncx>\n";

    std::string opf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"BookId\" version=\"2.0\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
        "    <dc:title>" + EscapeXml(fallback_title) + "</dc:title>\n"
        "    <dc:creator>" + EscapeXml(fallback_author) + "</dc:creator>\n"
        "    <dc:language>zh-CN</dc:language>\n"
        "    <dc:identifier id=\"BookId\">weread-" + EscapeXml(book_id) + "</dc:identifier>\n";
    if (has_cover) {
        opf += "    <meta name=\"cover\" content=\"cover-img\"/>\n";
    }
    opf +=
        "  </metadata>\n"
        "  <manifest>\n"
        "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
    if (has_cover) {
        opf += "    <item id=\"cover-img\" href=\"cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>\n";
    }
    for (size_t i = 0; i < packed.size(); ++i) {
        opf += "    <item id=\"ch_" + std::to_string(i) + "\" href=\"" + packed[i].href +
               "\" media-type=\"application/xhtml+xml\"/>\n";
    }
    opf += "  </manifest>\n  <spine toc=\"ncx\">\n";
    for (size_t i = 0; i < packed.size(); ++i) {
        opf += "    <itemref idref=\"ch_" + std::to_string(i) + "\"/>\n";
    }
    opf += "  </spine>\n</package>\n";

    if (!zip.AddDeflated("OEBPS/toc.ncx", ncx.data(), ncx.size()) ||
        !zip.AddDeflated("OEBPS/content.opf", opf.data(), opf.size()) ||
        !zip.Finish()) {
        zip.Close();
        unlink(tmp_path.c_str());
        err_out = Lang::Strings::BOOK_STORAGE_UNAVAIL;
        return false;
    }

    if (!reader::CommitDownloadedFile(tmp_path, save_path, true, err_out)) {
        unlink(tmp_path.c_str());
        return false;
    }
    report(94);

    // 7. 保存 spine <-> chapterUid 映射表 (.wrmap)、.sync 进度同步标识及旁路封面 (.a2l8)
    {
        std::string map_text;
        for (const auto& p : packed) {
            if (p.uid != "0") {
                map_text += p.uid + "\n";
            }
        }
        const std::string map_path = WrMapPath(save_path);
        if (!map_path.empty()) {
            reader::WriteFileAtomic(map_path.c_str(), reinterpret_cast<const uint8_t*>(map_text.data()), map_text.size());
        }
        valet::sync::Remember(save_path, "weread://" + book_id);
    }

    reader::DeleteBookCoverSidecar(save_path.c_str());
    if (!cover_img.empty()) {
        reader::SaveBookCoverSidecarFromL8(save_path.c_str(), cover_img);
    }

    if (downloaded_chapters_out) *downloaded_chapters_out = static_cast<int>(packed.size());
    reader::InvalidateBookLibraryCache();
    report(100);
    ESP_LOGI(TAG, "WeRead EPUB packaged -> %s (chapters=%u/%d)", save_path.c_str(),
             static_cast<unsigned>(packed.size()), total_ch);
    return true;
}

bool FetchProgress(const std::string& book_id, RemoteProgress& out, std::string& err) {
    out = RemoteProgress{};
    err.clear();
    if (book_id.empty() || !IsLoggedIn()) {
        err = "未登录微信读书";
        return false;
    }
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady(8000)) {
        err = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }
    ActivationHold hold;
    HttpSession sess;
    HttpResp resp;
    const std::string url = std::string(kHost) + "/web/book/getProgress?bookId=" + UrlEncode(book_id);
    const std::string referer = std::string(kHost) + "/web/reader/" + EncodeId(book_id);
    if (!sess.Request("GET", url, {}, referer, resp, true)) {
        err = "获取微信读书进度失败";
        return false;
    }
    Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
    if (root && (JsonErrCode(root.get()) == -2012 || JsonErrCode(root.get()) == -2010)) {
        if (TryRenewSession(sess) && sess.Request("GET", url, {}, referer, resp, true)) {
            root.reset(cJSON_ParseWithLength(resp.body.data, resp.body.len));
        }
    }
    if (!root) {
        err = "微信读书进度响应解析失败";
        return false;
    }
    cJSON* book = cJSON_GetObjectItemCaseSensitive(root.get(), "book");
    if (!cJSON_IsObject(book)) book = root.get();
    const int pct = JsonInt(book, "progress", -1);
    if (pct >= 0) out.progress_x10 = std::clamp(pct * 10, 0, 1000);
    out.chapter_uid = JsonStr(book, "chapterUid");
    out.chapter_offset = std::max(0, JsonInt(book, "chapterOffset", 0));
    out.update_time = static_cast<long long>(JsonInt(book, "updateTime", 0));
    out.reading_time_sec = JsonInt(book, "readingTime", 0);
    return out.progress_x10 >= 0;
}

bool UploadProgress(const std::string& book_id,
                    const std::string& chapter_uid,
                    int chapter_idx,
                    int chapter_offset,
                    int progress_x10,
                    int read_seconds,
                    std::string& err) {
    err.clear();
    if (book_id.empty() || !IsLoggedIn()) {
        err = "未登录微信读书";
        return false;
    }
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady(8000)) {
        err = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }
    ActivationHold hold;
    HttpSession sess;
    HttpResp resp;

    const std::string eff_uid = chapter_uid.empty() ? "1" : chapter_uid;
    const std::string reader_url = std::string(kHost) + "/web/reader/" + EncodeId(book_id) + "k" + EncodeId(eff_uid);
    if (!sess.Request("GET", reader_url, {}, "https://weread.qq.com/", resp, true)) {
        err = "加载阅读令牌失败";
        return false;
    }
    const std::string psvts = ExtractPsvts(resp.body.data, resp.body.len);
    resp.body.Reset();
    if (psvts.empty()) {
        err = "阅读令牌缺失";
        return false;
    }

    long now = static_cast<long>(std::time(nullptr));
    if (now < 1700000000L) now = 1764000000L;
    const uint64_t ts = static_cast<uint64_t>(now) * 1000ULL + (esp_random() % 1000);
    const uint32_t rn = esp_random() % 1000;
    const int pr = std::clamp((progress_x10 + 5) / 10, 0, 100);
    const int rt = std::clamp(read_seconds, 1, 3600);
    const std::string sg = Sha256Hex(std::to_string(ts) + std::to_string(rn) + kReaderToken);

    std::vector<std::pair<std::string, std::string>> params = {
        {"appId", WebAppId()},
        {"b", EncodeId(book_id)},
        {"c", EncodeId(eff_uid)},
        {"ci", std::to_string(std::max(1, chapter_idx))},
        {"co", std::to_string(std::max(0, chapter_offset))},
        {"sm", ""},
        {"pr", std::to_string(pr)},
        {"rt", std::to_string(rt)},
        {"ts", std::to_string(ts)},
        {"rn", std::to_string(rn)},
        {"sg", sg},
        {"ct", std::to_string(now)},
        {"ps", psvts},
        {"pc", EncodeId(std::to_string(now))},
    };
    params.emplace_back("s", Sign(SortedQuery(params)));

    static constexpr const char* kNumKeys[] = {"ci", "co", "pr", "rt", "ts", "rn", "ct"};
    std::string body = "{";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) body.push_back(',');
        const bool is_num = std::any_of(std::begin(kNumKeys), std::end(kNumKeys),
                                        [&](const char* k) { return params[i].first == k; });
        body += "\"" + params[i].first + "\":";
        body += is_num ? params[i].second : ("\"" + EscapeJson(params[i].second) + "\"");
    }
    body.push_back('}');

    if (!sess.Request("POST", std::string(kHost) + "/web/book/read", body, reader_url, resp, true)) {
        err = "上传微信读书进度失败";
        return false;
    }
    Json root(cJSON_ParseWithLength(resp.body.data, resp.body.len));
    if (!root || JsonInt(root.get(), "succ", 0) != 1) {
        err = "微信读书进度未接受";
        return false;
    }
    return true;
}

bool IsWeReadBook(const std::string& book_path, std::string* book_id_out) {
    if (book_id_out) book_id_out->clear();
    if (book_path.empty()) return false;
    const std::string sync_path = reader::BookStoragePath(book_path.c_str(), ".sync");
    if (sync_path.empty()) return false;
    FILE* f = std::fopen(sync_path.c_str(), "rb");
    if (!f) return false;
    char buf[160] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    char* nl = std::strchr(buf, '\n');
    if (nl) *nl = '\0';
    constexpr const char* kPrefix = "weread://";
    const size_t plen = std::strlen(kPrefix);
    if (std::strncmp(buf, kPrefix, plen) != 0) return false;
    if (book_id_out) *book_id_out = buf + plen;
    return buf[plen] != '\0';
}

reader::BookSource DetectBookSource(const std::string& book_path) {
    if (book_path.empty()) return reader::BookSource::kLocal;
    const std::string sync_path = reader::BookStoragePath(book_path.c_str(), ".sync");
    if (sync_path.empty()) return reader::BookSource::kLocal;
    FILE* f = std::fopen(sync_path.c_str(), "rb");
    if (!f) return reader::BookSource::kLocal;
    char buf[160] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    char* nl = std::strchr(buf, '\n');
    if (nl) *nl = '\0';
    if (std::strncmp(buf, "weread://", 9) == 0) return reader::BookSource::kWeRead;
    if (std::strstr(buf, "/api/u/") != nullptr) return reader::BookSource::kSyncedLib;
    if (std::strncmp(buf, "opds://", 7) == 0 || std::strncmp(buf, "cloud://", 8) == 0 ||
        std::strncmp(buf, "http://", 7) == 0 || std::strncmp(buf, "https://", 8) == 0) {
        return reader::BookSource::kLibrary;
    }
    return reader::BookSource::kLocal;
}

const char* BookSourceRibbonText(reader::BookSource src) {
    switch (src) {
        case reader::BookSource::kWeRead:    return "微读";
        case reader::BookSource::kSyncedLib: return "书库";
        case reader::BookSource::kLibrary:   return "书库";
        default:                             return "";
    }
}

const char* BookSourceFullName(reader::BookSource src) {
    switch (src) {
        case reader::BookSource::kWeRead:    return "微信读书";
        case reader::BookSource::kSyncedLib: return "已订阅书库";
        case reader::BookSource::kLibrary:   return "在线书库";
        default:                             return "";
    }
}

static std::vector<std::string> ReadWrMap(const std::string& book_path) {
    std::vector<std::string> uids;
    const std::string path = WrMapPath(book_path);
    if (path.empty()) return uids;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return uids;
    char line[64];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty()) uids.push_back(std::move(s));
    }
    std::fclose(f);
    return uids;
}

std::string ChapterUidForSpine(const std::string& book_path, int spine_index) {
    if (spine_index < 0) return {};
    const auto uids = ReadWrMap(book_path);
    if (static_cast<size_t>(spine_index) >= uids.size()) return {};
    return uids[static_cast<size_t>(spine_index)];
}

int SpineForChapterUid(const std::string& book_path, const std::string& chapter_uid) {
    if (chapter_uid.empty()) return -1;
    const auto uids = ReadWrMap(book_path);
    for (size_t i = 0; i < uids.size(); ++i) {
        if (uids[i] == chapter_uid) return static_cast<int>(i);
    }
    return -1;
}

int DownloadedChapterCount(const std::string& book_path) {
    return static_cast<int>(ReadWrMap(book_path).size());
}

}  // namespace valet::weread
