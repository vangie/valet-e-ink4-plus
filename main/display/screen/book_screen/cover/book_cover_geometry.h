#pragma once
#include <algorithm>

constexpr int kCoverFrameBorder = 3; // Crisp cover outline, shared by layout and drawing.

// 画廊极简风：封面采用直角（radius = 0），与整机瑞士网格线条一致
inline int BookCoverCornerRadius(int /*width*/, int /*height*/) {
    return 0;
}
inline bool BookCoverContainsPoint(int x1, int y1, int x2, int y2, int x, int y) {
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}
