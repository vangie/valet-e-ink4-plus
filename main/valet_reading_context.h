#pragma once
#include <string>
#include <cJSON.h>
namespace valet {
struct ReadingContext {
    std::string book_id, title, chapter, text;
    int page = 0;
    bool truncated = false;
    bool Valid() const { return !book_id.empty() && page > 0 && !text.empty(); }
};
// UI publishes an immutable copy of the page actually rendered; never reads SD.
void PublishReadingPage(const std::string& path, const std::string& title,
                        const std::string& chapter, int page, const std::string& text);
ReadingContext LatestReadingPage();
bool SelectBookContext();
void SelectStandaloneContext();
ReadingContext SelectedReadingContext();
std::string ContextStorageKey(const std::string& assistant_id);
cJSON* ContextJson(const ReadingContext& context);
}
