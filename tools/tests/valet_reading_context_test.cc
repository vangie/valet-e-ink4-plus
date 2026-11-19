#include "valet_reading_context.h"
#include <cassert>
#include <iostream>
int main() {
    using namespace valet;
    SelectStandaloneContext(); assert(!SelectedReadingContext().Valid()); assert(!SelectBookContext());
    PublishReadingPage("/sdcard/a.epub", "三国演义", "第一回", 2, "宴桃园豪杰三结义");
    assert(SelectBookContext()); auto first = SelectedReadingContext();
    assert(first.Valid() && first.page == 2 && first.title == "三国演义" && first.book_id.size() == 32);
    const auto key = ContextStorageKey("home");
    PublishReadingPage("/sdcard/a.epub", "三国演义", "第二回", 3, "下一页");
    assert(SelectedReadingContext().page == 2); // selected immutable snapshot stays fixed while recording
    assert(SelectBookContext() && SelectedReadingContext().page == 3);
    assert(ContextStorageKey("home") == key); // same book, same conversation
    assert(ContextStorageKey("work") != key);
    SelectStandaloneContext(); assert(!SelectedReadingContext().Valid()); assert(ContextStorageKey("home") != key);
    PublishReadingPage("/sdcard/b.epub", "三国演义", "第一回", 1, "另一个文件");
    assert(SelectBookContext() && ContextStorageKey("home") != key); // equal titles don't collide
    std::string text; for (int i=0;i<5000;++i) text += "汉";
    PublishReadingPage("/sdcard/c.epub", "字库", "正文", 1, text);
    assert(SelectBookContext()); auto clipped=SelectedReadingContext();
    assert(clipped.truncated && clipped.text.size() <= 12000 && clipped.text.size()%3==0);
    auto* json=ContextJson(clipped); assert(cJSON_IsObject(json)); cJSON_Delete(json);
    PublishReadingPage("/sdcard/image.cbz", "漫画", "", 1, ""); assert(!SelectBookContext());
    std::cout << "Reading contexts: rendered snapshots, book/assistant isolation, standalone clearing, UTF8 bounds passed\n";
}
