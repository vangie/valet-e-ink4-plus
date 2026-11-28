#include "book_screen/cover/book_cover_style.h"
#include "book_screen/cover/book_cover_geometry.h"
#include "fontpack_lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

#include <lvgl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// 实心墨点（无 AA），直线/圆弧同一套，避免墨色深浅不一
static void CoverInkFill(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink, lv_coord_t x1,
                         lv_coord_t y1, lv_coord_t x2, lv_coord_t y2) {
    if (x1 > x2 || y1 > y2) {
        return;
    }
    lv_area_t a;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    lv_draw_rect(layer, &ink, &a);
}

namespace {
// Share immutable masks between equal-sized covers; retain no pixel data after
// the last cover using a size is deleted. LVGL applies the mask to the complete
// object layer (background, children and border), before compositing below UI.
struct CoverMask {
    lv_image_dsc_t image{};
    uint8_t* pixels = nullptr;
    ~CoverMask() {
        lv_image_cache_drop(&image);
        lv_image_header_cache_drop(&image);
        lv_free(pixels);
    }
};
std::vector<std::weak_ptr<CoverMask>> s_masks;
struct CoverFrameState {
    std::shared_ptr<CoverMask> mask;
    reader::BookSource source = reader::BookSource::kLocal;
};

struct CornerRibbonDsc {
    lv_image_dsc_t dsc{};
    std::vector<uint8_t> pixels;
    bool built = false;
};
CornerRibbonDsc s_ribbons[4];

int CornerArcMaxX(int dy, int side, int radius) {
    if (dy >= radius) return side - 1;
    const int cx = side - 1 - radius;
    const int t = radius - dy;
    const int dx = static_cast<int>(std::floor(std::sqrt(static_cast<float>(radius * radius - t * t))));
    return std::min(side - 1, cx + dx);
}

bool UnpackFont25Glyph(uint32_t cp, int& gw, int& gh, std::vector<uint8_t>& a8) {
    const lv_font_t* font = fontpack_lv_font_get(25, 2);
    if (!font) font = fontpack_lv_font_ui();
    if (!font) return false;
    lv_font_glyph_dsc_t gd{};
    if (!lv_font_get_glyph_dsc(font, &gd, cp, 0) || gd.box_w <= 0 || gd.box_h <= 0) return false;
    gd.req_raw_bitmap = 1;
    const auto* raw = static_cast<const uint8_t*>(font->get_glyph_bitmap(&gd, nullptr));
    if (!raw) return false;
    gw = gd.box_w;
    gh = gd.box_h;
    a8.assign(static_cast<size_t>(gw) * gh, 0);
    const uint16_t bpp = fontpack_lv_font_bpp(font);
    const unsigned bits = (bpp == 1 || bpp == 2 || bpp == 4) ? bpp : 2;
    const unsigned per_byte = 8 / bits;
    const unsigned mask = (1u << bits) - 1;
    uint32_t pos = 0;
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            const unsigned shift = (per_byte - 1 - (pos % per_byte)) * bits;
            const unsigned v = (raw[pos / per_byte] >> shift) & mask;
            a8[static_cast<size_t>(y) * gw + x] = static_cast<uint8_t>(v * 255 / mask);
            ++pos;
        }
    }
    return true;
}

const lv_image_dsc_t* GetSourceCornerRibbon(reader::BookSource src) {
    const int idx = static_cast<int>(src);
    if (idx <= 0 || idx >= 4) return nullptr;
    auto& slot = s_ribbons[idx];
    if (slot.built) return &slot.dsc;

    uint32_t c0 = 0, c1 = 0;
    if (src == reader::BookSource::kWeRead) {
        c0 = 0x5FAE; c1 = 0x8BFB; // 微读
    } else if (src == reader::BookSource::kShuzhai) {
        c0 = 0x4E66; c1 = 0x658B; // 书斋
    } else {
        c0 = 0x4E66; c1 = 0x5E93; // 书库
    }

    constexpr int kSide = 88;
    constexpr int kRadius = 28;
    constexpr int kSMin = 38;
    constexpr int kSMax = 78;
    constexpr float kInvSqrt2 = 0.70710678f;

    slot.pixels.assign(static_cast<size_t>(kSide) * kSide * 2, 0);
    auto set_px = [&](int x, int y, uint8_t lum, uint8_t alpha) {
        if (x < 0 || x >= kSide || y < 0 || y >= kSide) return;
        const size_t off = (static_cast<size_t>(y) * kSide + x) * 2;
        slot.pixels[off] = lum;
        slot.pixels[off + 1] = alpha;
    };

    for (int y = 0; y < kSide; ++y) {
        const int max_x = CornerArcMaxX(y, kSide, kRadius);
        for (int x = 0; x <= max_x; ++x) {
            const int s = (kSide - 1 - x) + y;
            if (s < kSMin || s > kSMax) continue;
            const bool is_edge = (s <= kSMin + 1 || s >= kSMax - 1);
            if (src == reader::BookSource::kWeRead) {
                // 微信读书：黑底 + 细白内线
                const bool inner_line = (s == kSMin + 3 || s == kSMax - 3);
                set_px(x, y, inner_line ? 255 : 0, 255);
            } else if (src == reader::BookSource::kShuzhai) {
                // 书斋：白底双黑边
                const bool inner_border = (s == kSMin + 4 || s == kSMax - 4);
                set_px(x, y, (is_edge || inner_border) ? 0 : 255, 255);
            } else {
                // 在线书库：灰网点底 + 黑边
                const uint8_t check = ((x + y) & 1) ? 255 : 190;
                set_px(x, y, is_edge ? 0 : check, 255);
            }
        }
    }

    int gw0 = 0, gh0 = 0, gw1 = 0, gh1 = 0;
    std::vector<uint8_t> g0, g1;
    if (UnpackFont25Glyph(c0, gw0, gh0, g0) && UnpackFont25Glyph(c1, gw1, gh1, g1)) {
        const int gap = 2;
        const int tw = gw0 + gap + gw1;
        const int th = std::max(gh0, gh1);
        std::vector<uint8_t> strip(static_cast<size_t>(tw) * th, 0);
        const int y0_off = (th - gh0) / 2;
        const int y1_off = (th - gh1) / 2;
        for (int y = 0; y < gh0; ++y) {
            for (int x = 0; x < gw0; ++x) {
                strip[static_cast<size_t>(y + y0_off) * tw + x] = g0[static_cast<size_t>(y) * gw0 + x];
            }
        }
        for (int y = 0; y < gh1; ++y) {
            for (int x = 0; x < gw1; ++x) {
                strip[static_cast<size_t>(y + y1_off) * tw + (gw0 + gap + x)] = g1[static_cast<size_t>(y) * gw1 + x];
            }
        }

        const float s_mid = 0.5f * (kSMin + kSMax);
        const float cx = (kSide - 1) - 0.5f * s_mid;
        const float cy = 0.5f * s_mid;
        const bool white_text = (src == reader::BookSource::kWeRead);
        // 25px 字形以 0.76x 缩放到 45° 斜带上（字高约 18px，饱满锐利）
        constexpr float kGlyphScale = 1.30f;

        for (int y = 0; y < kSide; ++y) {
            const int max_x = CornerArcMaxX(y, kSide, kRadius);
            for (int x = 0; x <= max_x; ++x) {
                const int s = (kSide - 1 - x) + y;
                if (s <= kSMin + 4 || s >= kSMax - 4) continue;
                const float dx = static_cast<float>(x) - cx;
                const float dy = static_cast<float>(y) - cy;
                const float u = (dx + dy) * kInvSqrt2 * kGlyphScale + 0.5f * tw;
                const float v = (-dx + dy) * kInvSqrt2 * kGlyphScale + 0.5f * th;
                const int iu = static_cast<int>(std::floor(u));
                const int iv = static_cast<int>(std::floor(v));
                if (iu < 0 || iu >= tw - 1 || iv < 0 || iv >= th - 1) continue;
                const float fu = u - iu;
                const float fv = v - iv;
                const float a00 = strip[static_cast<size_t>(iv) * tw + iu];
                const float a10 = strip[static_cast<size_t>(iv) * tw + (iu + 1)];
                const float a01 = strip[static_cast<size_t>(iv + 1) * tw + iu];
                const float a11 = strip[static_cast<size_t>(iv + 1) * tw + (iu + 1)];
                const float a = (1.0f - fu) * (1.0f - fv) * a00 + fu * (1.0f - fv) * a10 +
                                (1.0f - fu) * fv * a01 + fu * fv * a11;
                if (a > 36.0f) {
                    const size_t off = (static_cast<size_t>(y) * kSide + x) * 2;
                    const uint8_t bg = slot.pixels[off];
                    const uint8_t fg = white_text ? 255 : 0;
                    const int blended = (static_cast<int>(fg) * static_cast<int>(a) +
                                         static_cast<int>(bg) * (255 - static_cast<int>(a))) / 255;
                    slot.pixels[off] = static_cast<uint8_t>(std::clamp(blended, 0, 255));
                }
            }
        }
    }

    slot.dsc = {};
    slot.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot.dsc.header.cf = LV_COLOR_FORMAT_AL88;
    slot.dsc.header.w = kSide;
    slot.dsc.header.h = kSide;
    slot.dsc.header.stride = kSide * 2;
    slot.dsc.data_size = slot.pixels.size();
    slot.dsc.data = slot.pixels.data();
    slot.built = true;
    return &slot.dsc;
}

std::shared_ptr<CoverMask> GetCoverMask(int width, int height) {
    for (auto it=s_masks.begin(); it!=s_masks.end();) {
        if (auto mask=it->lock()) {
            if (mask->image.header.w==width && mask->image.header.h==height) return mask;
            ++it;
        } else it=s_masks.erase(it);
    }
    auto mask=std::make_shared<CoverMask>();
    const uint32_t stride=lv_draw_buf_width_to_stride(width,LV_COLOR_FORMAT_A8);
    const size_t bytes=size_t(stride)*height;
    mask->pixels=static_cast<uint8_t*>(lv_malloc(bytes));
    LV_ASSERT_MALLOC(mask->pixels);
    if (!mask->pixels) return {};
    std::memset(mask->pixels,255,bytes);
    const int radius=BookCoverCornerRadius(width,height),cx=width-1-radius;
    // Match the existing border's integer circle exactly. Left corners stay square.
    for (int dy=0;dy<radius;++dy) {
        const int t=radius-dy;
        const int first=cx+int(std::floor(std::sqrt(float(radius*radius-t*t))))+1;
        std::memset(mask->pixels+size_t(dy)*stride+first,0,width-first);
        std::memset(mask->pixels+size_t(height-1-dy)*stride+first,0,width-first);
    }
    auto& image=mask->image;
    image.header.magic=LV_IMAGE_HEADER_MAGIC;
    image.header.cf=LV_COLOR_FORMAT_A8;
    image.header.w=width; image.header.h=height; image.header.stride=stride;
    image.data=mask->pixels; image.data_size=bytes;
    s_masks.push_back(mask);
    return mask;
}

void UpdateCoverMask(lv_obj_t* obj, CoverFrameState& state) {
    const int width=lv_obj_get_width(obj),height=lv_obj_get_height(obj);
    if (width<=0 || height<=0) return;
    if (state.mask && state.mask->image.header.w==width && state.mask->image.header.h==height) return;
    auto mask=GetCoverMask(width,height);
    if (!mask) return;
    lv_obj_set_style_bitmap_mask_src(obj,&mask->image,0);
    state.mask=std::move(mask);
}
} // namespace

// 右上/右下四分之一圆外轮廓：Bresenham 连续实心点，与直线边重叠 1px 衔接
static void DrawBookCoverCornerArcs(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink,
                                    const lv_area_t& box, lv_coord_t r) {
    const lv_coord_t bw = kCoverFrameBorder;
    const lv_coord_t cx = box.x2 - r;
    const lv_coord_t cy_top = box.y1 + r;
    const lv_coord_t cy_bot = box.y2 - r;
    // Rasterize the annulus as solid scanlines inside the cover bounds.
    const int inner=std::max<int>(0,r-bw);
    for(int dy=0;dy<=r;++dy) {
        const int outer_x=static_cast<int>(std::floor(std::sqrt(static_cast<float>(r*r-dy*dy))));
        const int inner_x=dy<inner?static_cast<int>(std::floor(std::sqrt(static_cast<float>(inner*inner-dy*dy))))+1:0;
        const int first=std::min<int>(outer_x,inner_x);
        CoverInkFill(layer,ink,cx+first,cy_top-dy,cx+outer_x,cy_top-dy);
        CoverInkFill(layer,ink,cx+first,cy_bot+dy,cx+outer_x,cy_bot+dy);
    }
}

void DrawBookCoverBorder(lv_layer_t* layer, const lv_area_t& box, lv_coord_t r) {
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_black();
    ink.bg_opa = LV_OPA_COVER;
    ink.border_width = 0;
    ink.radius = 0;
    const lv_coord_t bw = kCoverFrameBorder;
    const lv_coord_t cx = box.x2 - r;

    // 左边框（整高）
    CoverInkFill(layer, ink, box.x1, box.y1, box.x1 + bw - 1, box.y2);
    // 左侧书脊压痕细线（距左外缘 bw + 5 处画 1px 竖线，赋予所有书籍实体装订质感）
    if (box.x2 - box.x1 > 32 && box.y2 - box.y1 > 32) {
        const lv_coord_t crease_x = box.x1 + bw + 5;
        CoverInkFill(layer, ink, crease_x, box.y1 + bw, crease_x, box.y2 - bw);
    }
    // 顶/底直线接到圆角起点（含 cx，与弧端点重叠）
    CoverInkFill(layer, ink, box.x1, box.y1, cx, box.y1 + bw - 1);
    CoverInkFill(layer, ink, box.x1, box.y2 - bw + 1, cx, box.y2);
    // 右侧直线（含圆角端点行，与弧重叠）
    CoverInkFill(layer, ink, box.x2 - bw + 1, box.y1 + r, box.x2, box.y2 - r);

    DrawBookCoverCornerArcs(layer, ink, box, r);
}

void OnBookCoverFrameDraw(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) {
        return;
    }
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    const lv_coord_t w = lv_area_get_width(&box);
    const lv_coord_t h = lv_area_get_height(&box);
    if (w < 8 || h < 8) {
        return;
    }
    const lv_coord_t r = BookCoverCornerRadius(w, h);
    auto* state = static_cast<CoverFrameState*>(lv_event_get_user_data(e));
    if (state && state->source != reader::BookSource::kLocal) {
        if (const lv_image_dsc_t* rib = GetSourceCornerRibbon(state->source)) {
            lv_draw_image_dsc_t idsc;
            lv_draw_image_dsc_init(&idsc);
            idsc.src = rib;
            lv_area_t ra;
            ra.x2 = box.x2;
            ra.x1 = box.x2 - static_cast<lv_coord_t>(rib->header.w) + 1;
            ra.y1 = box.y1;
            ra.y2 = box.y1 + static_cast<lv_coord_t>(rib->header.h) - 1;
            lv_draw_image(layer, &idsc, &ra);
        }
    }
    DrawBookCoverBorder(layer, box, r);
}

namespace {
void OnCoverFrameEvent(lv_event_t* e) {
    if (lv_event_get_target(e)!=lv_event_get_current_target(e)) return;
    auto* obj=static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* state=static_cast<CoverFrameState*>(lv_event_get_user_data(e));
    switch (lv_event_get_code(e)) {
        case LV_EVENT_SIZE_CHANGED: UpdateCoverMask(obj,*state); break;
        case LV_EVENT_DRAW_POST_END: OnBookCoverFrameDraw(e); break;
        case LV_EVENT_DELETE: delete state; break;
        default: break;
    }
}
} // namespace

// Left spine remains square; native layer masking reveals any background at
// the right corners. Do not paint white to simulate transparent geometry.
void StyleBookCoverFrame(lv_obj_t* obj) {
    if (!obj) return;
    for (uint32_t i=0;i<lv_obj_get_event_count(obj);++i) {
        auto* event=lv_obj_get_event_dsc(obj,i);
        if (lv_event_dsc_get_cb(event)==OnCoverFrameEvent) {
            UpdateCoverMask(obj,*static_cast<CoverFrameState*>(lv_event_dsc_get_user_data(event)));
            return;
        }
    }
    auto* state=new CoverFrameState;
    lv_obj_set_style_radius(obj,0,0);
    lv_obj_set_style_clip_corner(obj,false,0);
    lv_obj_set_style_border_width(obj,0,0);
    lv_obj_add_event_cb(obj,OnCoverFrameEvent,LV_EVENT_ALL,state);
    UpdateCoverMask(obj,*state);
}

void SetBookCoverFrameSource(lv_obj_t* obj, reader::BookSource source) {
    if (!obj) return;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        auto* event = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(event) == OnCoverFrameEvent) {
            auto* state = static_cast<CoverFrameState*>(lv_event_dsc_get_user_data(event));
            if (state && state->source != source) {
                state->source = source;
                lv_obj_invalidate(obj);
            }
            return;
        }
    }
}
