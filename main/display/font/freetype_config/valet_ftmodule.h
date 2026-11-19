/*
 * FreeType module list for this firmware.
 *
 * The reader opens only .ttf and .otf (book_font_runtime.cc), and renders with
 * FT_RENDER_MODE_NORMAL. These are the modules those two formats need:
 *   truetype + sfnt        TrueType outlines and the SFNT container
 *   cff + psaux + pshinter CFF outlines inside OpenType
 *   psnames                glyph names used by sfnt / cff
 *   autofit                hinting for FT_LOAD_TARGET_NORMAL
 *   smooth                 anti-aliased rendering
 *
 * Left out, because no file the device opens can reach them: Type 1, CID,
 * PFR, Type 42, Windows FNT, PCF, BDF, the monochrome rasterizer, SDF and SVG.
 * Upstream's full list is freetype/include/freetype/config/ftmodule.h.
 */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
