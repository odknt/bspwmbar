/* See LICENSE file for copyright and license details. */

#include "font.h"
#include "util.h"

static void bb_font_destroy(struct bb_font);

struct bb_font_manager *
bb_font_manager_new(double dpi)
{
	struct bb_font_manager *fm;

	fm = calloc(1, sizeof(struct bb_font_manager));
	fm->load_flag = FT_LOAD_COLOR | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT;
	fm->dpi = dpi;

	FT_Init_FreeType(&fm->ftlib);

	return fm;
}

void
bb_font_manager_destroy(struct bb_font_manager *fm)
{
	int i;

	cairo_font_options_destroy(fm->font_opt);
	bb_font_destroy(fm->font);

	for ( i = 0; i < fm->nfcache; i++)
		bb_font_destroy(fm->fcaches[i]);
	fm->nfcache = fm->fcachecap = 0;
	free(fm->fcaches);

	FcPatternDestroy(fm->pattern);
	FT_Done_FreeType(fm->ftlib);

	free(fm);
}

void
bb_font_destroy(struct bb_font font)
{
	cairo_font_face_destroy(font.cairo);
	hb_font_destroy(font.hb);
	FT_Done_Face(font.face);
}

/**
 * bb_font_manger_find_font() - finds a font that renderable specified rune.
 * @rune: FcChar32
 *
 * Return: struct bb_font *
 */
struct bb_font *
bb_font_manager_find_font(struct bb_font_manager *fm, FcChar32 rune)
{
	FcResult result;
	FcFontSet *fonts;
	FcPattern *pat;
	FcCharSet *charset;
	FcChar8 *path;
	FT_Face face;
	int i, idx;

	/* Lookup character index with default font. */
	if ((idx = FT_Get_Char_Index(fm->font.face, rune)))
		return &fm->font;

	/* fallback on font cache */
	for (i = 0; i < fm->nfcache; i++)
		if ((idx = FT_Get_Char_Index(fm->fcaches[i].face, rune)))
			return &fm->fcaches[i];

	/* find font when not found */
	if (i >= fm->nfcache) {
		if (fm->nfcache >= fm->fcachecap) {
			fm->fcachecap += 8;
			fm->fcaches = realloc(fm->fcaches, fm->fcachecap * sizeof(struct bb_font));
		}

		pat = FcPatternDuplicate(fm->pattern);
		charset = FcCharSetCreate();

		/* find font that contains rune and scalable */
		FcCharSetAddChar(charset, rune);
		FcPatternAddCharSet(pat, FC_CHARSET, charset);
		FcPatternAddBool(pat, FC_SCALABLE, 1);
		FcPatternAddBool(pat, FC_COLOR, 1);

		FcConfigSubstitute(NULL, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);

		fonts = FcFontSort(NULL, pat, 1, NULL, &result);
		FcPatternDestroy(pat);
		FcCharSetDestroy(charset);
		if (!fonts)
			return NULL;

		/* Allocate memory for the new cache entry. */
		if (fm->nfcache >= fm->fcachecap) {
			fm->fcachecap += 16;
			fm->fcaches = realloc(fm->fcaches, fm->fcachecap * sizeof(struct bb_font));
		}

		/* veirfy matched font */
		for (i = 0; i < fonts->nfont; i++) {
			pat = fonts->fonts[i];
			FcPatternGetString(pat, FC_FILE, 0, &path);
			if (FT_New_Face(fm->ftlib, (const char *)path, 0, &face)) {
				FcFontSetDestroy(fonts);
				return NULL;
			}
			if ((idx = FT_Get_Char_Index(face, rune)))
				break;
			FT_Done_Face(face);
			face = NULL;
		}
		FcFontSetDestroy(fonts);
		if (!face)
			return NULL;

		fm->fcaches[fm->nfcache].face = face;
		fm->fcaches[fm->nfcache].cairo = cairo_ft_font_face_create_for_ft_face(fm->fcaches[fm->nfcache].face, fm->load_flag);
		fm->fcaches[fm->nfcache].hb = hb_ft_font_create(face, NULL);

		i = fm->nfcache++;
	}
	return &fm->fcaches[i];
}

/**
 * load_fonts() - load fonts by specified fontconfig pattern string.
 * @patstr: pattern string.
 *
 * Return:
 * 0 - success
 * 1 - failure
 */
bool
bb_font_manager_load_fonts(struct bb_font_manager *fm, const char *patstr)
{
	FcChar8 *path;
	FcPattern *pat = FcNameParse((FcChar8 *)patstr);

	if (!pat)
		return false;

	/* get dpi and set to pattern */
	FcPatternAddDouble(pat, FC_DPI, fm->dpi);
	FcPatternAddBool(pat, FC_SCALABLE, 1);

	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	if (!match) {
		FcPatternDestroy(pat);
		return false;
	}

	FcPatternGetString(match, FC_FILE, 0, &path);
	if (FT_New_Face(fm->ftlib, (const char *)path, 0, &fm->font.face))
		return false;
	FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &fm->font_size);
	FcPatternDestroy(match);

	if (!fm->font.face) {
		FcPatternDestroy(pat);
		return false;
	}

	fm->font.cairo = cairo_ft_font_face_create_for_ft_face(fm->font.face, fm->load_flag);
	fm->font.hb = hb_ft_font_create(fm->font.face, NULL);
	fm->pattern = pat;

	fm->font_opt = cairo_font_options_create();
	cairo_font_options_set_antialias(fm->font_opt, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_subpixel_order(fm->font_opt, CAIRO_SUBPIXEL_ORDER_RGB);
	cairo_font_options_set_hint_style(fm->font_opt, CAIRO_HINT_STYLE_SLIGHT);
	cairo_font_options_set_hint_metrics(fm->font_opt, CAIRO_HINT_METRICS_ON);

	/* padding width */
	fm->celwidth = fm->font_size / 2 - 1;

	return true;
}
