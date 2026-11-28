#include "opds_fetch.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "http.h"
#include "library_sources.h"
#include "power_policy.h"

#include <memory>

namespace valet::opds {
namespace {
constexpr int kReadChunk = 1024;

struct ActivationHold {
    ActivationHold() { Application::GetInstance().PauseActivationPolling(); }
    ~ActivationHold() { Application::GetInstance().ResumeActivationPolling(); }
};
} // namespace

FetchResult FetchFeed(const std::string& url, int timeout_ms) {
    FetchResult result;
    if (url.empty()) {
        result.error = Lang::Strings::BOOK_BAD_ARG;
        return result;
    }
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady()) {
        result.error = Lang::Strings::BOOK_NET_NOT_READY;
        return result;
    }
    ActivationHold hold;
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        result.error = Lang::Strings::BOOK_NO_NETWORK;
        return result;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        result.error = Lang::Strings::BOOK_CONN_FAIL;
        return result;
    }
    http->SetTimeout(timeout_ms);
    http->SetKeepAlive(false);
    http->SetHeader("Accept", "application/atom+xml,application/xml;q=0.9,*/*;q=0.5");
    http->SetHeader("Connection", "close");
    const std::string auth = valet::LibraryAuthHeaderForUrl(url);
    if (!auth.empty()) http->SetHeader("Authorization", auth);
    if (!http->Open("GET", url)) {
        result.error = Lang::Strings::BOOK_CONN_FAIL;
        return result;
    }
    result.status = http->GetStatusCode();
    if (result.status < 200 || result.status >= 300) {
        http->Close();
        result.error = result.status == 401 || result.status == 403 ? "书库需要认证或密钥已失效，请重新配置"
                     : result.status == 404                    ? "这个地址上没有书库"
                                                              : "书库没有回应（" + std::to_string(result.status) + "）";
        return result;
    }
    result.body.reserve(16 * 1024);
    char buffer[kReadChunk];
    int entries = 0;
    while (true) {
        const int n = http->Read(buffer, sizeof(buffer));
        if (n < 0) {
            http->Close();
            result.body.clear();
            result.error = Lang::Strings::BOOK_DOWNLOAD_FAIL;
            return result;
        }
        if (n == 0) break;
        if (result.body.size() + static_cast<size_t>(n) > kMaxFeedBytes) {
            // 一页太大（搜一个常见的词能回上千本）：只留前面这些，够列出 kMaxEntries 本
            result.truncated = true;
            break;
        }
        const size_t scan_from = result.body.size() > 7 ? result.body.size() - 7 : 0;
        result.body.append(buffer, static_cast<size_t>(n));
        // 一页能收的条数够了就不再往下读（搜一个常见的词，书库会一次回上千本、上 MB）
        for (size_t at = scan_from; (at = result.body.find("</entry>", at)) != std::string::npos; at += 8) {
            ++entries;
        }
        if (entries >= kMaxEntries) {
            // 够了：主动关连接，不再等服务端把剩余几百本数据全发完
            result.truncated = true;
            http->Close();
            result.ok = true;
            return result;
        }
    }
    http->Close();
    result.ok = true;
    return result;
}

} // namespace valet::opds
