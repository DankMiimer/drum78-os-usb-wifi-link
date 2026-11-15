#ifndef RA_TOAST_OSD_H
#define RA_TOAST_OSD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "libpicofe/fonts.h"

#define RA_TOAST_W       232
#define RA_TOAST_H       30
#define RA_TOAST_X       4
#define RA_TOAST_SLIDE   3
#define RA_TOAST_SHADOW  2
#define RA_TOAST_IN_MS   160
#define RA_TOAST_OUT_MS  240

enum ra_toast_kind {
	RA_TOAST_INFO = 0,
	RA_TOAST_UNLOCK,
	RA_TOAST_MASTER,
};

struct ra_toast_theme {
	uint16_t bg_top;
	uint16_t bg_bot;
	uint16_t rim_top;
	uint16_t rim_bot;
	uint16_t well_bg;
	uint16_t well_rim;
	uint16_t title_fg;
	uint16_t accent_fg;
};

static inline uint16_t ra_blend565(uint16_t bg, uint16_t fg, unsigned alpha256)
{
	if (alpha256 >= 255)
		return fg;
	if (alpha256 == 0)
		return bg;

	unsigned inv = 256 - alpha256;
	unsigned br = (bg >> 11) & 0x1f;
	unsigned bgc = (bg >> 5) & 0x3f;
	unsigned bb = bg & 0x1f;

	unsigned fr = (fg >> 11) & 0x1f;
	unsigned fgc = (fg >> 5) & 0x3f;
	unsigned fb = fg & 0x1f;

	unsigned r = (br * inv + fr * alpha256) >> 8;
	unsigned g = (bgc * inv + fgc * alpha256) >> 8;
	unsigned b = (bb * inv + fb * alpha256) >> 8;

	return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t ra_lerp565(uint16_t c0, uint16_t c1, unsigned num, unsigned den)
{
	if (den == 0 || num == 0)
		return c0;
	if (num >= den)
		return c1;
	return ra_blend565(c0, c1, (num * 256U) / den);
}

static inline uint16_t ra_darken565(uint16_t bg, unsigned keep256)
{
	if (keep256 >= 255)
		return bg;
	unsigned r = (((bg >> 11) & 0x1f) * keep256) >> 8;
	unsigned g = (((bg >> 5) & 0x3f) * keep256) >> 8;
	unsigned b = ((bg & 0x1f) * keep256) >> 8;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline int ra_str_has(const char *hay, const char *needle)
{
	if (!hay || !needle)
		return 0;
	return strstr(hay, needle) != NULL;
}

static inline enum ra_toast_kind ra_toast_detect_kind(const char *line1)
{
	if (ra_str_has(line1, "Mastered"))
		return RA_TOAST_MASTER;
	if (ra_str_has(line1, "Unlocked") || ra_str_has(line1, "unlocked"))
		return RA_TOAST_UNLOCK;
	return RA_TOAST_INFO;
}

static inline void ra_toast_get_theme(enum ra_toast_kind kind, struct ra_toast_theme *t)
{
	switch (kind) {
	case RA_TOAST_UNLOCK:
		t->bg_top    = 0x18E5; /* #181C28 slate navy */
		t->bg_bot    = 0x1083; /* #101018 deep charcoal */
		t->rim_top   = 0xFF0C; /* #FFE060 bright warm gold */
		t->rim_bot   = 0xBA42; /* #B84810 warm bronze-gold */
		t->well_bg   = 0x2126; /* #202430 icon pocket */
		t->well_rim  = 0xECA3; /* #E89418 warm gold ring */
		t->title_fg  = 0xFEA8; /* #FFD440 rich RA gold */
		t->accent_fg = 0xFFFA; /* #FFF4D0 warm ivory (+pts) */
		break;
	case RA_TOAST_MASTER:
		t->bg_top    = 0x20E7; /* #201C38 royal indigo */
		t->bg_bot    = 0x1084; /* #101020 deep twilight */
		t->rim_top   = 0xFF72; /* #FFEC90 radiant gold */
		t->rim_bot   = 0xDC63; /* #D88C18 burnished gold */
		t->well_bg   = 0x2928; /* #282440 crown pocket */
		t->well_rim  = 0xFEE8; /* #FFDC40 gold rim */
		t->title_fg  = 0xFF4C; /* #FFE860 crown gold */
		t->accent_fg = 0xFFFF; /* crisp white */
		break;
	case RA_TOAST_INFO:
	default:
		t->bg_top    = 0x18E6; /* #181C30 cool slate */
		t->bg_bot    = 0x0863; /* #080C18 dark navy */
		t->rim_top   = 0x6DFD; /* #68BCE8 crisp sky blue */
		t->rim_bot   = 0x2AD3; /* #285898 deep RA blue */
		t->well_bg   = 0x1927; /* #182438 blue pocket */
		t->well_rim  = 0x4C5C; /* #4888E0 RA blue rim */
		t->title_fg  = 0x7E3F; /* #78C4FF bright RA sky */
		t->accent_fg = 0xFEAA; /* #FFD450 gold counter accent */
		break;
	}
}

static inline int ra_rect_inside(int dx, int dy, int w, int h, int radius)
{
	if (dx < 0 || dy < 0 || dx >= w || dy >= h)
		return 0;
	if (radius >= 2) {
		if ((dy == 0 || dy == h - 1) && (dx < 2 || dx >= w - 2))
			return 0;
		if ((dy == 1 || dy == h - 2) && (dx < 1 || dx >= w - 1))
			return 0;
	} else if (radius == 1) {
		if ((dy == 0 || dy == h - 1) && (dx == 0 || dx == w - 1))
			return 0;
	}
	return 1;
}

static inline int ra_rect_is_border(int dx, int dy, int w, int h, int radius)
{
	if (!ra_rect_inside(dx, dy, w, h, radius))
		return 0;
	return !ra_rect_inside(dx - 1, dy, w, h, radius) ||
	       !ra_rect_inside(dx + 1, dy, w, h, radius) ||
	       !ra_rect_inside(dx, dy - 1, w, h, radius) ||
	       !ra_rect_inside(dx, dy + 1, w, h, radius);
}

static inline unsigned char ra_sanitize_ascii(char ch)
{
	unsigned char uc = (unsigned char)ch;
	if (uc < 32 || uc > 126)
		return '?';
	return uc;
}

static inline void ra_glyph_metrics(char ch, int *min_col, int *max_col, int *adv)
{
	unsigned char uc = ra_sanitize_ascii(ch);
	if (uc == ' ') {
		*min_col = 0;
		*max_col = -1;
		*adv = 3;
		return;
	}

	const unsigned char *rows = fontdata6x8[uc];
	unsigned mask = 0;
	for (int r = 0; r < 8; r++)
		mask |= rows[r];
	mask &= 0x3f;

	if (mask == 0) {
		*min_col = 0;
		*max_col = -1;
		*adv = 4;
		return;
	}

	int lo = 6, hi = -1;
	for (int c = 0; c < 6; c++) {
		if (mask & (0x20U >> c)) {
			if (c < lo) lo = c;
			if (c > hi) hi = c;
		}
	}
	*min_col = lo;
	*max_col = hi;
	*adv = (hi - lo + 1) + 1;
}

static inline int ra_text_width(const char *s)
{
	if (!s || !*s)
		return 0;
	int w = 0;
	for (; *s; s++) {
		int lo, hi, adv;
		ra_glyph_metrics(*s, &lo, &hi, &adv);
		w += adv;
	}
	return w > 0 ? w - 1 : 0;
}

static inline void ra_draw_glyph_clip(uint16_t *dst, uint32_t w, uint32_t h, uint32_t pitch,
                                      int x, int y, char ch, uint16_t fg, unsigned alpha256,
                                      int lo, int hi)
{
	if (lo > hi)
		return;
	unsigned char uc = ra_sanitize_ascii(ch);
	const unsigned char *rows = fontdata6x8[uc];

	for (int r = 0; r < 8; r++) {
		int py = y + r;
		if (py < 0 || (uint32_t)py >= h)
			continue;
		unsigned bits = rows[r];
		for (int c = lo; c <= hi; c++) {
			if (!(bits & (0x20U >> c)))
				continue;
			int px = x + (c - lo);
			if (px < 0 || (uint32_t)px >= w)
				continue;
			/* 1px bottom-right shadow */
			if (px + 1 < (int)w && py + 1 < (int)h) {
				uint16_t *sp = &dst[(py + 1) * pitch + (px + 1)];
				unsigned dark_keep = 256 - (alpha256 * 175U) / 256U;
				*sp = ra_darken565(*sp, dark_keep);
			}
			uint16_t *p = &dst[py * pitch + px];
			*p = ra_blend565(*p, fg, alpha256);
		}
	}
}

static inline void ra_draw_text_fit(uint16_t *dst, uint32_t w, uint32_t h, uint32_t pitch,
                                    int x, int y, int max_w, const char *s,
                                    uint16_t base_fg, uint16_t accent_fg,
                                    int highlight_tokens, unsigned alpha256)
{
	if (!s || !*s || max_w <= 0)
		return;

	int full_w = ra_text_width(s);
	int ell_w = ra_text_width("...");
	int truncate = (full_w > max_w && max_w > ell_w);
	int limit_w = truncate ? (max_w - ell_w) : max_w;

	int cx = x;
	int in_accent = 0;

	for (const char *p = s; *p; p++) {
		int lo, hi, adv;
		ra_glyph_metrics(*p, &lo, &hi, &adv);
		int gw = (hi >= lo) ? (hi - lo + 1) : 0;
		if ((cx - x) + gw > limit_w) {
			if (truncate) {
				const char *dots = "...";
				for (; *dots; dots++) {
					int dlo, dhi, dadv;
					ra_glyph_metrics(*dots, &dlo, &dhi, &dadv);
					ra_draw_glyph_clip(dst, w, h, pitch, cx, y, *dots, base_fg, alpha256, dlo, dhi);
					cx += dadv;
				}
			}
			break;
		}

		if (highlight_tokens) {
			if (*p == '+' && p[1] >= '0' && p[1] <= '9') {
				in_accent = 1;
			} else if (*p >= '0' && *p <= '9' && (p == s || p[-1] == ' ')) {
				/* highlight X/Y counter token */
				const char *q = p;
				while (*q >= '0' && *q <= '9') q++;
				if (*q == '/')
					in_accent = 1;
			} else if (*p == ' ' && in_accent) {
				/* keep "pts" glued with +N if present */
				if (!(p[1] == 'p' && p[2] == 't'))
					in_accent = 0;
			}
		}

		uint16_t color = in_accent ? accent_fg : base_fg;
		ra_draw_glyph_clip(dst, w, h, pitch, cx, y, *p, color, alpha256, lo, hi);
		cx += adv;
	}
}

static inline void ra_draw_icon16(uint16_t *dst, uint32_t w, uint32_t h, uint32_t pitch,
                                  int x0, int y0, enum ra_toast_kind kind, unsigned alpha256)
{
	static const char *const icon_trophy[16] = {
		"  ............  ",
		" .yyyyyyyyggga. ",
		".gyyyygggggggaa.",
		".g .ygggggaa. a.",
		".g .ygggggaa. a.",
		" .g.ygggggaa.a. ",
		"  ..ygggggaa..  ",
		"   ..ygggaa..   ",
		"    ..ygaa..    ",
		"      .ga.      ",
		"      .ga.      ",
		"    ..ygaa..    ",
		"  ..yyyyggaa..  ",
		"  .aaaaaaaaaa.  ",
		"  ............  ",
		"                ",
	};

	static const char *const icon_crown[16] = {
		"                ",
		"      ....      ",
		"      .yy.      ",
		" ..   .yg.   .. ",
		".yy.  .yg.  .ga.",
		".yg. .ygga. .ga.",
		".ygy..ygga..gaa.",
		".yygyygggggaaaa.",
		".yygyrggccgaaaa.",
		".yggggggggggaaa.",
		".yyyyyyyygggaaa.",
		"..aaaaaaaaaaaa..",
		".gggggggggggaaa.",
		"................",
		"                ",
		"                ",
	};

	static const char *const icon_shield[16] = {
		"                ",
		"  ............  ",
		" .bbbbbbbbdddd. ",
		".bbbbbyyyddddbd.",
		".bbbbyyyyydddbd.",
		".bbbbyyyggdddbd.",
		".bbyyyyygggaadd.",
		".bbbyyygggaaadd.",
		" .bbbyygggaadd. ",
		" .bbbbyygadddd. ",
		"  .bbyyg.gadd.  ",
		"  .bbdd...ddd.  ",
		"   .bbdddddb.   ",
		"    ..dddd..    ",
		"      ....      ",
		"                ",
	};

	const char *const *art = icon_shield;
	if (kind == RA_TOAST_UNLOCK)
		art = icon_trophy;
	else if (kind == RA_TOAST_MASTER)
		art = icon_crown;

	for (int r = 0; r < 16; r++) {
		int py = y0 + r;
		if (py < 0 || (uint32_t)py >= h)
			continue;
		const char *row = art[r];
		for (int c = 0; c < 16; c++) {
			char ch = row[c];
			if (ch == ' ' || ch == '\0')
				continue;
			int px = x0 + c;
			if (px < 0 || (uint32_t)px >= w)
				continue;

			uint16_t col;
			switch (ch) {
			case '.': col = 0x2104; break; /* deep dark outline */
			case 'y': col = 0xFFF0; break; /* bright sunlit gold */
			case 'g': col = 0xFE44; break; /* rich classic gold */
			case 'a': col = 0xDCA0; break; /* warm amber shadow */
			case 'r': col = 0xF945; break; /* ruby gem */
			case 'c': col = 0x3F3F; break; /* cyan diamond gem */
			case 'b': col = 0x6DDF; break; /* bright RA sky blue */
			case 'd': col = 0x2355; break; /* deep RA royal blue */
			default:  col = 0xFFFF; break;
			}
			uint16_t *p = &dst[py * pitch + px];
			*p = ra_blend565(*p, col, alpha256);
		}
	}
}

static inline uint32_t ra_toast_base_y(uint32_t h, int is_mini)
{
	uint32_t y = is_mini ? 51U : 4U;
	if (y + RA_TOAST_H + RA_TOAST_SHADOW > h)
		y = (h > RA_TOAST_H + RA_TOAST_SHADOW) ? (h - RA_TOAST_H - RA_TOAST_SHADOW) : 0;
	return y;
}

static inline void ra_toast_clear_box(uint16_t *dst, uint32_t h, uint32_t pitch, int is_mini)
{
	uint32_t y0 = ra_toast_base_y(h, is_mini);
	uint32_t top = (y0 >= RA_TOAST_SLIDE) ? (y0 - RA_TOAST_SLIDE) : 0;
	uint32_t end = y0 + RA_TOAST_H + RA_TOAST_SHADOW;
	if (end > h)
		end = h;
	if (end > top)
		memset(dst + top * pitch, 0, (end - top) * pitch * sizeof(uint16_t));
}

static inline void ra_toast_draw(uint16_t *dst, uint32_t w, uint32_t h, uint32_t pitch,
                                 int is_mini, const char *line1, const char *line2,
                                 unsigned elapsed_ms, unsigned remain_ms)
{
	if ((!line1 || !line1[0]) && (!line2 || !line2[0]))
		return;

	unsigned anim_alpha = 256;
	int y_shift = 0;

	if (elapsed_ms < RA_TOAST_IN_MS) {
		anim_alpha = 40 + (elapsed_ms * 216U) / RA_TOAST_IN_MS;
		unsigned rem_slide = RA_TOAST_IN_MS - elapsed_ms;
		y_shift = -(int)((rem_slide * RA_TOAST_SLIDE + RA_TOAST_IN_MS - 1) / RA_TOAST_IN_MS);
	} else if (remain_ms < RA_TOAST_OUT_MS) {
		anim_alpha = (remain_ms * 256U) / RA_TOAST_OUT_MS;
		if (anim_alpha < 8)
			return;
	}

	int x0 = RA_TOAST_X;
	int bw = (x0 + RA_TOAST_W <= (int)w) ? RA_TOAST_W : ((int)w > x0 ? (int)w - x0 : (int)w);
	int y0 = (int)ra_toast_base_y(h, is_mini) + y_shift;
	if (y0 < 0)
		y0 = 0;

	enum ra_toast_kind kind = ra_toast_detect_kind(line1);
	struct ra_toast_theme theme;
	ra_toast_get_theme(kind, &theme);

	/* Soft rounded drop shadow behind bottom & right edge */
	for (int dy = 1; dy < RA_TOAST_H + RA_TOAST_SHADOW; dy++) {
		int py = y0 + dy;
		if (py < 0 || (uint32_t)py >= h)
			continue;
		for (int dx = 1; dx < bw + RA_TOAST_SHADOW; dx++) {
			int px = x0 + dx;
			if (px < 0 || (uint32_t)px >= w)
				continue;
			if (ra_rect_inside(dx, dy, bw, RA_TOAST_H, 2))
				continue;
			int near1 = ra_rect_inside(dx - 1, dy - 1, bw, RA_TOAST_H, 2);
			int near2 = ra_rect_inside(dx - 2, dy - 2, bw, RA_TOAST_H, 2) ||
			            ra_rect_inside(dx, dy - 1, bw, RA_TOAST_H, 2);
			if (!near1 && !near2)
				continue;
			unsigned sh_strength = near1 ? 150 : 85;
			unsigned keep = 256 - (sh_strength * anim_alpha) / 256U;
			uint16_t *p = &dst[py * pitch + px];
			*p = ra_darken565(*p, keep);
		}
	}

	/* Card backdrop + gradient + 1px metallic rim */
	unsigned card_alpha = (238U * anim_alpha) / 256U;
	unsigned rim_alpha = (248U * anim_alpha) / 256U;
	for (int dy = 0; dy < RA_TOAST_H; dy++) {
		int py = y0 + dy;
		if (py < 0 || (uint32_t)py >= h)
			continue;
		uint16_t row_bg = ra_lerp565(theme.bg_top, theme.bg_bot, (unsigned)dy, RA_TOAST_H - 1);
		uint16_t row_rim = ra_lerp565(theme.rim_top, theme.rim_bot, (unsigned)dy, RA_TOAST_H - 1);
		for (int dx = 0; dx < bw; dx++) {
			int px = x0 + dx;
			if (px < 0 || (uint32_t)px >= w)
				continue;
			if (!ra_rect_inside(dx, dy, bw, RA_TOAST_H, 2))
				continue;
			uint16_t *p = &dst[py * pitch + px];
			if (ra_rect_is_border(dx, dy, bw, RA_TOAST_H, 2)) {
				*p = ra_blend565(*p, row_rim, rim_alpha);
			} else if (dy == 1) {
				/* Subtle top inner highlight sheen */
				uint16_t sheen = ra_blend565(row_bg, theme.rim_top, 52);
				*p = ra_blend565(*p, sheen, card_alpha);
			} else {
				*p = ra_blend565(*p, row_bg, card_alpha);
			}
		}
	}

	/* Icon pocket (24x24) at left */
	const int well_x = 5;
	const int well_y = 3;
	const int well_sz = 24;
	for (int dy = 0; dy < well_sz; dy++) {
		int py = y0 + well_y + dy;
		if (py < 0 || (uint32_t)py >= h)
			continue;
		for (int dx = 0; dx < well_sz; dx++) {
			int px = x0 + well_x + dx;
			if (px < 0 || (uint32_t)px >= w)
				continue;
			if (!ra_rect_inside(dx, dy, well_sz, well_sz, 2))
				continue;
			uint16_t *p = &dst[py * pitch + px];
			if (ra_rect_is_border(dx, dy, well_sz, well_sz, 2))
				*p = ra_blend565(*p, theme.well_rim, (200U * anim_alpha) / 256U);
			else
				*p = ra_blend565(*p, theme.well_bg, (230U * anim_alpha) / 256U);
		}
	}

	ra_draw_icon16(dst, w, h, pitch, x0 + well_x + 4, y0 + well_y + 4, kind, anim_alpha);

	/* Text layout right of icon pocket */
	int text_x = x0 + well_x + well_sz + 6;
	int text_max_w = (x0 + bw - 6) - text_x;
	if (line1 && line1[0]) {
		ra_draw_text_fit(dst, w, h, pitch, text_x, y0 + 5, text_max_w, line1,
		                 theme.title_fg, theme.accent_fg, 1, anim_alpha);
	}
	if (line2 && line2[0]) {
		ra_draw_text_fit(dst, w, h, pitch, text_x, y0 + 16, text_max_w, line2,
		                 0xFFFF, 0xFFFF, 0, anim_alpha);
	}
}

#endif /* RA_TOAST_OSD_H */
