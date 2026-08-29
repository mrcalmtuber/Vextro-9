/*
 * third_party/freetype-port/ftmodule.h — which drivers FreeType builds.
 *
 * FreeType's default list is every format it has ever supported: Type 1,
 * CFF, CID, PFR, PCF, BDF, Windows FNT, Type 42, plus the validators for
 * OpenType and TrueTypeGX. That is most of nineteen megabytes of source
 * and every byte of it would be linked into a ring-3 program here.
 *
 * This system has one font format. The volume ships a TrueType face, the
 * kernel's own rasteriser in src/ttf.h parses TrueType, and what WebKit
 * would ask for is TrueType and OpenType outlines. So the list is three
 * modules:
 *
 *   truetype   the outline driver — glyf, loca, hmtx, cmap
 *   sfnt       the container those tables live in, shared with OpenType
 *   smooth     the anti-aliasing rasteriser
 *
 * and two helpers the two above genuinely need: psnames, because sfnt
 * maps glyph names through it, and the sfnt/truetype pair is not
 * buildable without it.
 *
 * The order matters and is FreeType's, not ours: drivers are tried in
 * the order they appear here.
 */

FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
