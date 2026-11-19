/*
 * FreeType options for this firmware: upstream's generated ftoption.h, then
 * the changes below.
 *
 * The Adobe Glyph List maps PostScript glyph names to Unicode. FreeType uses
 * it only to build a Unicode cmap for Type 1 and bare CFF fonts. TTF and OTF
 * carry their own cmap, and the reader opens nothing else (see
 * valet_ftmodule.h), so the ~55 KB table is never consulted.
 */
#include <freetype/config/ftoption.h>

#undef FT_CONFIG_OPTION_ADOBE_GLYPH_LIST
