"""生成随行首页的两套小字库（2bpp）：时钟数字（MiSans ExtraLight）和首页两块的标题（提问 / 通话 / 接着读 / 阅读）（MiSans Demibold）。

界面字库最大只有 30 px 的中文，标题要 40 px，所以只为这几个字单独生成。

    python3 tools/generate_valet_home_font.py --output main/display/screen/home_screen/home_valet_font.h
"""
from pathlib import Path
import argparse
from PIL import ImageFont

FONTS = Path(__file__).resolve().parent / "ttf-fonts/MiSans"


def font_source(name, weight, size, chars):
    chars = "".join(sorted(set(chars)))
    font = ImageFont.truetype(str(FONTS / f"MiSans-{weight}.ttf"), size)
    bitmap = bytearray()
    glyphs = ["{.bitmap_index=0,.adv_w=0,.box_w=0,.box_h=0,.ofs_x=0,.ofs_y=0}"]
    ascent = descent = 0
    for char in chars:
        mask, (left, top) = font.getmask2(char, mode="L", anchor="ls")
        width, height = mask.size
        ascent = max(ascent, -top)
        descent = max(descent, top + height)
        start = len(bitmap)
        values = bytes(mask)
        # LVGL 位图不按行对齐：像素连续排列，高位在前。
        packed = bytearray((len(values) * 2 + 7) // 8)
        for i, value in enumerate(values):
            packed[i // 4] |= ((value + 42) // 85) << (6 - 2 * (i % 4))
        bitmap.extend(packed)
        glyphs.append("{.bitmap_index=%d,.adv_w=%d,.box_w=%d,.box_h=%d,.ofs_x=%d,.ofs_y=%d}" %
                      (start, round(font.getlength(char) * 16), width, height, left, -top - height))
    lines = ["alignas(4) static const uint8_t %s_bitmap[] = {" % name]
    lines += [",".join("0x%02x" % n for n in bitmap[i:i + 24]) + "," for i in range(0, len(bitmap), 24)]
    lines += ["};", "static const lv_font_fmt_txt_glyph_dsc_t %s_glyphs[] = {" % name, ",\n".join(glyphs), "};",
              "static const uint16_t %s_unicode[] = {%s};" % (name, ",".join(str(ord(c) - ord(chars[0])) for c in chars)),
              "static const lv_font_fmt_txt_cmap_t %s_cmap[] = {{" % name,
              ".range_start=%d,.range_length=%d,.glyph_id_start=1,.unicode_list=%s_unicode," %
              (ord(chars[0]), ord(chars[-1]) - ord(chars[0]) + 1, name),
              ".glyph_id_ofs_list=nullptr,.list_length=%d,.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}};" % len(chars),
              "static const lv_font_fmt_txt_dsc_t %s_dsc = {" % name,
              ".glyph_bitmap=%s_bitmap,.glyph_dsc=%s_glyphs,.cmaps=%s_cmap," % (name, name, name),
              ".kern_dsc=nullptr,.kern_scale=0,.cmap_num=1,.bpp=2,.kern_classes=0,.bitmap_format=0};",
              "static const lv_font_t %s = {" % name,
              ".get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,",
              ".line_height=%d,.base_line=%d,.subpx=LV_FONT_SUBPX_NONE," % (ascent + descent, descent),
              ".underline_position=-1,.underline_thickness=1,.dsc=&%s_dsc," % name,
              ".fallback=nullptr,.user_data=nullptr};"]
    return "\n".join(lines), len(bitmap), ascent


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    clock, clock_bytes, cap = font_source("kValetClockFont", "ExtraLight", 132, "0123456789")
    title, title_bytes, _ = font_source("kValetTitleFont", "Demibold", 40, "提问通话接着读阅AskCalRed")
    # 日期块：公历一行细体数字；农历一行半粗，用字只有月名、日名和「闰」。
    date, date_bytes, _ = font_source("kValetDateFont", "Light", 32, "0123456789.-")
    lunar, lunar_bytes, _ = font_source("kValetLunarFont", "Demibold", 40, "正一二三四五六七八九十冬腊寒月初廿卅闰")
    clock_bytes += date_bytes + lunar_bytes
    header = ("// 由 tools/generate_valet_home_font.py 生成；源字体 tools/ttf-fonts/MiSans\n"
              "#pragma once\n#include <lvgl.h>\n" + clock + "\n" + title + "\n" + date + "\n" + lunar + "\n")
    Path(args.output).write_text(header, encoding="utf-8", newline="")
    print("Valet home fonts:", clock_bytes + title_bytes, "bytes; digit height", cap, "px")
