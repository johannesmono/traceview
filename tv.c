/* === libc & util ============================================================================== */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TV_STR(s)      #s
#define TV_XSTR(s)     TV_STR(s)
#define TV_ROL32(x, k) (((x) << (k)) | ((x) >> (32 - (k))))

#define TV_ASSERT(c)                                                                               \
	do {                                                                                       \
		if (c)                                                                             \
			break;                                                                     \
		fflush(stdout);                                                                    \
		fprintf(                                                                           \
		    stderr, "[!] tv: " __FILE__ ":" TV_XSTR(__LINE__) ": assertion failed: %s.\n", \
		    #c                                                                             \
		);                                                                                 \
		exit(1);                                                                           \
	} while (0)
#define TV_ERROR(s, ...)                                                                           \
	do {                                                                                       \
		fprintf(stderr, "[!] tv: error: " s ".\n", __VA_ARGS__);                           \
		exit(1);                                                                           \
	} while (0)

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;

static float
tv_absf(float x)
{
	return x < 0 ? -x : x;
}

static void
tv_prng_init(u32 state[4], u32 seed)
{
	u32 x = (seed += 0x9e3779b9);
	x = (x ^ (x >> 16)) * 0x85ebca6b;
	x = (x ^ (x >> 13)) * 0xc2b2ae35;
	state[0] = x ^ (x >> 16);

	x = (seed += 0x9e3779b9);
	x = (x ^ (x >> 16)) * 0x85ebca6b;
	x = (x ^ (x >> 13)) * 0xc2b2ae35;
	state[1] = x ^ (x >> 16);
}

static u32
tv_prng_squeeze(u32 state[4])
{
	u32 r = TV_ROL32(state[0] + state[3], 7) + state[0];
	u32 t = state[1] << 9;
	state[2] ^= state[0];
	state[3] ^= state[1];
	state[1] ^= state[2];
	state[0] ^= state[3];
	state[2] ^= t;
	state[3] = TV_ROL32(state[3], 11);
	return r;
}


/* === nuklear & sokol ========================================================================== */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_STANDARD_BOOL
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear/nuklear.h"

#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
#include "sokol/util/sokol_gl.h"
#include "sokol/util/sokol_nuklear.h"

NK_API nk_bool
nk_button_behavior(nk_flags *, struct nk_rect, const struct nk_input *, enum nk_button_behavior);

static nk_bool
tv_nk_filter_uint(struct nk_text_edit const *box, nk_rune r)
{
	(void)box;
	return '0' <= r && r <= '9';
}

static nk_bool
tv_nk_filter_filter(struct nk_text_edit const *box, nk_rune r)
{
	return nk_filter_decimal(box, r) || r == ',' || r == '!';
}


/* === tv ======================================================================================= */
#define TV_COLOR_HIGHLIGHT "2aa198"
#define TV_COLOR_RED       "dc322f"
#define TV_COLOR_MASK      0xFF404040

#define TV_FILTER_MAX      512
#define TV_FILTER_INVERT   (1 << 0)
#define TV_FILTER_RANGE    (1 << 1)
#define TV_FILTER_EQUAL    (1 << 2)

#define TV_HIGHLIGHT_MAX   128

#define TV_SGL_VERTICES    (1 << 25)
#define TV_SGL_COMMANDS    (1 << 23)

typedef struct tv_color_picker {
	int len;
	char buf[7];
	char picking;
} tv_color_picker;

typedef struct tv_filter {
	u16 bounds[2];
	u8 type;
} tv_filter;

typedef struct tv_highlight {
	struct nk_color color;
	u16 trace;
} tv_highlight;

static struct {
	char *filepath;
	u16 traces;
	u32 points;
	s8 *data;

	u32 prng[4];
	sg_sampler sampler;

	tv_filter filters[TV_FILTER_MAX];
	u16 filters_len;

	tv_highlight highlights[TV_HIGHLIGHT_MAX];
	tv_color_picker highlight_pickers[TV_HIGHLIGHT_MAX];
	u16 highlights_len;
} tv_global = { 0 };

static void
tv_sapp_init(void)
{
	sg_setup(&(sg_desc){
	    .environment = sglue_environment(),
	    .logger.func = slog_func,
	});
	snk_setup(&(snk_desc_t){
	    .dpi_scale = sapp_dpi_scale(),
	    .logger.func = slog_func,
	});
	sgl_setup(&(sgl_desc_t){
	    .depth_format = SG_PIXELFORMAT_NONE,
	    .max_vertices = TV_SGL_VERTICES,
	    .max_commands = TV_SGL_COMMANDS,
	    .logger.func = slog_func,
	});

	tv_prng_init(tv_global.prng, 0xdeadbeef);
	tv_global.sampler = sg_make_sampler(&(sg_sampler_desc){
	    .min_filter = SG_FILTER_LINEAR,
	    .mag_filter = SG_FILTER_LINEAR,
	    .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
	    .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
	});

	FILE *f = fopen(tv_global.filepath, "r");
	if (f == 0)
		TV_ERROR("could not open file %s", tv_global.filepath);

	u32 traces, points;
	TV_ASSERT(fread(&traces, sizeof traces, 1, f) == 1);
	if (traces >= (1 << 16))
		TV_ERROR("%s", "number of traces does not fit into u16");
	TV_ASSERT(fread(&points, sizeof points, 1, f) == 1);
	TV_ASSERT(fseek(f, sizeof traces, SEEK_SET) == 0);
	tv_global.traces = (u16)traces;
	tv_global.points = points;

	u64 len = (u64)traces * points;
	s8 *d = calloc(len, sizeof *d);
	if (d == 0)
		TV_ERROR("%s", "could not allocate enough memory");
	tv_global.data = d;

	while (traces--) {
		u32 p;
		TV_ASSERT(fread(&p, sizeof p, 1, f) == 1);
		if (p != points)
			TV_ERROR("expected %" PRIu32 " points, trace has %" PRIu32, points, p);
		TV_ASSERT(fseek(f, 256, SEEK_CUR) == 0);
		TV_ASSERT(fread(d, sizeof *d, points, f) == points);
		d += points;
	}
	fclose(f);
}

static void
tv_sapp_cleanup(void)
{
	sg_destroy_sampler(tv_global.sampler);
	free(tv_global.data);

	sgl_shutdown();
	snk_shutdown();
	sg_shutdown();
}

static void
tv_sapp_event(sapp_event const *e)
{
	if (e->type == SAPP_EVENTTYPE_KEY_DOWN) {
		if (e->key_code == SAPP_KEYCODE_Q && (e->modifiers & SAPP_MODIFIER_SUPER))
			sapp_request_quit();
	}
	snk_handle_event(e);
}

static nk_bool
tv_button_color_picker(struct nk_context *ctx, struct nk_color color, struct nk_vec2 padding)
{
	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
	struct nk_rect bounds = nk_widget_bounds(ctx);

	nk_style_push_float(ctx, &ctx->style.button.border, 0);
	nk_bool ret = nk_button_color(ctx, ctx->style.window.background);
	nk_style_pop_float(ctx);
	bounds.x += padding.x;
	bounds.y += padding.y;
	bounds.w = 12;
	bounds.h = 12;
	nk_fill_circle(canvas, bounds, color);

	return ret;
}

static struct nk_color
tv_color_pick(struct nk_context *ctx, char buf[7], int *len)
{
	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);

	nk_style_push_float(ctx, &ctx->style.window.spacing.x, 0);
	nk_layout_row(ctx, NK_STATIC, 132, 2, (float[]){ 1, 141 });
	nk_spacing(ctx, 1);
	struct nk_rect b = nk_widget_bounds(ctx);
	struct nk_color c = nk_rgb_hex(buf);
	struct nk_colorf cf = nk_color_cf(c);
	if (nk_color_pick(ctx, &cf, NK_RGB)) {
		c = nk_rgb_cf(cf);
		nk_color_hex_rgb(buf, c);
		*len = 6;
	}
	b.w = 129;
	nk_stroke_rect(canvas, b, 0, 1, c);
	nk_style_pop_float(ctx);

	nk_layout_row(ctx, NK_STATIC, 24, 2, (float[]){ 32, 94 });
	nk_label(ctx, "hex:", NK_TEXT_LEFT);
	nk_edit_string(ctx, NK_EDIT_FIELD, buf, len, 7, nk_filter_hex);
	for (int i = *len; i < 6; ++i)
		buf[i] = '0';

	return nk_rgb_hex(buf);
}

static bool
tv_filters_parse(char const *s)
{
	tv_global.filters_len = 0;
	tv_filter *f = tv_global.filters;
	u16 traces = tv_global.traces;

	char cpy[TV_FILTER_MAX] = { 0 };
	strcpy(cpy, s);
	char *p = cpy;

	u16 flen = 0;
	int last = 0;
	while (!last) {
		char *next = strchr(p, ',');
		if (next == 0)
			last = 1;
		else
			*next = 0;
		if (*p == 0)
			return false;

		u8 type = 0;
		if (*p == '!') {
			type |= TV_FILTER_INVERT;
			if (*++p == 0)
				return false;
		}

		char *end;
		long bound0 = strtol(p, &end, 0);
		if (bound0 > traces)
			return false;
		f->bounds[0] = (u16)bound0;

		if (*end == 0)
			type |= TV_FILTER_EQUAL;
		else if (*end != '-')
			return false;
		else {
			p = end + 1;
			if (*p == 0)
				return false;
			type |= TV_FILTER_RANGE;
			long bound1 = strtol(p, &end, 0);
			if (*end != 0 || bound1 > traces || bound1 <= bound0)
				return false;
			f->bounds[1] = (u16)bound1;
		}
		f->type = type;

		++f;
		++flen;
		p = next + 1;
	}
	tv_global.filters_len = flen;
	return true;
}

static void
tv_zoom(struct nk_input *in, struct nk_rect *zoom, struct nk_rect bounds, struct nk_vec2 min)
{
	if (nk_input_is_mouse_hovering_rect(in, bounds)) {
		if (nk_input_is_mouse_down(in, NK_BUTTON_LEFT)) {
			zoom->x -= in->mouse.delta.x / bounds.w * zoom->w;
			zoom->y += in->mouse.delta.y / bounds.h * zoom->h;
		} else {
			float dm = in->mouse.scroll_delta.y * .05f;
			float mx = (in->mouse.pos.x - bounds.x) / bounds.w;
			float zw = NK_CLAMP(min.x, zoom->w + dm * zoom->w, 1);
			zoom->x += (zoom->w - zw) * mx;
			zoom->w = zw;

			if (!nk_input_is_key_down(in, NK_KEY_CTRL)) {
				float my = (in->mouse.pos.y - bounds.y) / bounds.h;
				float zh = NK_CLAMP(min.y, zoom->h + dm * zoom->h, 1);
				zoom->y += (zoom->h - zh) * (1 - my);
				zoom->h = zh;
			}
		}
	}
	zoom->x = NK_CLAMP(0, zoom->x, 1 - zoom->w);
	zoom->y = NK_CLAMP(0, zoom->y, 1 - zoom->h);
}

static bool
tv_is_trace_filtered(u16 trace)
{
	tv_filter *f = tv_global.filters;
	u16 flen = tv_global.filters_len;

	bool incl = false;
	bool excl = false;
	while (flen--) {
		switch (f->type) {
		case TV_FILTER_EQUAL:
			incl |= trace == f->bounds[0];
			break;
		case TV_FILTER_EQUAL | TV_FILTER_INVERT:
			excl |= trace == f->bounds[0];
			break;
		case TV_FILTER_RANGE:
			incl |= f->bounds[0] <= trace && trace < f->bounds[1];
			break;
		case TV_FILTER_RANGE | TV_FILTER_INVERT:
			excl |= f->bounds[0] <= trace && trace < f->bounds[1];
			break;
		default:
			TV_ASSERT(0);
		}
		++f;
	}
	return !incl || excl;
}

static bool
tv_is_trace_highlighted(u16 trace)
{
	struct tv_highlight *h = tv_global.highlights;
	u16 hlen = tv_global.highlights_len;

	while (hlen--)
		if (h++->trace == trace)
			return true;
	return false;
}

static void
tv_draw_rules(struct nk_context *ctx, struct nk_rect bounds, struct nk_color bg, struct nk_color fg)
{
	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
	struct nk_user_font const *font = ctx->style.font;

	char const s0[] = "FILTER RULES";
	char const s1[] = "Trace";
	char const s2[] = "a";
	char const s3[] = "Range";
	char const s4[] = "b-c";
	char const s5[] = "Multiple";
	char const s6[] = "a,b-c";
	char const s7[] = "Exclude";
	char const s8[] = "!a,!b-c";

	int tab = 96;
	float fh = font->height;
	float w = tab + font->width(font->userdata, fh, s8, sizeof s8 - 1);
	float h = 5.5f * fh + 16;
	if (bounds.w < w + 8 || bounds.h < h + 8)
		return;

	float x0 = bounds.x + (bounds.w - w) / 2;
	float y0 = bounds.y + (bounds.h - h) / 2;

	float w0 = font->width(font->userdata, fh, s0, sizeof s0 - 1);
	struct nk_rect r0 = { x0 + (w - w0) / 2, y0, w, fh };
	nk_draw_text(canvas, r0, s0, sizeof s0 - 1, font, bg, fg);
	struct nk_rect r1 = { x0, y0 + 1.5f * fh + 4, w, fh };
	nk_draw_text(canvas, r1, s1, sizeof s1 - 1, font, bg, fg);
	struct nk_rect r2 = { x0 + tab, y0 + 1.5f * fh + 4, w, fh };
	nk_draw_text(canvas, r2, s2, sizeof s2 - 1, font, bg, fg);
	struct nk_rect r3 = { x0, y0 + 2.5f * fh + 8, w, fh };
	nk_draw_text(canvas, r3, s3, sizeof s3 - 1, font, bg, fg);
	struct nk_rect r4 = { x0 + tab, y0 + 2.5f * fh + 8, w, fh };
	nk_draw_text(canvas, r4, s4, sizeof s4 - 1, font, bg, fg);
	struct nk_rect r5 = { x0, y0 + 3.5f * fh + 12, w, fh };
	nk_draw_text(canvas, r5, s5, sizeof s5 - 1, font, bg, fg);
	struct nk_rect r6 = { x0 + tab, y0 + 3.5f * fh + 12, w, fh };
	nk_draw_text(canvas, r6, s6, sizeof s6 - 1, font, bg, fg);
	struct nk_rect r7 = { x0, y0 + 4.5f * fh + 16, w, fh };
	nk_draw_text(canvas, r7, s7, sizeof s7 - 1, font, bg, fg);
	struct nk_rect r8 = { x0 + tab, y0 + 4.5f * fh + 16, w, fh };
	nk_draw_text(canvas, r8, s8, sizeof s8 - 1, font, bg, fg);
}

void
tv_draw_traces(float x0, float x1, nk_bool colorize)
{
	u16 traces = tv_global.traces;
	u32 points = tv_global.points;
	s8 *d = tv_global.data;
	struct tv_highlight *h = tv_global.highlights;
	u16 hlen = tv_global.highlights_len;

	TV_ASSERT(0 <= (u32)x0 && (u32)x0 < points);
	TV_ASSERT(0 <= (u32)x1 && (u32)x1 < points);
	TV_ASSERT(x0 < x1);
	u32 off0 = (u32)x0;
	u32 off1 = (u32)x1;

	u32 prng[4] = { 0 };
	tv_prng_init(prng, 0xcafebabe);

	sgl_begin_lines();
	for (u16 t = 0; t < traces; ++t) {
		if (tv_is_trace_filtered(t) || tv_is_trace_highlighted(t))
			continue;
		if (colorize) {
			struct nk_color c = nk_rgba_u32(tv_prng_squeeze(prng) | TV_COLOR_MASK);
			sgl_c3b(c.r, c.g, c.b);
		}
		s8 *p = d + t * points + off0;
		u32 x = off0;
		sgl_v2f(x, *p++);
		while (x++ < off1) {
			sgl_v2f(x, *p);
			sgl_v2f(x, *p++);
		}
		sgl_v2f(x, *p);
	}
	while (hlen--) {
		u16 t = h->trace;
		struct nk_color c = h++->color;
		if (tv_is_trace_filtered(t))
			continue;
		sgl_c3b(c.r, c.g, c.b);
		s8 *p = d + t * points + off0;
		u32 x = off0;
		sgl_v2f(x, *p++);
		while (x++ < off1) {
			sgl_v2f(x, *p);
			sgl_v2f(x, *p++);
		}
		sgl_v2f(x, *p);
	}
	sgl_end();
}

void
tv_draw_lttb(s8 const *data, u32 points, u32 buckets)
{
	TV_ASSERT(buckets > 2 && points > 2);
	buckets -= 2;
	points -= 2;

	float points_per_bucket = (float)points / buckets;
	float ppb_frac = points_per_bucket;

	u32 x0 = 0;
	s8 y0 = *data++;
	sgl_v2f(x0, y0);

	u32 p = 1;
	u32 dx0 = 1;
	while (--buckets) {
		u32 ppb = (u32)ppb_frac;
		ppb_frac += points_per_bucket - ppb;
		u32 ppb_next = (u32)ppb_frac;
		s8 const *next = data + ppb;

		float avg = 0;
		u32 i = ppb_next;
		while (i--)
			avg += *next++;
		avg /= ppb_next;

		float dx = dx0 + ppb + (ppb_next - 1) / 2.0f;
		float dy = avg - y0;
		float area_max = 0;
		u32 xmax = 0;
		s8 ymax = 0;
		for (u32 x = 0; x < ppb; ++x) {
			s8 y = *data++;
			float area = tv_absf(.5f * (dx * (y - y0) - dy * (x - x0)));
			if (area > area_max) {
				area_max = area;
				xmax = x;
				ymax = y;
			}
		}
		if (area_max > 0) {
			sgl_v2f(p + xmax, ymax);
			sgl_v2f(p + xmax, ymax);
		}

		p += ppb;
		x0 = xmax;
		y0 = ymax;
		dx0 = ppb - x0;
	}

	u32 ppb = points - p;
	s8 ylast = data[ppb];
	float dx = dx0 + ppb;
	float dy = ylast - y0;
	float area_max = 0;
	u32 xmax = 0;
	s8 ymax = 0;
	for (u32 x = 0; x < ppb; ++x) {
		s8 y = *data++;
		float area = tv_absf(.5f * (dx * (y - y0) - dy * (x - x0)));
		if (area > area_max) {
			area_max = area;
			xmax = x;
			ymax = y;
		}
	}
	if (area_max > 0) {
		sgl_v2f(p + xmax, ymax);
		sgl_v2f(p + xmax, ymax);
	}
	sgl_v2f(points - 1, ylast);
}

void
tv_draw_traces_lttb(u32 buckets, nk_bool colorize)
{
	u16 traces = tv_global.traces;
	u32 points = tv_global.points;
	s8 *d = tv_global.data;
	struct tv_highlight *h = tv_global.highlights;
	u16 hlen = tv_global.highlights_len;

	u32 prng[4] = { 0 };
	tv_prng_init(prng, 0xcafebabe);

	sgl_begin_lines();
	for (u16 t = 0; t < traces; ++t) {
		if (tv_is_trace_filtered(t) || tv_is_trace_highlighted(t))
			continue;
		if (colorize) {
			struct nk_color c = nk_rgba_u32(tv_prng_squeeze(prng) | TV_COLOR_MASK);
			sgl_c3b(c.r, c.g, c.b);
		}
		tv_draw_lttb(d + t * points, points, buckets);
	}
	while (hlen--) {
		u16 t = h->trace;
		struct nk_color c = h++->color;
		if (tv_is_trace_filtered(t))
			continue;
		sgl_c3b(c.r, c.g, c.b);
		tv_draw_lttb(d + t * points, points, buckets);
	}
	sgl_end();
}

static void
tv_sapp_frame(void)
{
	static struct nk_rect zoom = { 0, 0, 1, 1 };
	static u32 buckets = 0;
	static nk_bool colorize = nk_true;
	static nk_bool lttb = nk_false;

	double fps = 1 / sapp_frame_duration();
	int height = sapp_height();
	int width = sapp_width();
	int img_w = width - 181;
	int img_h = height - 108;
	sg_image img_sg = sg_make_image(&(sg_image_desc){
	    .width = NK_MAX(1, img_w),
	    .height = NK_MAX(1, img_h),
	    .usage.color_attachment = true,
	});
	sg_view img_att = sg_make_view(&(sg_view_desc){
	    .color_attachment = { .image = img_sg },
	});
	sg_view img_tex = sg_make_view(&(sg_view_desc){
	    .texture = { .image = img_sg },
	});
	snk_image_t img_snk = snk_make_image(&(snk_image_desc_t){
	    .sampler = tv_global.sampler,
	    .texture_view = img_tex,
	});

	struct nk_context *ctx = snk_new_frame();
	struct nk_input *in = &ctx->input;

	struct nk_color c_wb = ctx->style.window.border_color;
	struct nk_color c_bg = ctx->style.edit.normal.data.color;
	struct nk_color c_fg = ctx->style.text.color;
	struct nk_color c_hl = nk_rgb_hex(TV_COLOR_HIGHLIGHT);
	struct nk_color c_red = nk_rgb_hex(TV_COLOR_RED);
	ctx->style.button.border_color = c_hl;
	ctx->style.button.hover = nk_style_item_color(c_hl);
	ctx->style.button.active = nk_style_item_color(c_hl);
	ctx->style.button.text_hover = ctx->style.window.background;
	ctx->style.button.text_active = ctx->style.window.background;
	ctx->style.checkbox.cursor_normal = nk_style_item_color(c_hl);
	ctx->style.checkbox.cursor_hover = nk_style_item_color(c_hl);
	ctx->style.cursor_visible = nk_false;
	ctx->style.window.group_padding = (struct nk_vec2){ 0, 0 };
	ctx->style.window.scrollbar_size = (struct nk_vec2){ 0, 0 };

	struct nk_style_button sb_red = ctx->style.button;
	sb_red.border_color = c_red;
	sb_red.hover = nk_style_item_color(c_red);
	sb_red.active = nk_style_item_color(c_red);
	sb_red.text_hover = ctx->style.window.background;
	sb_red.text_active = ctx->style.window.background;

	struct nk_style_button sb_inactive = ctx->style.button;
	sb_inactive.border_color = c_wb;
	sb_inactive.text_normal = c_bg;
	sb_inactive.hover = sb_inactive.normal;
	sb_inactive.active = sb_inactive.normal;
	sb_inactive.text_hover = sb_inactive.text_normal;
	sb_inactive.text_active = sb_inactive.text_normal;

	u32 traces = tv_global.traces;
	u32 points = tv_global.points;
	u16 hlen = tv_global.highlights_len;
	if (nk_begin(ctx, "tv", nk_rect(0, 0, width, height), NK_WINDOW_NO_SCROLLBAR)) {
		struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
		struct nk_user_font const *font = ctx->style.font;

		nk_layout_row_dynamic(ctx, 32, 2);
		nk_labelf(ctx, NK_TEXT_LEFT, "traceview: %s", tv_global.filepath);
		nk_labelf(ctx, NK_TEXT_RIGHT, "%.0lffps", fps);

		nk_layout_row(ctx, NK_STATIC, height - 44, 3, (float[]){ 132, 1, width - 149 });
		if (nk_group_begin(ctx, "column0", 0)) {
			nk_layout_row_static(ctx, 12, 64, 2);
			nk_label(ctx, "Traces:", NK_TEXT_LEFT);
			nk_labelf(ctx, NK_TEXT_RIGHT, "%" PRIu16, traces);
			nk_label(ctx, "Points:", NK_TEXT_LEFT);
			nk_labelf(ctx, NK_TEXT_RIGHT, "%" PRIu32, points);
			nk_layout_row_static(ctx, 8, 0, 0);

			nk_layout_row_static(ctx, 14, 132, 1);
			nk_checkbox_label_align(ctx, "LTTB:", &lttb, NK_WIDGET_RIGHT, NK_TEXT_LEFT);

			nk_style_push_float(ctx, &ctx->style.window.spacing.y, 0);
			nk_layout_row(ctx, NK_STATIC, 16, 2, (float[]){ 86, 42 });
			struct nk_vec2 v = nk_widget_position(ctx);
			nk_label(ctx, "LTTB Buckets:", NK_TEXT_LEFT);
			nk_labelf(ctx, NK_TEXT_RIGHT, "%" PRIu32, buckets);
			nk_style_pop_float(ctx);
			{
				buckets = (u32)NK_MAX(3, img_w);
				static bool bucketing = false;
				static char buf[11] = { 0 };
				nk_flags state = 0;
				struct nk_rect r = { v.x, v.y, 132, 16 };
				if (nk_button_behavior(&state, r, in, NK_BUTTON_DEFAULT))
					bucketing = !bucketing;
				if (bucketing) {
					nk_layout_row_static(ctx, 24, 132, 1);
					nk_edit_string_zero_terminated(
					    ctx, NK_EDIT_FIELD, buf, 11, tv_nk_filter_uint
					);
				}
				if (*buf != 0) {
					long b = strtol(buf, 0, 0);
					if (3 <= b && b <= points)
						buckets = (u32)b;
					else {
						nk_layout_row_static(ctx, 12, 132, 1);
						nk_label_colored(
						    ctx, "invalid bucket size", NK_TEXT_LEFT, c_red
						);
					}
				}
			}

			nk_layout_row(ctx, NK_STATIC, 16, 2, (float[]){ 86, 42 });
			nk_label(ctx, "LTTB PPB:", NK_TEXT_LEFT);
			nk_labelf(ctx, NK_TEXT_RIGHT, "%.2f", (float)points / buckets);
			nk_layout_row_static(ctx, 8, 0, 0);

			nk_layout_row_static(ctx, 14, 132, 1);
			nk_checkbox_label_align(
			    ctx, "Colorize:", &colorize, NK_WIDGET_RIGHT, NK_TEXT_LEFT
			);

			nk_layout_row_static(ctx, 14, 132, 1);
			nk_label(ctx, "Highlights:", NK_TEXT_LEFT);
			{
				tv_highlight *h = tv_global.highlights;
				tv_color_picker *hp = tv_global.highlight_pickers;
				u16 len = hlen;
				while (len--) {
					struct nk_color c = h->color;
					char picking = hp->picking;

					nk_style_push_float(ctx, &ctx->style.window.spacing.x, 0);
					nk_layout_row(
					    ctx, NK_STATIC, 12, 4, (float[]){ 18, 4, 85, 24, 1 }
					);
					struct nk_vec2 pad = { 3, -1 };
					if (tv_button_color_picker(ctx, c, pad))
						hp->picking = picking = !picking;
					nk_spacing(ctx, 1);
					nk_labelf(ctx, NK_TEXT_LEFT, "%" PRIu16, h->trace);
					nk_bool delete = nk_button_label_styled(ctx, &sb_red, "-");
					nk_style_pop_float(ctx);

					if (delete) {
						while (len--) {
							h[0] = h[1];
							++h;
							hp[0] = hp[1];
							++hp;
						}
						*h = (tv_highlight){ 0 };
						*hp = (tv_color_picker){ 0 };
						--tv_global.highlights_len;
						goto column0_end;
					}

					if (picking)
						h->color = tv_color_pick(ctx, hp->buf, &hp->len);
					else {
						nk_color_hex_rgb(hp->buf, c);
						hp->len = 6;
					}

					++h;
					++hp;
				}
			}

			static struct nk_color c = { 0 };
			static bool adding = 0;
			if (adding) {
				static tv_color_picker cp = { 0 };
				static char buf[6] = { 0 };

				nk_layout_row(ctx, NK_STATIC, 24, 2, (float[]){ 18, 108 });
				if (tv_button_color_picker(ctx, c, (struct nk_vec2){ 3, 6 }))
					cp.picking = !cp.picking;
				nk_edit_string_zero_terminated(
				    ctx, NK_EDIT_FIELD, buf, 6, tv_nk_filter_uint
				);

				long t = strtol(buf, 0, 0);
				bool error = t >= traces;
				if (*buf != 0) {
					tv_highlight *h = tv_global.highlights;
					u16 len = hlen;
					while (len--)
						error = error || h++->trace == t;
					if (error) {
						nk_layout_row_static(ctx, 12, 132, 1);
						nk_label_colored(
						    ctx, "invalid trace ID", NK_TEXT_LEFT, c_red
						);
					}
				}

				if (cp.picking)
					c = tv_color_pick(ctx, cp.buf, &cp.len);
				else {
					nk_color_hex_rgb(cp.buf, c);
					cp.len = 6;
				}

				nk_style_push_float(ctx, &ctx->style.window.spacing.x, 0);
				nk_layout_row(ctx, NK_STATIC, 16, 5, (float[]){ 1, 63, 4, 63, 1 });
				nk_spacing(ctx, 1);
				if (*buf == 0 || error)
					nk_button_label_styled(ctx, &sb_inactive, "Save");
				else if (nk_button_label(ctx, "Save")) {
					tv_highlight *h = tv_global.highlights;
					tv_highlight *p = h + hlen;
					while (p-- != h && p->trace > t)
						p[1] = p[0];
					p[1].color = c;
					p[1].trace = (u16)t;
					++tv_global.highlights_len;
					adding = cp.picking = *buf = 0;
				}
				nk_spacing(ctx, 1);
				if (nk_button_label(ctx, "Cancel"))
					adding = cp.picking = *buf = 0;
				nk_spacing(ctx, 1);
				nk_style_pop_float(ctx);
			} else {
				u32 r = tv_prng_squeeze(tv_global.prng);
				c = hlen == 0 ? c_hl : nk_rgba_u32(r | TV_COLOR_MASK);

				nk_style_push_float(ctx, &ctx->style.window.spacing.x, 0);
				nk_layout_row(ctx, NK_STATIC, 16, 5, (float[]){ 1, 63, 4, 63, 1 });
				nk_spacing(ctx, 1);
				if (hlen == TV_HIGHLIGHT_MAX)
					nk_button_label_styled(ctx, &sb_inactive, "Add");
				else if (nk_button_label(ctx, "Add"))
					adding = 1;
				nk_spacing(ctx, 3);
				nk_style_pop_float(ctx);
			}
		column0_end:
			nk_group_end(ctx);
		}
		nk_fill_rect(canvas, nk_widget_bounds(ctx), 0, c_wb);
		nk_spacing(ctx, 1);
		if (nk_group_begin(ctx, "column1", NK_WINDOW_NO_SCROLLBAR)) {
			static char s[TV_FILTER_MAX] = { 0 };

			nk_layout_row(ctx, NK_STATIC, 28, 3, (float[]){ 48, img_w - 52, 28 });
			nk_label(ctx, "Filter:", NK_TEXT_LEFT);
			nk_edit_string_zero_terminated(
			    ctx, NK_EDIT_FIELD, s, TV_FILTER_MAX, tv_nk_filter_filter
			);

			nk_style_push_color(ctx, &ctx->style.button.border_color, c_wb);
			nk_style_push_color(ctx, &ctx->style.button.text_normal, c_hl);
			if (zoom.x == 0 && zoom.y == 0 && zoom.w == 1 && zoom.h == 1)
				nk_button_symbol_styled(ctx, &sb_inactive, NK_SYMBOL_RECT_OUTLINE);
			else if (nk_button_symbol(ctx, NK_SYMBOL_RECT_OUTLINE))
				zoom = (struct nk_rect){ 0, 0, 1, 1 };
			nk_style_pop_color(ctx);
			nk_style_pop_color(ctx);

			nk_layout_row(ctx, NK_STATIC, img_h, 1, (float[]){ img_w, 28 });
			struct nk_rect bounds = nk_widget_bounds(ctx);
			nk_image(ctx, nk_image_handle(snk_nkhandle(img_snk)));
			nk_stroke_rect(canvas, bounds, 0, 1, c_wb);

			if (!tv_filters_parse(s))
				tv_draw_rules(ctx, bounds, c_bg, c_fg);
			else if (bounds.w > 96 && bounds.h > 96) {
				struct nk_vec2 min = { 8.5f / points, 8.5f / 272 };
				tv_zoom(in, &zoom, bounds, min);

				u32 w_max = points - 1;
				float x0 = zoom.x * w_max;
				float x1 = x0 + zoom.w * w_max;
				float y0 = zoom.y * 272 - 136;
				float y1 = y0 + zoom.h * 272;

				sgl_viewportf(0, 0, bounds.w, bounds.h, true);
				sgl_matrix_mode_projection();
				sgl_load_identity();
				sgl_ortho(x0, x1, y0, y1, -1, 1);
				sgl_matrix_mode_modelview();
				sgl_load_identity();

				sgl_c3b(c_wb.r, c_wb.g, c_wb.b);
				int logdy = 31 - __builtin_clz((u32)(zoom.h * 272));
				int ys = logdy < 4 ? 4 : 4 << (logdy - 4);
				float fx0 = bounds.x + bounds.w + 4;
				float fh = font->height;
				sgl_begin_lines();
				for (int y = -128; y <= 128; y += ys) {
					sgl_v2f(x0, y);
					sgl_v2f(x1, y);

					float fy_min = bounds.y + fh / 2;
					float fy_max = bounds.y + bounds.h - fh / 2;
					float fy = bounds.y + (y1 - y) / (y1 - y0) * bounds.h;
					if (fy < fy_min || fy > fy_max)
						continue;
					char buf[5] = { 0 };
					int len = snprintf(buf, 5, "%d", y);
					struct nk_rect r = { fx0, fy - fh / 2, 28, fh };
					nk_draw_text(canvas, r, buf, len, font, c_bg, c_fg);
				}
				sgl_end();

				sgl_c3b(c_fg.r, c_fg.g, c_fg.b);
				if (lttb)
					tv_draw_traces_lttb(buckets, colorize);
				else
					tv_draw_traces(x0, x1, colorize);

				float w = (bounds.w - 8) / 3.0f;
				nk_layout_row(ctx, NK_STATIC, 12, 4, (float[]){ w, w, w, 28 });
				nk_labelf(ctx, NK_TEXT_LEFT, "%.0f", x0);
				if (nk_input_is_mouse_hovering_rect(in, bounds)) {
					float mx = (in->mouse.pos.x - bounds.x) / bounds.w;
					float my = (in->mouse.pos.y - bounds.y) / bounds.h;
					float x = x0 + mx * zoom.w * w_max;
					float y = y0 + (1 - my) * zoom.h * 272;
					nk_labelf(ctx, NK_TEXT_CENTERED, "(%.0f, %.0f)", x, y);
				} else
					nk_spacing(ctx, 1);
				nk_labelf(ctx, NK_TEXT_RIGHT, "%.0f", x1);
			}
		}
	}
	nk_end(ctx);

	struct nk_colorf cf = nk_color_cf(c_bg);
	sg_begin_pass(&(sg_pass){
	    .action.colors[0] = {
	        .load_action = SG_LOADACTION_CLEAR,
	        .clear_value = { cf.r, cf.g, cf.b, 1 },
	    },
	    .attachments = { .colors[0] = img_att },
	});
	sgl_draw();
	sg_end_pass();

	sg_begin_pass(&(sg_pass){ .swapchain = sglue_swapchain() });
	snk_render(width, height);
	sg_end_pass();
	sg_commit();

	sg_destroy_image(img_sg);
	sg_destroy_view(img_att);
	sg_destroy_view(img_tex);
	snk_destroy_image(img_snk);
}

sapp_desc
sokol_main(int argc, char *argv[])
{
	if ((argc == 2 || argc == 3) && strcmp(argv[1], "-h") == 0) {
		fputs("usage: tv [-h] <trace.bin>\n", stderr);
		exit(0);
	}
	if (argc != 2) {
		fputs("usage: tv [-h] <trace.bin>\n", stderr);
		exit(1);
	}
	tv_global.filepath = argv[1];

	return (sapp_desc){
		.window_title = "tv",
		.init_cb = tv_sapp_init,
		.frame_cb = tv_sapp_frame,
		.cleanup_cb = tv_sapp_cleanup,
		.event_cb = tv_sapp_event,
		.logger.func = slog_func,
	};
}
