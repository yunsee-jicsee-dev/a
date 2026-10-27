/*
 * GATE16 board - graphical, mouse-driven logic circuit editor.
 *
 * Draws straight to the display through KMS (/dev/dri/card0) and reads the mouse
 * from evdev (/dev/input/event*), so it needs no X server or libraries.
 * Keyboard input still comes from the tty (see gate16.c).
 *
 * Parts are dragged from the palette onto the board, wired by dragging from
 * pin to pin, and gates can have 2..8 inputs. Up to 16 logic gates, 8 input
 * switches, 8 LEDs, clocks and constant-1 sources fit on one board.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "font8x16.h"
#include "gate16.h"

/* ------------------------------------------------------------------ model */

enum { C_NONE, C_IN, C_OUT, C_CLK, C_HIGH, C_BUF, C_NOT, C_AND, C_OR, C_NAND, C_NOR, C_XOR, C_XNOR, C_KINDS };
static const char *KNAME[C_KINDS] = {"", "SWITCH", "LED", "CLOCK", "HIGH", "BUF", "NOT", "AND", "OR",
				     "NAND", "NOR", "XOR", "XNOR"};

#define MAXPIN 8
#define MAXCOMP 48
#define MAXGATES 16
#define MAXIN 8
#define MAXOUT 8
#define MAXCLK 2
#define MAXHIGH 4
#define MAXITER 64

struct comp {
	int kind, x, y, nin;
	int src[MAXPIN]; /* driving component index + 1, 0 = unconnected */
	int val;
	char name[8];
};

struct board {
	char title[24];
	struct comp c[MAXCOMP];
};

static struct board B;
static int sim_stable = 1;

static int is_gate(int k) { return k >= C_BUF; }
static int inverted(int k) { return k == C_NOT || k == C_NAND || k == C_NOR || k == C_XNOR; }
static int has_output(int k) { return k != C_NONE && k != C_OUT; }
static int min_in(int k) { return k == C_OUT || k == C_BUF || k == C_NOT ? 1 : is_gate(k) ? 2 : 0; }
static int max_in(int k) { return is_gate(k) && k != C_BUF && k != C_NOT ? MAXPIN : min_in(k); }

static int limit_of(int k)
{
	switch (k) {
	case C_IN: return MAXIN;
	case C_OUT: return MAXOUT;
	case C_CLK: return MAXCLK;
	case C_HIGH: return MAXHIGH;
	}
	return MAXGATES;
}

/* number of parts in the same limit group as kind k */
static int count_like(int k)
{
	int n = 0;
	for (int i = 0; i < MAXCOMP; i++) {
		int ki = B.c[i].kind;
		if (ki != C_NONE && (ki == k || (is_gate(k) && is_gate(ki)))) n++;
	}
	return n;
}

static int src_val(int s) { return s ? B.c[s - 1].val : 0; }

static int eval(const struct comp *c)
{
	int ones = 0;
	for (int i = 0; i < c->nin; i++) ones += src_val(c->src[i]);
	switch (c->kind) {
	case C_OUT: case C_BUF: return ones > 0;
	case C_NOT: return ones == 0;
	case C_AND: return ones == c->nin;
	case C_OR: return ones > 0;
	case C_NAND: return ones != c->nin;
	case C_NOR: return ones == 0;
	case C_XOR: return ones & 1;
	case C_XNOR: return !(ones & 1);
	case C_HIGH: return 1;
	}
	return c->val; /* switches and clocks hold their own value */
}

/* In-place relaxation (Gauss-Seidel) so cross-coupled latches settle. */
static void simulate(void)
{
	for (int it = 0; it < MAXITER; it++) {
		int changed = 0;
		for (int i = 0; i < MAXCOMP; i++) {
			struct comp *c = &B.c[i];
			if (c->kind == C_NONE) continue;
			int v = eval(c);
			if (v != c->val) {
				c->val = v;
				changed = 1;
			}
		}
		if (!changed) {
			sim_stable = 1;
			return;
		}
	}
	sim_stable = 0;
}

static void default_name(int k, char buf[8])
{
	for (int n = 0; n < 64; n++) {
		if (k == C_IN) snprintf(buf, 8, "%c", 'A' + n);
		else if (k == C_OUT) snprintf(buf, 8, "Y%d", n);
		else if (k == C_CLK) snprintf(buf, 8, n ? "CLK%d" : "CLK", n);
		else if (k == C_HIGH) snprintf(buf, 8, "VCC%d", n);
		else snprintf(buf, 8, "G%X", n);
		int used = 0;
		for (int i = 0; i < MAXCOMP; i++)
			if (B.c[i].kind != C_NONE && !strcmp(B.c[i].name, buf)) used = 1;
		if (!used) return;
	}
}

#define GRID 16
static int snap(int v) { return (int)floor((double)v / GRID + 0.5) * GRID; }

static const char *add_error;

/* Adds a part with its top-left corner at (x, y); returns its index or -1. */
static int add_comp(int k, int x, int y)
{
	if (count_like(k) >= limit_of(k)) {
		add_error = is_gate(k) ? "All 16 gate slots are used" : "No more parts of this kind allowed";
		return -1;
	}
	for (int i = 0; i < MAXCOMP; i++) {
		struct comp *c = &B.c[i];
		if (c->kind != C_NONE) continue;
		memset(c, 0, sizeof *c);
		default_name(k, c->name); /* before setting kind, so it doesn't see itself */
		c->kind = k;
		c->x = snap(x);
		c->y = snap(y);
		c->nin = min_in(k);
		return i;
	}
	add_error = "Board is full";
	return -1;
}

static void del_comp(int i)
{
	B.c[i].kind = C_NONE;
	for (int j = 0; j < MAXCOMP; j++)
		for (int p = 0; p < MAXPIN; p++)
			if (B.c[j].src[p] == i + 1) B.c[j].src[p] = 0;
}

static int set_pins(int i, int n)
{
	struct comp *c = &B.c[i];
	if (n < min_in(c->kind) || n > max_in(c->kind) || n == c->nin) return 0;
	for (int p = n; p < MAXPIN; p++) c->src[p] = 0;
	c->nin = n;
	return 1;
}

static void clear_board(const char *title)
{
	memset(&B, 0, sizeof B);
	snprintf(B.title, sizeof B.title, "%s", title);
}

/* ---------------------------------------------------------------- geometry */

#define BW 48   /* gate body width */
#define STUB 14 /* pin stub length */

static int comp_w(const struct comp *c)
{
	switch (c->kind) {
	case C_IN: case C_CLK: return 48;
	case C_OUT: return 32;
	case C_HIGH: return 32;
	}
	return (c->kind == C_XOR || c->kind == C_XNOR ? 6 : 0) + BW + (inverted(c->kind) ? 8 : 0);
}

static int comp_h(const struct comp *c)
{
	if (is_gate(c->kind)) return GRID * (c->nin + 1);
	return 32;
}

static void in_pin(const struct comp *c, int p, int *x, int *y)
{
	*x = c->x - STUB;
	*y = c->kind == C_OUT ? c->y + 16 : c->y + GRID * (p + 1);
}

static void out_pin(const struct comp *c, int *x, int *y)
{
	*x = c->x + comp_w(c) + STUB;
	*y = c->y + comp_h(c) / 2;
}

/* Orthogonal route from an output pin to an input pin; returns point count. */
static int route(int x1, int y1, int x2, int y2, int *px, int *py)
{
	if (x2 - x1 >= 2 * GRID) {
		int mx = snap((x1 + x2) / 2);
		int xs[] = {x1, mx, mx, x2}, ys[] = {y1, y1, y2, y2};
		memcpy(px, xs, sizeof xs);
		memcpy(py, ys, sizeof ys);
		return 4;
	}
	/* going backwards: loop around below/above */
	int ym = (y1 + y2) / 2;
	if (abs(y1 - y2) < 2 * GRID) ym = (y1 > y2 ? y1 : y2) + 3 * GRID;
	int xs[] = {x1, x1 + GRID, x1 + GRID, x2 - GRID, x2 - GRID, x2};
	int ys[] = {y1, y1, ym, ym, y2, y2};
	memcpy(px, xs, sizeof xs);
	memcpy(py, ys, sizeof ys);
	return 6;
}

static int wire_route(int dst, int p, int *px, int *py)
{
	const struct comp *c = &B.c[dst];
	int x1, y1, x2, y2;
	out_pin(&B.c[c->src[p] - 1], &x1, &y1);
	in_pin(c, p, &x2, &y2);
	return route(x1, y1, x2, y2, px, py);
}

/* ------------------------------------------------------------ framebuffer */

static int SCALE = 1;   /* logical pixel -> SCALE x SCALE physical pixels */
static int W, H;        /* logical screen size */
static uint32_t *bb;    /* logical back buffer, 0xRRGGBB */
static int clip_x0, clip_y0, clip_x1, clip_y1;

static void clip_all(void) { clip_x0 = clip_y0 = 0, clip_x1 = W, clip_y1 = H; }
static void clip_set(int x, int y, int w, int h) { clip_x0 = x, clip_y0 = y, clip_x1 = x + w, clip_y1 = y + h; }

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
	int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
	int x1 = x + w > clip_x1 ? clip_x1 : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++) bb[j * W + i] = c;
}

static void pset(int x, int y, uint32_t c)
{
	if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) bb[y * W + x] = c;
}

static void hline(int x0, int x1, int y, uint32_t c)
{
	if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
	fill_rect(x0, y, x1 - x0 + 1, 1, c);
}

static void rect_outline(int x, int y, int w, int h, uint32_t c)
{
	fill_rect(x, y, w, 1, c);
	fill_rect(x, y + h - 1, w, 1, c);
	fill_rect(x, y, 1, h, c);
	fill_rect(x + w - 1, y, 1, h, c);
}

static void round_rect(int x, int y, int w, int h, int r, uint32_t c)
{
	for (int j = 0; j < h; j++) {
		int in = 0;
		if (j < r) in = r - (int)sqrt((double)r * r - (r - j - 0.5) * (r - j - 0.5));
		else if (j >= h - r) in = r - (int)sqrt((double)r * r - (j - (h - r) + 0.5) * (j - (h - r) + 0.5));
		fill_rect(x + in, y + j, w - 2 * in, 1, c);
	}
}

static void circle(int cx, int cy, int r, uint32_t c)
{
	for (int j = -r; j <= r; j++) {
		int dx = (int)sqrt((double)r * r - j * j + 0.3);
		hline(cx - dx, cx + dx, cy + j, c);
	}
}

/* thick axis-aligned segment */
static void seg(int x1, int y1, int x2, int y2, int t, uint32_t c)
{
	int xa = x1 < x2 ? x1 : x2, xb = x1 < x2 ? x2 : x1;
	int ya = y1 < y2 ? y1 : y2, yb = y1 < y2 ? y2 : y1;
	fill_rect(xa - t / 2, ya - t / 2, xb - xa + t, yb - ya + t, c);
}

static uint32_t mix(uint32_t a, uint32_t b, int t) /* t/256 of b */
{
	uint32_t r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
	uint32_t g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
	uint32_t bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
	return r << 16 | g << 8 | bl;
}

static int text(int x, int y, uint32_t c, const char *s)
{
	for (; *s; s++, x += 8) {
		unsigned ch = (unsigned char)*s;
		if (ch < 32 || ch > 126) ch = '?';
		const unsigned char *g = FONT8X16[ch - 32];
		for (int j = 0; j < 16; j++)
			for (int i = 0; i < 8; i++)
				if (g[j] & (0x80 >> i)) pset(x + i, y + j, c);
	}
	return x;
}

static int textf(int x, int y, uint32_t c, const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return text(x, y, c, buf);
}

static int text_w(const char *s) { return 8 * (int)strlen(s); }

/* The screen is driven through KMS: one XRGB8888 dumb buffer scanned out
 * on the first connected display, flushed with DIRTYFB after each frame.
 * With simpledrm this works on any firmware framebuffer (UEFI GOP or VESA). */
static int drmfd = -1;
static uint32_t drm_fb, drm_crtc, drm_conn, drm_pitch, drm_w, drm_h;
static uint8_t *drm_map;

static void fb_restore(void)
{
	if (drmfd >= 0) {
		close(drmfd); /* the fbdev console restores itself on last close */
		drmfd = -1;
		ioctl(ttyfd, KDSETMODE, KD_TEXT);
	}
}

static void fatal_signal(int sig)
{
	fb_restore();
	tty_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

static int kms_pick(int fd, struct drm_mode_modeinfo *mode)
{
	struct drm_mode_card_res res = {0};
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors || !res.count_crtcs) return -1;
	uint32_t conns[16], crtcs[16], fbs[16], encs[16];
	if (res.count_connectors > 16) res.count_connectors = 16;
	if (res.count_crtcs > 16) res.count_crtcs = 16;
	if (res.count_fbs > 16) res.count_fbs = 16;
	if (res.count_encoders > 16) res.count_encoders = 16;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.fb_id_ptr = (uintptr_t)fbs;
	res.encoder_id_ptr = (uintptr_t)encs;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return -1;
	for (uint32_t i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector c = {.connector_id = conns[i]};
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connection != 1 || !c.count_modes) continue;
		struct drm_mode_modeinfo modes[64];
		uint32_t cencs[16];
		uint64_t props[64], vals[64];
		if (c.count_modes > 64) c.count_modes = 64;
		if (c.count_encoders > 16) c.count_encoders = 16;
		if (c.count_props > 64) c.count_props = 64;
		c.modes_ptr = (uintptr_t)modes;
		c.encoders_ptr = (uintptr_t)cencs;
		c.props_ptr = (uintptr_t)props;
		c.prop_values_ptr = (uintptr_t)vals;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || !c.count_modes) continue;
		*mode = modes[0];
		for (uint32_t m = 0; m < c.count_modes; m++)
			if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
				*mode = modes[m];
				break;
			}
		struct drm_mode_get_encoder e = {.encoder_id = c.encoder_id ? c.encoder_id : cencs[0]};
		if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e)) continue;
		drm_crtc = e.crtc_id;
		for (uint32_t k = 0; !drm_crtc && k < res.count_crtcs; k++)
			if (e.possible_crtcs & (1u << k)) drm_crtc = crtcs[k];
		if (!drm_crtc) continue;
		drm_conn = conns[i];
		return 0;
	}
	return -1;
}

static int fb_open(void)
{
	struct drm_mode_modeinfo mode;
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) return -1;
	ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
	if (kms_pick(fd, &mode) || mode.hdisplay < 640 || mode.vdisplay < 480) goto fail;
	struct drm_mode_create_dumb cd = {.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) goto fail;
	struct drm_mode_fb_cmd fc = {.width = cd.width, .height = cd.height, .pitch = cd.pitch,
				     .bpp = 32, .depth = 24, .handle = cd.handle};
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fc)) goto fail;
	struct drm_mode_map_dumb md = {.handle = cd.handle};
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) goto fail;
	drm_map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (drm_map == MAP_FAILED) goto fail;
	memset(drm_map, 0, cd.size);
	struct drm_mode_crtc crtc = {.crtc_id = drm_crtc, .fb_id = fc.fb_id, .set_connectors_ptr = (uintptr_t)&drm_conn,
				     .count_connectors = 1, .mode = mode, .mode_valid = 1};
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc)) goto fail;
	drmfd = fd;
	drm_fb = fc.fb_id;
	drm_pitch = cd.pitch;
	drm_w = cd.width;
	drm_h = cd.height;
	if (drm_w >= 3200 && drm_h >= 1800) SCALE = 3;
	else if (drm_w >= 1800 && drm_h >= 1100) SCALE = 2;
	W = drm_w / SCALE;
	H = drm_h / SCALE;
	bb = calloc((size_t)W * H, sizeof *bb);
	clip_all();
	return 0;
fail:
	close(fd);
	return -1;
}

static void fb_blit(void)
{
	for (int y = 0; y < H; y++) {
		uint32_t *row = (uint32_t *)(drm_map + (size_t)y * SCALE * drm_pitch);
		const uint32_t *src = bb + (size_t)y * W;
		if (SCALE == 1) {
			memcpy(row, src, (size_t)W * 4);
			continue;
		}
		for (int x = 0; x < W; x++)
			for (int s = 0; s < SCALE; s++) row[x * SCALE + s] = src[x];
		for (int s = 1; s < SCALE; s++) memcpy((uint8_t *)row + (size_t)s * drm_pitch, row, (size_t)W * SCALE * 4);
	}
	struct drm_mode_fb_dirty_cmd d = {.fb_id = drm_fb};
	ioctl(drmfd, DRM_IOCTL_MODE_DIRTYFB, &d);
}

/* ------------------------------------------------------------------ mouse */

struct mouse {
	int fd, absolute;
	struct input_absinfo ax, ay;
	char path[32];
};
static struct mouse mice[8];
static int nmice;
static double mx = 200, my = 200; /* cursor, logical pixels */

#define BIT(arr, b) ((arr[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

static void scan_mice(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	if (!d) return;
	while ((e = readdir(d)) && nmice < 8) {
		char path[32];
		if (strncmp(e->d_name, "event", 5)) continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		int known = 0;
		for (int i = 0; i < nmice; i++)
			if (!strcmp(mice[i].path, path)) known = 1;
		if (known) continue;
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		unsigned long ev[1] = {0}, keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
		unsigned long rel[1] = {0}, abs_[1] = {0};
		ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
		ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_), abs_);
		int button = BIT(keys, BTN_LEFT) || BIT(keys, BTN_TOUCH);
		struct mouse *m = &mice[nmice];
		if (button && BIT(ev, EV_REL) && BIT(rel, REL_X)) {
			m->absolute = 0;
		} else if (button && BIT(ev, EV_ABS) && BIT(abs_, ABS_X) &&
			   !ioctl(fd, EVIOCGABS(ABS_X), &m->ax) && !ioctl(fd, EVIOCGABS(ABS_Y), &m->ay) &&
			   m->ax.maximum > m->ax.minimum && m->ay.maximum > m->ay.minimum) {
			m->absolute = 1;
		} else {
			close(fd);
			continue;
		}
		m->fd = fd;
		snprintf(m->path, sizeof m->path, "%s", path);
		nmice++;
	}
	closedir(d);
}

/* ---------------------------------------------------------------- UI state */

#define TOP_H 34
#define STATUS_H 24
#define PAL_W 132

enum { M_EDIT, M_PRESETS, M_TRUTH, M_HELP, M_MENU, M_RENAME };
enum { D_NONE, D_NEW, D_MOVE, D_WIRE_OUT, D_WIRE_IN, D_PAN };

static int mode = M_EDIT;
static int drag = D_NONE, drag_kind, drag_comp, drag_pin, drag_moved;
static int drag_dx, drag_dy, press_x, press_y, pan_start_x, pan_start_y;
static int pan_x = 0, pan_y = 0;
static int sel = -1;                 /* selected component */
static int sel_wire = -1, sel_wire_pin; /* selected wire: destination comp + pin */
static int truth_top;
static char rename_buf[8];
static char status_msg[120];
static double status_until;
static int exit_code = -1;

static const double CLK_HZ[] = {0.5, 1, 2, 4, 8};
#define NCLK ((int)(sizeof CLK_HZ / sizeof CLK_HZ[0]))
static int clk_rate = 1, clk_run;

static int canvas_x(void) { return PAL_W; }
static int canvas_y(void) { return TOP_H; }
static int canvas_w(void) { return W - PAL_W; }
static int canvas_h(void) { return H - TOP_H - STATUS_H; }
static int to_sx(int wx) { return wx + pan_x + canvas_x(); }
static int to_sy(int wy) { return wy + pan_y + canvas_y(); }
static int to_wx(int sx) { return sx - canvas_x() - pan_x; }
static int to_wy(int sy) { return sy - canvas_y() - pan_y; }
static int in_canvas(int sx, int sy)
{
	return sx >= canvas_x() && sy >= canvas_y() && sx < canvas_x() + canvas_w() && sy < canvas_y() + canvas_h();
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(status_msg, sizeof status_msg, fmt, ap);
	va_end(ap);
	status_until = now_s() + 4;
}

/* undo / redo: whole-board snapshots */
#define UNDO 64
static struct board undo_st[UNDO], redo_st[UNDO];
static int nundo, nredo;

static void push_undo_state(const struct board *b)
{
	if (nundo == UNDO) {
		memmove(undo_st, undo_st + 1, sizeof undo_st[0] * (UNDO - 1));
		nundo--;
	}
	undo_st[nundo++] = *b;
	nredo = 0;
}

static void push_undo(void) { push_undo_state(&B); }

static void undo(void)
{
	if (!nundo) { say("Nothing to undo"); return; }
	redo_st[nredo++] = B;
	B = undo_st[--nundo];
	sel = sel_wire = -1;
	say("Undone");
}

static void redo(void)
{
	if (!nredo) { say("Nothing to redo"); return; }
	undo_st[nundo++] = B;
	B = redo_st[--nredo];
	sel = sel_wire = -1;
	say("Redone");
}

/* ------------------------------------------------------------------ hits */

static int hit_out_pin(int wx, int wy)
{
	for (int i = MAXCOMP - 1; i >= 0; i--) {
		if (!has_output(B.c[i].kind)) continue;
		int x, y;
		out_pin(&B.c[i], &x, &y);
		if (abs(wx - x) <= 7 && abs(wy - y) <= 7) return i;
	}
	return -1;
}

static int hit_in_pin(int wx, int wy, int *pin)
{
	for (int i = MAXCOMP - 1; i >= 0; i--) {
		const struct comp *c = &B.c[i];
		for (int p = 0; c->kind != C_NONE && p < c->nin; p++) {
			int x, y;
			in_pin(c, p, &x, &y);
			if (abs(wx - x) <= 7 && abs(wy - y) <= 6) {
				*pin = p;
				return i;
			}
		}
	}
	return -1;
}

static int hit_comp(int wx, int wy)
{
	for (int i = MAXCOMP - 1; i >= 0; i--) {
		const struct comp *c = &B.c[i];
		if (c->kind != C_NONE && wx >= c->x && wx < c->x + comp_w(c) && wy >= c->y && wy < c->y + comp_h(c))
			return i;
	}
	return -1;
}

static int hit_wire(int wx, int wy, int *pin)
{
	int px[6], py[6];
	for (int i = 0; i < MAXCOMP; i++) {
		const struct comp *c = &B.c[i];
		for (int p = 0; c->kind != C_NONE && p < c->nin; p++) {
			if (!c->src[p]) continue;
			int n = wire_route(i, p, px, py);
			for (int k = 0; k + 1 < n; k++) {
				int xa = px[k] < px[k + 1] ? px[k] : px[k + 1], xb = px[k] ^ px[k + 1] ^ xa;
				int ya = py[k] < py[k + 1] ? py[k] : py[k + 1], yb = py[k] ^ py[k + 1] ^ ya;
				if (wx >= xa - 4 && wx <= xb + 4 && wy >= ya - 4 && wy <= yb + 4) {
					*pin = p;
					return i;
				}
			}
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- buttons */

enum {
	B_NEW = 1, B_PRESETS, B_RUN, B_STEP, B_SPEED, B_TRUTH, B_UNDO, B_REDO, B_CENTER, B_HELP, B_MENU,
	B_PALETTE, B_PIN_MINUS, B_PIN_PLUS, B_RENAME, B_DELETE, B_TOGGLE, B_PRESET, B_MENU_ITEM, B_CLOSE
};

struct button {
	int x, y, w, h, id, arg;
};
static struct button btns[96];
static int nbtn, modal_btn_start;

static int button(int x, int y, int w, int h, int id, int arg, const char *label, int active)
{
	int hover = mx >= x && mx < x + w && my >= y && my < y + h;
	uint32_t bg = active ? 0x2f6f9f : hover ? 0x3a4356 : 0x2c3240;
	round_rect(x, y, w, h, 4, bg);
	if (label) text(x + (w - text_w(label)) / 2, y + (h - 16) / 2, hover || active ? 0xffffff : 0xd8dee9, label);
	if (nbtn < (int)(sizeof btns / sizeof btns[0])) btns[nbtn++] = (struct button){x, y, w, h, id, arg};
	return x + w;
}

static const struct button *hit_button(int x, int y)
{
	int from = mode == M_EDIT ? 0 : modal_btn_start;
	for (int i = nbtn - 1; i >= from; i--) {
		const struct button *b = &btns[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return b;
	}
	return NULL;
}

/* ----------------------------------------------------------------- colors */

#define COL_BG 0x161a21
#define COL_GRID 0x272d38
#define COL_PANEL 0x1e232c
#define COL_PANEL2 0x232934
#define COL_LINE 0x343c4b
#define COL_TEXT 0xd8dee9
#define COL_DIM 0x7d8799
#define COL_ACCENT 0x4fc3f7
#define COL_BODY 0x2b3547
#define COL_EDGE 0xa9b8cc
#define COL_SEL 0xffd54f
#define COL_W0 0x41604a
#define COL_W1 0x52ff7a
#define COL_FLOAT 0xff6b6b
#define COL_LED 0xff4d4d

static uint32_t wire_col(int v) { return v ? COL_W1 : COL_W0; }

/* ------------------------------------------------------------ part drawing */

/* Horizontal extent [l, r] of a gate body of size bw x h at row y. */
static void gate_span(int k, double bw, double h, double y, double *l, double *r)
{
	double dy = (y - h / 2) / (h / 2);
	if (dy < -1) dy = -1;
	if (dy > 1) dy = 1;
	double s = sqrt(1 - dy * dy);
	switch (k) {
	case C_AND: case C_NAND:
		*l = 0;
		*r = bw * 0.5 + bw * 0.5 * s;
		break;
	case C_OR: case C_NOR: case C_XOR: case C_XNOR:
		*l = bw * 0.2 * s;
		*r = bw * 0.3 + bw * 0.7 * sqrt(1 - fabs(dy));
		break;
	default: /* BUF, NOT: triangle */
		*l = 0;
		*r = bw * (1 - fabs(dy));
	}
}

static int curved_back(int k) { return k == C_OR || k == C_NOR || k == C_XOR || k == C_XNOR; }

/* Draws a gate symbol with its top-left at (x0, y0); returns the body's right edge. */
static int gate_symbol(int k, int x0, int y0, int bw, int h, uint32_t fill, uint32_t edge)
{
	int off = (k == C_XOR || k == C_XNOR) ? bw / 8 : 0;
	double l, r, l2, r2, l3, r3;
	for (int y = 0; y < h; y++) {
		gate_span(k, bw, h, y + 0.5, &l, &r);
		hline(x0 + off + (int)l, x0 + off + (int)r, y0 + y, edge);
	}
	for (int y = 2; y < h - 2; y++) {
		gate_span(k, bw, h, y + 0.5, &l, &r);
		gate_span(k, bw, h, y - 1.5, &l2, &r2);
		gate_span(k, bw, h, y + 2.5, &l3, &r3);
		double li = fmax(l, fmax(l2, l3)) + 2, ri = fmin(r, fmin(r2, r3)) - 2;
		if (ri > li) hline(x0 + off + (int)li, x0 + off + (int)ri, y0 + y, fill);
	}
	if (off) /* XOR's extra back curve */
		for (int y = 0; y < h; y++) {
			gate_span(k, bw, h, y + 0.5, &l, &r);
			pset(x0 + (int)l, y0 + y, edge);
			pset(x0 + (int)l + 1, y0 + y, edge);
		}
	int right = x0 + off + bw;
	if (inverted(k)) {
		int br = bw >= 40 ? 4 : 3;
		circle(right + br, y0 + h / 2, br, edge);
		circle(right + br, y0 + h / 2, br - 2, fill);
		right += 2 * br;
	}
	return right;
}

static void pin_dot(int x, int y, int connected, int v)
{
	if (connected) {
		circle(x, y, 3, wire_col(v));
	} else {
		circle(x, y, 4, COL_FLOAT);
		circle(x, y, 2, COL_BG);
	}
}

static void draw_comp(const struct comp *c, int sx, int sy, int selected)
{
	int w = comp_w(c), h = comp_h(c);
	uint32_t edge = selected ? COL_SEL : COL_EDGE;
	int ox, oy;

	/* input stubs */
	for (int p = 0; p < c->nin; p++) {
		int px, py;
		in_pin(c, p, &px, &py);
		px += sx - c->x;
		py += sy - c->y;
		int bx = sx;
		if (is_gate(c->kind) && curved_back(c->kind)) {
			double l, r;
			gate_span(c->kind, BW, h, py - sy + 0.5, &l, &r);
			bx = sx + (int)l;
		}
		int v = src_val(c->src[p]);
		seg(px, py, bx + 1, py, 2, c->src[p] ? wire_col(v) : COL_EDGE);
		pin_dot(px, py, c->src[p] != 0, v);
	}
	/* body */
	if (is_gate(c->kind)) {
		int right = gate_symbol(c->kind, sx, sy, BW, h, COL_BODY, edge);
		int tx = sx + (c->kind == C_XOR || c->kind == C_XNOR ? 6 : 0) + (curved_back(c->kind) ? 10 : 4);
		if (c->kind != C_BUF && c->kind != C_NOT) text(tx, sy + h / 2 - 8, COL_DIM, KNAME[c->kind]);
		(void)right;
	} else if (c->kind == C_IN) {
		round_rect(sx, sy, w, h, 6, edge);
		round_rect(sx + 2, sy + 2, w - 4, h - 4, 5, COL_BODY);
		round_rect(sx + 8, sy + 10, 32, 12, 6, c->val ? 0x2e8b57 : 0x3a4252);
		circle(c->val ? sx + 33 : sx + 15, sy + 16, 7, c->val ? COL_W1 : 0x9aa5b8);
	} else if (c->kind == C_OUT) {
		if (c->val) {
			circle(sx + 16, sy + 16, 15, mix(COL_BG, COL_LED, 90));
			circle(sx + 16, sy + 16, 12, COL_LED);
			circle(sx + 13, sy + 12, 3, 0xffc0c0);
		} else {
			circle(sx + 16, sy + 16, 12, edge);
			circle(sx + 16, sy + 16, 10, 0x4a2a2e);
		}
	} else if (c->kind == C_CLK) {
		round_rect(sx, sy, w, h, 4, edge);
		round_rect(sx + 2, sy + 2, w - 4, h - 4, 3, COL_BODY);
		uint32_t wc = wire_col(c->val);
		int yh = sy + 9, yl = sy + 22;
		seg(sx + 6, yl, sx + 14, yl, 2, wc);
		seg(sx + 14, yl, sx + 14, yh, 2, wc);
		seg(sx + 14, yh, sx + 24, yh, 2, wc);
		seg(sx + 24, yh, sx + 24, yl, 2, wc);
		seg(sx + 24, yl, sx + 34, yl, 2, wc);
		seg(sx + 34, yl, sx + 34, yh, 2, wc);
		seg(sx + 34, yh, sx + 42, yh, 2, wc);
	} else if (c->kind == C_HIGH) {
		round_rect(sx, sy, w, h, 4, edge);
		round_rect(sx + 2, sy + 2, w - 4, h - 4, 3, COL_BODY);
		text(sx + 12, sy + 8, COL_W1, "1");
	}
	/* output stub */
	if (has_output(c->kind)) {
		out_pin(c, &ox, &oy);
		ox += sx - c->x;
		oy += sy - c->y;
		seg(sx + w - 1, oy, ox, oy, 2, wire_col(c->val));
		circle(ox, oy, 3, wire_col(c->val));
	}
	/* label */
	uint32_t lc = selected ? COL_SEL : (c->kind == C_IN || c->kind == C_OUT ? COL_TEXT : COL_DIM);
	if (c->kind == C_OUT) text(sx + w + 4, sy + 8, lc, c->name);
	else text(sx, sy - 18, lc, c->name);
	if (selected && is_gate(c->kind)) rect_outline(sx - 3, sy - 3, w + 6, h + 6, mix(COL_BG, COL_SEL, 80));
}

/* small palette icon centred at (cx, cy) */
static void draw_icon(int k, int cx, int cy)
{
	if (is_gate(k)) {
		int bw = 28, h = 22;
		int x0 = cx - bw / 2 - 2, y0 = cy - h / 2;
		int nin = min_in(k);
		for (int p = 0; p < nin; p++) {
			int py = nin == 1 ? cy : y0 + 6 + p * 10;
			seg(x0 - 8, py, x0 + 4, py, 2, COL_EDGE);
		}
		int right = gate_symbol(k, x0, y0, bw, h, COL_BODY, COL_EDGE);
		seg(right, cy, right + 8, cy, 2, COL_EDGE);
		return;
	}
	if (k == C_IN) {
		round_rect(cx - 18, cy - 7, 36, 14, 7, 0x2e8b57);
		circle(cx + 11, cy, 7, COL_W1);
	} else if (k == C_OUT) {
		circle(cx, cy, 10, COL_LED);
		circle(cx - 3, cy - 3, 2, 0xffc0c0);
	} else if (k == C_CLK) {
		int x = cx - 18, yh = cy - 6, yl = cy + 6;
		seg(x, yl, x + 8, yl, 2, COL_W1);
		seg(x + 8, yl, x + 8, yh, 2, COL_W1);
		seg(x + 8, yh, x + 18, yh, 2, COL_W1);
		seg(x + 18, yh, x + 18, yl, 2, COL_W1);
		seg(x + 18, yl, x + 28, yl, 2, COL_W1);
		seg(x + 28, yl, x + 28, yh, 2, COL_W1);
		seg(x + 28, yh, x + 36, yh, 2, COL_W1);
	} else if (k == C_HIGH) {
		round_rect(cx - 12, cy - 11, 24, 22, 4, COL_EDGE);
		round_rect(cx - 10, cy - 9, 20, 18, 3, COL_BODY);
		text(cx - 4, cy - 8, COL_W1, "1");
	}
}

/* ------------------------------------------------------------------ render */

static const int PALETTE[] = {C_IN, C_OUT, C_CLK, C_HIGH, C_BUF, C_NOT, C_AND, C_OR, C_NAND, C_NOR, C_XOR, C_XNOR};
#define NPAL ((int)(sizeof PALETTE / sizeof PALETTE[0]))

static void draw_wires(void)
{
	int px[6], py[6];
	for (int i = 0; i < MAXCOMP; i++) {
		const struct comp *c = &B.c[i];
		for (int p = 0; c->kind != C_NONE && p < c->nin; p++) {
			if (!c->src[p]) continue;
			int n = wire_route(i, p, px, py);
			int v = src_val(c->src[p]);
			int selw = sel_wire == i && sel_wire_pin == p;
			uint32_t col = selw ? COL_SEL : wire_col(v);
			if (selw || v)
				for (int k = 0; k + 1 < n; k++)
					seg(to_sx(px[k]), to_sy(py[k]), to_sx(px[k + 1]), to_sy(py[k + 1]), 6,
					    mix(COL_BG, col, selw ? 70 : 45));
			for (int k = 0; k + 1 < n; k++)
				seg(to_sx(px[k]), to_sy(py[k]), to_sx(px[k + 1]), to_sy(py[k + 1]), 2, col);
		}
	}
}

static void draw_canvas(void)
{
	int cx = canvas_x(), cy = canvas_y(), cw = canvas_w(), ch = canvas_h();
	clip_set(cx, cy, cw, ch);
	fill_rect(cx, cy, cw, ch, COL_BG);
	int gx0 = ((pan_x % GRID) + GRID) % GRID, gy0 = ((pan_y % GRID) + GRID) % GRID;
	for (int y = cy + gy0; y < cy + ch; y += GRID)
		for (int x = cx + gx0; x < cx + cw; x += GRID) pset(x, y, COL_GRID);
	/* origin cross helps orientation when panning */
	for (int k = -4; k <= 4; k++) {
		pset(to_sx(0) + k, to_sy(0), COL_LINE);
		pset(to_sx(0), to_sy(0) + k, COL_LINE);
	}

	draw_wires();
	for (int i = 0; i < MAXCOMP; i++)
		if (B.c[i].kind != C_NONE) draw_comp(&B.c[i], to_sx(B.c[i].x), to_sy(B.c[i].y), i == sel);

	int wx = to_wx((int)mx), wy = to_wy((int)my), pin;
	/* pin under the cursor */
	if (mode == M_EDIT && in_canvas((int)mx, (int)my)) {
		int o = hit_out_pin(wx, wy), d = o < 0 ? hit_in_pin(wx, wy, &pin) : -1;
		int x, y;
		if (o >= 0 && drag != D_WIRE_OUT) {
			out_pin(&B.c[o], &x, &y);
			circle(to_sx(x), to_sy(y), 7, mix(COL_BG, COL_ACCENT, 120));
			circle(to_sx(x), to_sy(y), 3, COL_ACCENT);
		} else if (d >= 0 && drag != D_WIRE_IN) {
			in_pin(&B.c[d], pin, &x, &y);
			circle(to_sx(x), to_sy(y), 7, mix(COL_BG, COL_ACCENT, 120));
			circle(to_sx(x), to_sy(y), 3, COL_ACCENT);
		}
	}

	/* rubber-band wire while connecting */
	if (drag == D_WIRE_OUT || drag == D_WIRE_IN) {
		int x1, y1, px[6], py[6], n;
		if (drag == D_WIRE_OUT) {
			out_pin(&B.c[drag_comp], &x1, &y1);
			n = route(x1, y1, wx, wy, px, py);
		} else {
			in_pin(&B.c[drag_comp], drag_pin, &x1, &y1);
			n = route(wx, wy, x1, y1, px, py);
		}
		for (int k = 0; k + 1 < n; k++)
			seg(to_sx(px[k]), to_sy(py[k]), to_sx(px[k + 1]), to_sy(py[k + 1]), 2, COL_ACCENT);
		circle((int)mx, (int)my, 4, COL_ACCENT);
	}

	/* pin count badges above the selected gate */
	if (mode == M_EDIT && sel >= 0 && max_in(B.c[sel].kind) > min_in(B.c[sel].kind) && drag != D_MOVE) {
		const struct comp *c = &B.c[sel];
		int bx = to_sx(c->x) + comp_w(c) + 4, by = to_sy(c->y) - 20;
		button(bx, by, 18, 18, B_PIN_MINUS, 0, "-", 0);
		button(bx + 20, by, 18, 18, B_PIN_PLUS, 0, "+", 0);
	}

	/* part being dragged in from the palette */
	if (drag == D_NEW && in_canvas((int)mx, (int)my)) {
		struct comp g = {.kind = drag_kind, .nin = min_in(drag_kind)};
		g.x = snap(wx - comp_w(&g) / 2);
		g.y = snap(wy - comp_h(&g) / 2);
		draw_comp(&g, to_sx(g.x), to_sy(g.y), 1);
	}

	if (!sim_stable) {
		const char *s = "OSCILLATING - circuit never settles";
		round_rect(cx + cw - text_w(s) - 24, cy + 8, text_w(s) + 16, 24, 4, 0x6b1f24);
		text(cx + cw - text_w(s) - 16, cy + 12, 0xffd0d0, s);
	}
	clip_all();
}

static void draw_toolbar(void)
{
	fill_rect(0, 0, W, TOP_H, COL_PANEL);
	fill_rect(0, TOP_H - 1, W, 1, COL_LINE);
	int x = 10, y = 6, h = 22;
	if (W >= 1000) {
		text(x, 9, COL_ACCENT, "GATE16");
		x = text(x + 56, 9, COL_DIM, "board") + 14;
	}
	char spd[16];
	snprintf(spd, sizeof spd, "%gHz", CLK_HZ[clk_rate]);
	x = button(x, y, 48, h, B_NEW, 0, "New", 0) + 4;
	x = button(x, y, 72, h, B_PRESETS, 0, "Presets", mode == M_PRESETS) + 12;
	x = button(x, y, 56, h, B_RUN, 0, clk_run ? "Stop" : "Run", clk_run) + 4;
	x = button(x, y, 48, h, B_STEP, 0, "Step", 0) + 4;
	x = button(x, y, 56, h, B_SPEED, 0, spd, 0) + 12;
	x = button(x, y, 56, h, B_TRUTH, 0, "Truth", mode == M_TRUTH) + 12;
	x = button(x, y, 48, h, B_UNDO, 0, "Undo", 0) + 4;
	x = button(x, y, 48, h, B_REDO, 0, "Redo", 0) + 4;
	x = button(x, y, 64, h, B_CENTER, 0, "Center", 0) + 12;
	x = button(x, y, 48, h, B_HELP, 0, "Help", mode == M_HELP) + 4;
	button(x, y, 48, h, B_MENU, 0, "Menu", mode == M_MENU);
	if (x + 64 + text_w(B.title) < W) text(W - text_w(B.title) - 12, 9, COL_TEXT, B.title);
}

static void draw_palette(void)
{
	int y0 = TOP_H, h = H - TOP_H - STATUS_H;
	fill_rect(0, y0, PAL_W, h, COL_PANEL2);
	fill_rect(PAL_W - 1, y0, 1, h, COL_LINE);
	text(10, y0 + 8, COL_DIM, "PARTS - drag");
	int cw = (PAL_W - 12) / 2, chh = 50;
	for (int i = 0; i < NPAL; i++) {
		int k = PALETTE[i];
		int x = 6 + (i % 2) * cw, y = y0 + 30 + (i / 2) * chh;
		int full = count_like(k) >= limit_of(k);
		int hover = mode == M_EDIT && mx >= x && mx < x + cw - 2 && my >= y && my < y + chh - 2;
		round_rect(x, y, cw - 2, chh - 2, 5, hover ? 0x343d4f : 0x2a303c);
		draw_icon(k, x + cw / 2 - 1, y + 17);
		const char *nm = KNAME[k];
		text(x + (cw - 2 - text_w(nm)) / 2, y + 31, full ? COL_FLOAT : COL_DIM, nm);
		if (nbtn < (int)(sizeof btns / sizeof btns[0])) btns[nbtn++] = (struct button){x, y, cw - 2, chh - 2, B_PALETTE, k};
	}
	int y = y0 + 30 + (NPAL / 2) * chh + 6;
	fill_rect(8, y, PAL_W - 16, 1, COL_LINE);
	y += 8;
	textf(10, y, count_like(C_AND) >= MAXGATES ? COL_FLOAT : COL_TEXT, "Gates  %2d/%d", count_like(C_AND), MAXGATES);
	textf(10, y + 18, COL_DIM, "Inputs %2d/%d", count_like(C_IN), MAXIN);
	textf(10, y + 36, COL_DIM, "LEDs   %2d/%d", count_like(C_OUT), MAXOUT);
	y += 62;

	/* inspector for the selected part */
	if (y + 150 > y0 + h) return;
	fill_rect(8, y, PAL_W - 16, 1, COL_LINE);
	y += 8;
	if (sel < 0) {
		text(10, y, COL_DIM, "Click a part");
		text(10, y + 18, COL_DIM, "to edit it.");
		return;
	}
	struct comp *c = &B.c[sel];
	text(10, y, COL_SEL, c->name);
	text(10 + text_w(c->name) + 8, y, COL_DIM, KNAME[c->kind]);
	textf(10, y + 20, COL_TEXT, "value ");
	text(58, y + 20, c->val ? COL_W1 : COL_DIM, c->val ? "1" : "0");
	y += 44;
	if (max_in(c->kind) > min_in(c->kind)) {
		textf(10, y + 3, COL_TEXT, "inputs %d", c->nin);
		button(PAL_W - 52, y, 20, 22, B_PIN_MINUS, 0, "-", 0);
		button(PAL_W - 30, y, 20, 22, B_PIN_PLUS, 0, "+", 0);
		y += 28;
	}
	if (c->kind == C_IN) {
		button(10, y, PAL_W - 20, 22, B_TOGGLE, 0, "Toggle", 0);
		y += 26;
	}
	button(10, y, PAL_W - 20, 22, B_RENAME, 0, "Rename", 0);
	button(10, y + 26, PAL_W - 20, 22, B_DELETE, 0, "Delete", 0);
}

static const char *hover_hint(void)
{
	if (drag == D_NEW) return "Release on the board to place the part";
	if (drag == D_WIRE_OUT) return "Release on an input pin to connect (empty spot = cancel)";
	if (drag == D_WIRE_IN) return "Release on an output pin to connect";
	if (drag == D_PAN) return "Moving the board";
	if (drag == D_MOVE) return "Moving part";
	if (!in_canvas((int)mx, (int)my)) return "Drag parts from the palette onto the board";
	int wx = to_wx((int)mx), wy = to_wy((int)my), pin, i;
	if (hit_out_pin(wx, wy) >= 0) return "Output pin: drag to an input pin to wire it";
	if ((i = hit_in_pin(wx, wy, &pin)) >= 0)
		return B.c[i].src[pin] ? "Input pin: drag to re-wire, right-click to disconnect"
				       : "Input pin: drag to an output pin to wire it";
	if ((i = hit_comp(wx, wy)) >= 0) {
		if (B.c[i].kind == C_IN) return "Switch: click to toggle, drag to move, right-click deletes";
		if (max_in(B.c[i].kind) > min_in(B.c[i].kind))
			return "Gate: drag to move, mouse wheel or +/- changes inputs, right-click deletes";
		return "Drag to move, right-click to delete";
	}
	if (hit_wire(wx, wy, &pin) >= 0) return "Wire: click to select (Del deletes), right-click deletes";
	return "Drag the empty board to move it   Q menu   H help   Ctrl+Z undo";
}

static void draw_status(void)
{
	int y = H - STATUS_H;
	fill_rect(0, y, W, STATUS_H, COL_PANEL);
	fill_rect(0, y, W, 1, COL_LINE);
	if (status_msg[0] && now_s() < status_until) text(10, y + 4, COL_SEL, status_msg);
	else text(10, y + 4, COL_DIM, nmice ? hover_hint() : "No mouse found: plug one in, or Q -> Classic keyboard editor");
	char buf[48];
	snprintf(buf, sizeof buf, "%s  CLK %s", sim_stable ? "stable" : "OSCILLATING", clk_run ? "running" : "stopped");
	text(W - text_w(buf) - 10, y + 4, sim_stable ? COL_DIM : COL_FLOAT, buf);
}

static void dim_screen(void)
{
	for (int i = 0; i < W * H; i++) bb[i] = (bb[i] >> 1) & 0x7f7f7f;
}

static int panel_x, panel_y, panel_w, panel_h; /* the open dialog */

static void panel(int x, int y, int w, int h, const char *title)
{
	panel_x = x, panel_y = y, panel_w = w, panel_h = h;
	round_rect(x - 1, y - 1, w + 2, h + 2, 8, COL_LINE);
	round_rect(x, y, w, h, 8, COL_PANEL2);
	text(x + 16, y + 12, COL_ACCENT, title);
}

/* ----------------------------------------------------------------- presets */

static int P(int k, int gx, int gy, const char *name)
{
	int i = add_comp(k, gx * GRID, gy * GRID);
	if (name) snprintf(B.c[i].name, sizeof B.c[i].name, "%s", name);
	return i;
}
static void Wr(int dst, int pin, int src) { B.c[dst].src[pin] = src + 1; }

static void pr_half_adder(void)
{
	int a = P(C_IN, 2, 3, "A"), b = P(C_IN, 2, 9, "B");
	int x = P(C_XOR, 10, 2, NULL), n = P(C_AND, 10, 8, NULL);
	Wr(x, 0, a); Wr(x, 1, b); Wr(n, 0, a); Wr(n, 1, b);
	Wr(P(C_OUT, 18, 3, "SUM"), 0, x); Wr(P(C_OUT, 18, 9, "CARRY"), 0, n);
}

static void pr_full_adder(void)
{
	int a = P(C_IN, 2, 2, "A"), b = P(C_IN, 2, 7, "B"), ci = P(C_IN, 2, 13, "Cin");
	int x1 = P(C_XOR, 10, 2, NULL), x2 = P(C_XOR, 19, 5, NULL);
	int a1 = P(C_AND, 10, 8, NULL), a2 = P(C_AND, 19, 11, NULL), o = P(C_OR, 27, 9, NULL);
	Wr(x1, 0, a); Wr(x1, 1, b); Wr(x2, 0, x1); Wr(x2, 1, ci);
	Wr(a1, 0, a); Wr(a1, 1, b); Wr(a2, 0, x1); Wr(a2, 1, ci);
	Wr(o, 0, a1); Wr(o, 1, a2);
	Wr(P(C_OUT, 35, 6, "SUM"), 0, x2); Wr(P(C_OUT, 35, 10, "Cout"), 0, o);
}

static void pr_sr_latch(void)
{
	int s = P(C_IN, 2, 3, "S"), r = P(C_IN, 2, 10, "R");
	int n1 = P(C_NOR, 11, 2, NULL), n2 = P(C_NOR, 11, 9, NULL);
	Wr(n1, 0, r); Wr(n1, 1, n2); Wr(n2, 0, s); Wr(n2, 1, n1);
	Wr(P(C_OUT, 20, 3, "Q"), 0, n1); Wr(P(C_OUT, 20, 10, "/Q"), 0, n2);
}

static void pr_d_latch(void)
{
	int d = P(C_IN, 2, 3, "D"), en = P(C_IN, 2, 10, "EN");
	int g0 = P(C_NOT, 8, 7, NULL), g1 = P(C_NAND, 15, 2, NULL), g2 = P(C_NAND, 15, 9, NULL);
	int g3 = P(C_NAND, 23, 2, NULL), g4 = P(C_NAND, 23, 9, NULL);
	Wr(g0, 0, d); Wr(g1, 0, d); Wr(g1, 1, en); Wr(g2, 0, g0); Wr(g2, 1, en);
	Wr(g3, 0, g1); Wr(g3, 1, g4); Wr(g4, 0, g2); Wr(g4, 1, g3);
	Wr(P(C_OUT, 31, 3, "Q"), 0, g3); Wr(P(C_OUT, 31, 10, "/Q"), 0, g4);
}

static void pr_mux(void)
{
	int a = P(C_IN, 2, 2, "A"), s = P(C_IN, 2, 8, "SEL"), b = P(C_IN, 2, 13, "B");
	int n = P(C_NOT, 9, 6, NULL), a1 = P(C_AND, 15, 2, NULL), a2 = P(C_AND, 15, 11, NULL);
	int o = P(C_OR, 23, 6, NULL);
	Wr(n, 0, s); Wr(a1, 0, a); Wr(a1, 1, n); Wr(a2, 0, b); Wr(a2, 1, s); Wr(o, 0, a1); Wr(o, 1, a2);
	Wr(P(C_OUT, 31, 7, "Y"), 0, o);
}

static void pr_wide(void)
{
	int in[4];
	const char *nm[] = {"A", "B", "C", "D"};
	for (int i = 0; i < 4; i++) in[i] = P(C_IN, 2, 3 + i * 5, nm[i]);
	int g[3] = {P(C_AND, 13, 1, NULL), P(C_OR, 13, 8, NULL), P(C_XOR, 13, 15, NULL)};
	const char *on[] = {"ALL", "ANY", "ODD"};
	for (int k = 0; k < 3; k++) {
		set_pins(g[k], 4);
		for (int i = 0; i < 4; i++) Wr(g[k], i, in[i]);
		Wr(P(C_OUT, 22, 3 + k * 7, on[k]), 0, g[k]);
	}
}

static void pr_nand_xor(void)
{
	int a = P(C_IN, 2, 3, "A"), b = P(C_IN, 2, 10, "B");
	int n0 = P(C_NAND, 9, 6, NULL), n1 = P(C_NAND, 17, 2, NULL), n2 = P(C_NAND, 17, 10, NULL);
	int n3 = P(C_NAND, 25, 6, NULL);
	Wr(n0, 0, a); Wr(n0, 1, b); Wr(n1, 0, a); Wr(n1, 1, n0); Wr(n2, 0, n0); Wr(n2, 1, b);
	Wr(n3, 0, n1); Wr(n3, 1, n2);
	Wr(P(C_OUT, 33, 7, "Y"), 0, n3);
}

static void pr_comparator(void)
{
	int a0 = P(C_IN, 2, 2, "A0"), a1 = P(C_IN, 2, 7, "A1"), b0 = P(C_IN, 2, 12, "B0"), b1 = P(C_IN, 2, 17, "B1");
	int x0 = P(C_XNOR, 10, 1, NULL), x1 = P(C_XNOR, 10, 6, NULL);
	int nb1 = P(C_NOT, 10, 12, NULL), nb0 = P(C_NOT, 10, 16, NULL);
	int eq = P(C_AND, 18, 2, NULL), g1 = P(C_AND, 18, 8, NULL), g0 = P(C_AND, 18, 13, NULL);
	int gt = P(C_OR, 26, 9, NULL), lt = P(C_NOR, 26, 15, NULL);
	set_pins(g0, 3);
	Wr(x0, 0, a0); Wr(x0, 1, b0); Wr(x1, 0, a1); Wr(x1, 1, b1); Wr(nb1, 0, b1); Wr(nb0, 0, b0);
	Wr(eq, 0, x0); Wr(eq, 1, x1); Wr(g1, 0, a1); Wr(g1, 1, nb1);
	Wr(g0, 0, x1); Wr(g0, 1, a0); Wr(g0, 2, nb0);
	Wr(gt, 0, g1); Wr(gt, 1, g0); Wr(lt, 0, eq); Wr(lt, 1, gt);
	Wr(P(C_OUT, 34, 3, "A=B"), 0, eq); Wr(P(C_OUT, 34, 10, "A>B"), 0, gt); Wr(P(C_OUT, 34, 16, "A<B"), 0, lt);
}

static void pr_divider(void)
{
	int clk = P(C_CLK, 2, 13, "CLK");
	int g0 = P(C_NOT, 8, 13, NULL);
	int g1 = P(C_NAND, 14, 2, NULL), g2 = P(C_NAND, 14, 8, NULL), g3 = P(C_NAND, 20, 2, NULL), g4 = P(C_NAND, 20, 8, NULL);
	int g5 = P(C_NAND, 27, 2, NULL), g6 = P(C_NAND, 27, 8, NULL), g7 = P(C_NAND, 33, 2, NULL), g8 = P(C_NAND, 33, 8, NULL);
	Wr(g0, 0, clk);
	Wr(g1, 0, g8); Wr(g1, 1, g0); Wr(g2, 0, g7); Wr(g2, 1, g0); Wr(g3, 0, g1); Wr(g3, 1, g4); Wr(g4, 0, g2); Wr(g4, 1, g3);
	Wr(g5, 0, g3); Wr(g5, 1, clk); Wr(g6, 0, g4); Wr(g6, 1, clk); Wr(g7, 0, g5); Wr(g7, 1, g8); Wr(g8, 0, g6); Wr(g8, 1, g7);
	Wr(P(C_OUT, 41, 3, "Q"), 0, g7); Wr(P(C_OUT, 41, 9, "/Q"), 0, g8); Wr(P(C_OUT, 41, 14, "CLK"), 0, clk);
}

static void pr_empty(void) {}

static const struct {
	const char *name, *desc;
	void (*build)(void);
} PRESETS[] = {
	{"Empty board", "start from scratch", pr_empty},
	{"Half Adder", "XOR + AND", pr_half_adder},
	{"Full Adder", "two half adders + OR", pr_full_adder},
	{"SR Latch", "two cross-coupled NORs", pr_sr_latch},
	{"Gated D Latch", "NAND latch with enable", pr_d_latch},
	{"2:1 Multiplexer", "SEL picks A or B", pr_mux},
	{"4-input Gates", "wide AND / OR / XOR", pr_wide},
	{"XOR from NANDs", "four NAND gates", pr_nand_xor},
	{"2-bit Comparator", "A=B, A>B, A<B", pr_comparator},
	{"Clock Divider", "master-slave flip-flop", pr_divider},
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))

static void load_preset(int i)
{
	clear_board(PRESETS[i].name);
	PRESETS[i].build();
	sel = sel_wire = -1;
	pan_x = 48;
	pan_y = 32;
	simulate();
}

/* ---------------------------------------------------------------- overlays */

static void draw_presets(void)
{
	int w = 420, h = 64 + NPRESETS * 30, x = (W - w) / 2, y = (H - h) / 2;
	panel(x, y, w, h, "Load a preset circuit");
	for (int i = 0; i < NPRESETS; i++) {
		int by = y + 40 + i * 30;
		int hover = mx >= x + 12 && mx < x + w - 12 && my >= by && my < by + 26;
		round_rect(x + 12, by, w - 24, 26, 4, hover ? 0x2f6f9f : 0x2a303c);
		text(x + 24, by + 5, COL_TEXT, PRESETS[i].name);
		text(x + 200, by + 5, hover ? COL_TEXT : COL_DIM, PRESETS[i].desc);
		btns[nbtn++] = (struct button){x + 12, by, w - 24, 26, B_PRESET, i};
	}
}

static void draw_menu(void)
{
	static const char *items[] = {"Resume", "Classic keyboard editor", "Linux shell", "Reboot", "Power off"};
	int w = 300, h = 60 + 5 * 34, x = (W - w) / 2, y = (H - h) / 2;
	panel(x, y, w, h, "Menu");
	for (int i = 0; i < 5; i++) button(x + 16, y + 42 + i * 34, w - 32, 28, B_MENU_ITEM, i, items[i], 0);
}

static void draw_help(void)
{
	static const char *lines[] = {
		"MOUSE",
		"  drag a part from the palette    place it on the board",
		"  drag a part                     move it (snaps to the grid)",
		"  drag from a pin to a pin        connect a wire",
		"  drag a connected input pin      re-wire / pull the wire off",
		"  click a switch                  toggle it",
		"  wheel over a gate, - / + badge  fewer / more inputs (2..8)",
		"  drag empty board (any button)   move the whole board",
		"  right-click                     delete part, wire or connection",
		"",
		"KEYBOARD",
		"  1..8  toggle switches     Del  delete selection   +/-  inputs",
		"  C  run/stop clock   K  clock step   [ ]  clock speed",
		"  T  truth table   P  presets   N  new   R  rename   Arrows  pan",
		"  Ctrl+Z / Ctrl+Y  undo / redo    Home  center    Q  menu",
		"",
		"Up to 16 gates, 8 switches, 8 LEDs, 2 clocks. Unconnected inputs",
		"(red rings) read as 0. Feedback loops work: latches, flip-flops.",
	};
	int n = sizeof lines / sizeof lines[0];
	int w = 560, h = 56 + n * 18, x = (W - w) / 2, y = (H - h) / 2;
	panel(x, y, w, h, "Help  (click or press any key to close)");
	for (int i = 0; i < n; i++)
		text(x + 16, y + 40 + i * 18, lines[i][0] && lines[i][0] != ' ' ? COL_SEL : COL_TEXT, lines[i]);
}

static int tt_vars(int *vars)
{
	int n = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < MAXCOMP; i++)
			if (B.c[i].kind == (pass ? C_CLK : C_IN)) vars[n++] = i;
	return n;
}

/* outputs for one input combination, evaluated from a reset state */
static int tt_row(const int *vars, int nv, unsigned bits, int *outs, int *nout)
{
	struct board save = B;
	int st = sim_stable;
	for (int i = 0; i < MAXCOMP; i++) B.c[i].val = 0;
	for (int i = 0; i < nv; i++) B.c[vars[i]].val = (bits >> (nv - 1 - i)) & 1;
	simulate();
	int ok = sim_stable;
	*nout = 0;
	for (int i = 0; i < MAXCOMP; i++)
		if (B.c[i].kind == C_OUT) outs[(*nout)++] = B.c[i].val;
	B = save;
	sim_stable = st;
	return ok;
}

static int truth_rows_visible(void) { return (H - 200) / 18 > 4 ? (H - 200) / 18 : 4; }

static void draw_truth(void)
{
	int vars[MAXIN + MAXCLK], nv = tt_vars(vars), nrows = 1 << nv;
	int vis = truth_rows_visible();
	int nout = count_like(C_OUT);
	int colw = 48, w = 32 + (nv + nout) * colw + 16, h = 96 + vis * 18;
	if (w < 540) w = 540;
	int x = (W - w) / 2, y = (H - h) / 2;
	panel(x, y, w, h, "Truth table  (wheel / arrows scroll, click outside to close)");
	if (!nout) {
		text(x + 16, y + 44, COL_DIM, "Place some LEDs and wire them first.");
		return;
	}
	int cx = x + 16;
	for (int i = 0; i < nv; i++) text(cx + i * colw, y + 40, COL_SEL, B.c[vars[i]].name);
	int split = cx + nv * colw;
	fill_rect(split - 8, y + 38, 1, 22 + vis * 18, COL_LINE);
	for (int i = 0, o = 0; i < MAXCOMP; i++)
		if (B.c[i].kind == C_OUT) text(split + 8 + o++ * colw, y + 40, COL_ACCENT, B.c[i].name);
	for (int r = 0; r < vis && truth_top + r < nrows; r++) {
		unsigned bits = truth_top + r;
		int outs[MAXOUT], no, ry = y + 62 + r * 18;
		int ok = tt_row(vars, nv, bits, outs, &no);
		if (r & 1) fill_rect(x + 8, ry - 1, w - 16, 18, 0x262c38);
		for (int i = 0; i < nv; i++) {
			int v = (bits >> (nv - 1 - i)) & 1;
			text(cx + i * colw, ry, v ? COL_W1 : COL_DIM, v ? "1" : "0");
		}
		for (int o = 0; o < no; o++)
			text(split + 8 + o * colw, ry, !ok ? COL_FLOAT : outs[o] ? COL_W1 : COL_DIM, !ok ? "~" : outs[o] ? "1" : "0");
	}
	textf(x + 16, y + h - 26, COL_DIM, "rows %d-%d of %d, from reset state (~ = oscillates)", truth_top + 1,
	      truth_top + vis < nrows ? truth_top + vis : nrows, nrows);
}

static void draw_rename(void)
{
	int w = 320, h = 110, x = (W - w) / 2, y = (H - h) / 2;
	panel(x, y, w, h, "Rename part (Enter = OK, Esc = cancel)");
	round_rect(x + 16, y + 44, w - 32, 26, 4, COL_BG);
	int ex = text(x + 24, y + 49, COL_TEXT, rename_buf);
	if ((int)(now_s() * 2) & 1) fill_rect(ex + 1, y + 49, 2, 16, COL_ACCENT);
	text(x + 16, y + 80, COL_DIM, "up to 7 characters");
}

static void draw_cursor(void)
{
	static const char *shape[] = {
		"X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
		"X........X", "X.....XXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
	};
	for (int j = 0; j < (int)(sizeof shape / sizeof shape[0]); j++)
		for (int i = 0; shape[j][i]; i++) {
			if (shape[j][i] == 'X') pset((int)mx + i, (int)my + j, 0x000000);
			else if (shape[j][i] == '.') pset((int)mx + i, (int)my + j, 0xffffff);
		}
}

static void render(void)
{
	nbtn = 0;
	clip_all();
	draw_canvas();
	draw_palette();
	draw_toolbar();
	draw_status();
	if (mode != M_EDIT) {
		dim_screen();
		modal_btn_start = nbtn;
		if (mode == M_PRESETS) draw_presets();
		else if (mode == M_MENU) draw_menu();
		else if (mode == M_HELP) draw_help();
		else if (mode == M_TRUTH) draw_truth();
		else if (mode == M_RENAME) draw_rename();
	}
	if (nmice) draw_cursor();
	fb_blit();
}

/* ----------------------------------------------------------------- actions */

static void change_pins(int i, int d)
{
	if (i < 0) return;
	struct comp *c = &B.c[i];
	if (max_in(c->kind) == min_in(c->kind)) {
		say("%s has a fixed number of inputs", KNAME[c->kind]);
		return;
	}
	struct board before = B;
	if (set_pins(i, c->nin + d)) {
		push_undo_state(&before);
		say("%s now has %d inputs", c->name, c->nin);
	} else {
		say("%s gates take %d to %d inputs", KNAME[c->kind], min_in(c->kind), max_in(c->kind));
	}
}

static void delete_selection(void)
{
	if (sel >= 0) {
		push_undo();
		say("Deleted %s", B.c[sel].name);
		del_comp(sel);
		sel = -1;
	} else if (sel_wire >= 0) {
		push_undo();
		B.c[sel_wire].src[sel_wire_pin] = 0;
		sel_wire = -1;
		say("Wire deleted");
	}
}

static void toggle_input(int i)
{
	B.c[i].val = !B.c[i].val;
}

static void start_rename(void)
{
	if (sel < 0) {
		say("Select a part first");
		return;
	}
	snprintf(rename_buf, sizeof rename_buf, "%s", B.c[sel].name);
	mode = M_RENAME;
}

static void center_view(void)
{
	int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30), any = 0;
	for (int i = 0; i < MAXCOMP; i++) {
		const struct comp *c = &B.c[i];
		if (c->kind == C_NONE) continue;
		any = 1;
		if (c->x - STUB < x0) x0 = c->x - STUB;
		if (c->y - 18 < y0) y0 = c->y - 18;
		if (c->x + comp_w(c) + STUB + 48 > x1) x1 = c->x + comp_w(c) + STUB + 48;
		if (c->y + comp_h(c) > y1) y1 = c->y + comp_h(c);
	}
	if (!any) x0 = y0 = x1 = y1 = 0;
	pan_x = (canvas_w() - (x1 - x0)) / 2 - x0;
	pan_y = (canvas_h() - (y1 - y0)) / 2 - y0;
}

static void clock_tick(void)
{
	for (int i = 0; i < MAXCOMP; i++)
		if (B.c[i].kind == C_CLK) B.c[i].val = !B.c[i].val;
	simulate();
}

static void do_button(const struct button *b)
{
	switch (b->id) {
	case B_NEW: push_undo(); load_preset(0); say("New empty board - drag parts from the left"); break;
	case B_PRESETS: mode = M_PRESETS; break;
	case B_RUN: clk_run = !clk_run; break;
	case B_STEP: clock_tick(); break;
	case B_SPEED: clk_rate = (clk_rate + 1) % NCLK; say("Clock %gHz", CLK_HZ[clk_rate]); break;
	case B_TRUTH: mode = M_TRUTH; truth_top = 0; break;
	case B_UNDO: undo(); break;
	case B_REDO: redo(); break;
	case B_CENTER: center_view(); break;
	case B_HELP: mode = M_HELP; break;
	case B_MENU: mode = M_MENU; break;
	case B_PIN_MINUS: change_pins(sel, -1); break;
	case B_PIN_PLUS: change_pins(sel, +1); break;
	case B_RENAME: start_rename(); break;
	case B_DELETE: delete_selection(); break;
	case B_TOGGLE: if (sel >= 0) toggle_input(sel); break;
	case B_PRESET:
		push_undo();
		load_preset(b->arg);
		mode = M_EDIT;
		say("Loaded %s", PRESETS[b->arg].name);
		break;
	case B_MENU_ITEM:
		mode = M_EDIT;
		if (b->arg == 1) exit_code = EXIT_TUI;
		else if (b->arg == 2) exit_code = EXIT_SHELL;
		else if (b->arg == 3) exit_code = EXIT_REBOOT;
		else if (b->arg == 4) exit_code = EXIT_POWEROFF;
		break;
	case B_PALETTE:
		drag = D_NEW;
		drag_kind = b->arg;
		break;
	}
}

static void mouse_down(int btn)
{
	int sx = (int)mx, sy = (int)my;
	press_x = sx;
	press_y = sy;
	drag_moved = 0;
	if (mode != M_EDIT) {
		const struct button *b = btn == BTN_LEFT ? hit_button(sx, sy) : NULL;
		int inside = sx >= panel_x && sx < panel_x + panel_w && sy >= panel_y && sy < panel_y + panel_h;
		if (b) do_button(b);
		else if (!inside && mode != M_RENAME) mode = M_EDIT; /* click outside closes the dialog */
		return;
	}
	const struct button *b = btn == BTN_LEFT ? hit_button(sx, sy) : NULL;
	if (b) {
		do_button(b);
		return;
	}
	if (!in_canvas(sx, sy)) return;
	int wx = to_wx(sx), wy = to_wy(sy), pin, i;
	if (btn == BTN_MIDDLE) goto pan;
	if (btn == BTN_RIGHT) {
		if ((i = hit_in_pin(wx, wy, &pin)) >= 0 && B.c[i].src[pin]) {
			push_undo();
			B.c[i].src[pin] = 0;
			say("Disconnected");
		} else if ((i = hit_comp(wx, wy)) >= 0) {
			push_undo();
			say("Deleted %s", B.c[i].name);
			del_comp(i);
			if (sel == i) sel = -1;
		} else if ((i = hit_wire(wx, wy, &pin)) >= 0) {
			push_undo();
			B.c[i].src[pin] = 0;
			say("Wire deleted");
		} else {
			goto pan;
		}
		sel_wire = -1;
		return;
	}
	sel_wire = -1;
	if ((i = hit_out_pin(wx, wy)) >= 0) {
		drag = D_WIRE_OUT;
		drag_comp = i;
	} else if ((i = hit_in_pin(wx, wy, &pin)) >= 0) {
		if (B.c[i].src[pin]) { /* pick the wire up by its end */
			push_undo();
			drag = D_WIRE_OUT;
			drag_comp = B.c[i].src[pin] - 1;
			B.c[i].src[pin] = 0;
		} else {
			drag = D_WIRE_IN;
			drag_comp = i;
			drag_pin = pin;
		}
	} else if ((i = hit_comp(wx, wy)) >= 0) {
		sel = i;
		drag = D_MOVE;
		drag_comp = i;
		drag_dx = wx - B.c[i].x;
		drag_dy = wy - B.c[i].y;
	} else if ((i = hit_wire(wx, wy, &pin)) >= 0) {
		sel = -1;
		sel_wire = i;
		sel_wire_pin = pin;
	} else {
		sel = -1;
		goto pan;
	}
	return;
pan:
	drag = D_PAN;
	pan_start_x = pan_x;
	pan_start_y = pan_y;
}

static void mouse_move(void)
{
	if (abs((int)mx - press_x) > 3 || abs((int)my - press_y) > 3) {
		if (!drag_moved && drag == D_MOVE) push_undo();
		drag_moved = 1;
	}
	if (drag == D_MOVE && drag_moved) {
		B.c[drag_comp].x = snap(to_wx((int)mx) - drag_dx);
		B.c[drag_comp].y = snap(to_wy((int)my) - drag_dy);
	} else if (drag == D_PAN) {
		pan_x = pan_start_x + (int)mx - press_x;
		pan_y = pan_start_y + (int)my - press_y;
	}
}

static void mouse_up(int btn)
{
	(void)btn;
	int sx = (int)mx, sy = (int)my, wx = to_wx(sx), wy = to_wy(sy), pin, i;
	if (drag == D_NEW && in_canvas(sx, sy)) {
		struct comp tmp = {.kind = drag_kind, .nin = min_in(drag_kind)};
		struct board before = B;
		i = add_comp(drag_kind, wx - comp_w(&tmp) / 2, wy - comp_h(&tmp) / 2);
		if (i < 0) {
			say("%s", add_error);
		} else {
			push_undo_state(&before);
			sel = i;
			say("Added %s %s", KNAME[drag_kind], B.c[i].name);
		}
	} else if (drag == D_MOVE && !drag_moved && B.c[drag_comp].kind == C_IN) {
		toggle_input(drag_comp);
	} else if (drag == D_WIRE_OUT && (i = hit_in_pin(wx, wy, &pin)) >= 0) {
		push_undo();
		B.c[i].src[pin] = drag_comp + 1;
	} else if (drag == D_WIRE_IN && (i = hit_out_pin(wx, wy)) >= 0) {
		push_undo();
		B.c[drag_comp].src[drag_pin] = i + 1;
	}
	drag = D_NONE;
}

static void mouse_wheel(int d)
{
	if (mode == M_TRUTH) {
		truth_top -= d * 3;
		return;
	}
	if (mode != M_EDIT || !in_canvas((int)mx, (int)my)) return;
	int i = hit_comp(to_wx((int)mx), to_wy((int)my));
	if (i >= 0 && max_in(B.c[i].kind) > min_in(B.c[i].kind)) {
		sel = i;
		change_pins(i, d > 0 ? 1 : -1);
	} else {
		pan_y += d * 3 * GRID;
	}
}

static void handle_key(int k)
{
	if (mode == M_RENAME) {
		size_t len = strlen(rename_buf);
		if (k == K_ESC) mode = M_EDIT;
		else if (k == 13 || k == 10) {
			if (len && sel >= 0) {
				push_undo();
				snprintf(B.c[sel].name, sizeof B.c[sel].name, "%s", rename_buf);
			}
			mode = M_EDIT;
		} else if ((k == 127 || k == 8) && len) rename_buf[len - 1] = 0;
		else if (k > 32 && k < 127 && len < sizeof rename_buf - 1) rename_buf[len] = (char)k, rename_buf[len + 1] = 0;
		return;
	}
	if (mode == M_TRUTH) {
		if (k == K_DOWN) truth_top++;
		else if (k == K_UP) truth_top--;
		else if (k == K_PGDN || k == ' ') truth_top += truth_rows_visible();
		else if (k == K_PGUP) truth_top -= truth_rows_visible();
		else mode = M_EDIT;
		return;
	}
	if (mode == M_MENU) {
		switch (k) {
		case 's': case 'S': exit_code = EXIT_SHELL; break;
		case 'b': case 'B': exit_code = EXIT_REBOOT; break;
		case 'p': case 'P': exit_code = EXIT_POWEROFF; break;
		case 'c': case 'C': exit_code = EXIT_TUI; break;
		}
		mode = M_EDIT;
		return;
	}
	if (mode == M_PRESETS) {
		if (k >= '0' && k <= '9' && k - '0' < NPRESETS) {
			push_undo();
			load_preset(k - '0');
		}
		mode = M_EDIT;
		return;
	}
	if (mode == M_HELP) {
		mode = M_EDIT;
		return;
	}
	switch (k) {
	case K_DEL: case 127: case 8: delete_selection(); break;
	case '+': case '=': change_pins(sel, +1); break;
	case '-': case '_': change_pins(sel, -1); break;
	case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': {
		int n = k - '1';
		for (int i = 0; i < MAXCOMP; i++)
			if (B.c[i].kind == C_IN && n-- == 0) toggle_input(i);
		break;
	}
	case 'c': case 'C': clk_run = !clk_run; break;
	case 'k': case 'K': clock_tick(); break;
	case '[': if (clk_rate > 0) clk_rate--; say("Clock %gHz", CLK_HZ[clk_rate]); break;
	case ']': if (clk_rate < NCLK - 1) clk_rate++; say("Clock %gHz", CLK_HZ[clk_rate]); break;
	case 't': case 'T': mode = M_TRUTH; truth_top = 0; break;
	case 'p': case 'P': mode = M_PRESETS; break;
	case 'n': case 'N': push_undo(); load_preset(0); break;
	case 'r': case 'R': start_rename(); break;
	case 26: case 'z': case 'Z': undo(); break;
	case 25: case 'y': case 'Y': redo(); break;
	case 'h': case 'H': case '?': case K_F1: mode = M_HELP; break;
	case 'q': case 'Q': mode = M_MENU; break;
	case K_ESC:
		if (drag != D_NONE) drag = D_NONE;
		else if (sel >= 0 || sel_wire >= 0) sel = sel_wire = -1;
		else mode = M_MENU;
		break;
	case K_LEFT: pan_x += 4 * GRID; break;
	case K_RIGHT: pan_x -= 4 * GRID; break;
	case K_UP: pan_y += 4 * GRID; break;
	case K_DOWN: pan_y -= 4 * GRID; break;
	case K_HOME: center_view(); break;
	case 9: /* Tab cycles the selection */
		for (int n = 1; n <= MAXCOMP; n++) {
			int i = (sel + n + MAXCOMP) % MAXCOMP;
			if (B.c[i].kind != C_NONE) { sel = i; break; }
		}
		break;
	}
}

/* --------------------------------------------------------------- event loop */

static void read_mouse(struct mouse *m)
{
	struct input_event ev[64];
	ssize_t n = read(m->fd, ev, sizeof ev);
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR) return;
		close(m->fd); /* unplugged */
		*m = mice[--nmice];
		return;
	}
	double accel = SCALE > 1 ? 1.0 / SCALE : 1.0;
	for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
		const struct input_event *e = &ev[i];
		if (e->type == EV_REL) {
			double d = e->value * accel;
			if (e->code == REL_X) mx += d;
			else if (e->code == REL_Y) my += d;
			else if (e->code == REL_WHEEL) mouse_wheel(e->value);
		} else if (e->type == EV_ABS && m->absolute) {
			if (e->code == ABS_X)
				mx = (double)(e->value - m->ax.minimum) * W / (m->ax.maximum - m->ax.minimum + 1);
			else if (e->code == ABS_Y)
				my = (double)(e->value - m->ay.minimum) * H / (m->ay.maximum - m->ay.minimum + 1);
		} else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE ||
						 e->code == BTN_TOUCH)) {
			if (mx < 0) mx = 0;
			if (my < 0) my = 0;
			if (mx > W - 1) mx = W - 1;
			if (my > H - 1) my = H - 1;
			mouse_move();
			int b = e->code == BTN_TOUCH ? BTN_LEFT : e->code;
			if (e->value == 1) mouse_down(b);
			else if (e->value == 0) mouse_up(b);
		}
	}
	if (mx < 0) mx = 0;
	if (my < 0) my = 0;
	if (mx > W - 1) mx = W - 1;
	if (my > H - 1) my = H - 1;
	mouse_move();
}

int board_main(void)
{
	if (fb_open() < 0) return -1;
	tty_raw();
	atexit(tty_restore);
	atexit(fb_restore);
	ioctl(ttyfd, KDSETMODE, KD_GRAPHICS);
	signal(SIGSEGV, fatal_signal);
	signal(SIGBUS, fatal_signal);
	signal(SIGFPE, fatal_signal);
	signal(SIGABRT, fatal_signal);
	signal(SIGTERM, fatal_signal);

	scan_mice();
	mx = W / 2;
	my = H / 2;
	load_preset(2);
	say("Welcome to GATE16! Drag parts from the left, wire pins with the mouse, H for help.");
	double next_clk = now_s(), next_scan = now_s() + 2;

	while (exit_code < 0) {
		double t = now_s();
		if (clk_run && t >= next_clk) {
			clock_tick();
			next_clk = t + 0.5 / CLK_HZ[clk_rate];
		}
		if (t >= next_scan) { /* pick up mice plugged in later */
			scan_mice();
			next_scan = t + 2;
		}
		if (mode == M_TRUTH) {
			int vars[MAXIN + MAXCLK], nrows = 1 << tt_vars(vars), maxtop = nrows - truth_rows_visible();
			if (truth_top > maxtop) truth_top = maxtop;
			if (truth_top < 0) truth_top = 0;
		}
		simulate();
		render();

		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(0, &fds);
		int maxfd = 0;
		for (int i = 0; i < nmice; i++) {
			FD_SET(mice[i].fd, &fds);
			if (mice[i].fd > maxfd) maxfd = mice[i].fd;
		}
		struct timeval tv = {0, clk_run ? 20000 : 250000};
		if (select(maxfd + 1, &fds, NULL, NULL, &tv) <= 0) continue;
		if (FD_ISSET(0, &fds)) {
			int k;
			while ((k = read_key(0)) != K_NONE) handle_key(k);
		}
		for (int i = nmice - 1; i >= 0; i--)
			if (FD_ISSET(mice[i].fd, &fds)) read_mouse(&mice[i]);
	}
	return exit_code;
}

/* ------------------------------------------------------------------- tests */

static int find(const char *name)
{
	for (int i = 0; i < MAXCOMP; i++)
		if (B.c[i].kind != C_NONE && !strcmp(B.c[i].name, name)) return i;
	return -1;
}

static void setv(const char *name, int v) { B.c[find(name)].val = v; }
static int getv(const char *name) { return B.c[find(name)].val; }

int board_selftest(void)
{
	int fails = 0;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: board %s\n", what); fails++; } } while (0)
	load_preset(2); /* full adder */
	for (int v = 0; v < 8; v++) {
		setv("A", v & 1); setv("B", v >> 1 & 1); setv("Cin", v >> 2 & 1);
		simulate();
		int s = (v & 1) + (v >> 1 & 1) + (v >> 2 & 1);
		CHECK(sim_stable && getv("SUM") == (s & 1) && getv("Cout") == (s >> 1), "full adder");
	}
	load_preset(6); /* 4-input gates */
	for (int v = 0; v < 16; v++) {
		setv("A", v & 1); setv("B", v >> 1 & 1); setv("C", v >> 2 & 1); setv("D", v >> 3 & 1);
		simulate();
		CHECK(getv("ALL") == (v == 15) && getv("ANY") == (v != 0) && getv("ODD") == __builtin_parity(v), "wide gates");
	}
	load_preset(3); /* SR latch */
	setv("S", 1); simulate(); setv("S", 0); simulate();
	CHECK(sim_stable && getv("Q") == 1 && getv("/Q") == 0, "SR latch set");
	setv("R", 1); simulate(); setv("R", 0); simulate();
	CHECK(sim_stable && getv("Q") == 0 && getv("/Q") == 1, "SR latch reset");
	load_preset(8); /* comparator with a 3-input AND */
	for (int v = 0; v < 16; v++) {
		setv("A0", v & 1); setv("A1", v >> 1 & 1); setv("B0", v >> 2 & 1); setv("B1", v >> 3 & 1);
		simulate();
		int a = v & 3, b = v >> 2;
		CHECK(getv("A=B") == (a == b) && getv("A>B") == (a > b) && getv("A<B") == (a < b), "comparator");
	}
	load_preset(9); /* divider */
	int q0 = getv("Q"), toggles = 0;
	for (int i = 0; i < 8; i++) {
		int before = getv("Q");
		clock_tick();
		CHECK(sim_stable, "divider stable");
		if (getv("Q") != before) toggles++;
	}
	CHECK(toggles == 4 && getv("Q") == q0, "divider halves the clock");
	/* editing operations */
	load_preset(0);
	int g = add_comp(C_AND, 0, 0), a = add_comp(C_IN, 0, 0);
	CHECK(set_pins(g, 5) && B.c[g].nin == 5 && comp_h(&B.c[g]) == 6 * GRID, "grow pins");
	B.c[g].src[4] = a + 1;
	CHECK(set_pins(g, 3) && B.c[g].src[4] == 0, "shrink pins drops wires");
	CHECK(!set_pins(g, 1) && !set_pins(g, 9), "pin limits");
	del_comp(a);
	CHECK(B.c[g].src[0] == 0, "delete removes wires");
	for (int i = 0; i < 20; i++) add_comp(C_OR, 0, 0);
	CHECK(count_like(C_AND) == MAXGATES, "16 gate limit");
	printf(fails ? "board: %d check(s) failed\n" : "board: all checks passed\n", fails);
	return fails ? 1 : 0;
}
