/*
 * GATE16 - interactive 16-gate logic circuit editor for the Linux text console.
 *
 * Runs on a bare Linux VT (vgacon or fbcon), needs no ncurses or other
 * libraries, and is built as a static binary so it can be the only program
 * in a boot CD's initramfs.
 *
 * Circuit model:
 *   8 input switches (IN0..IN7), a clock (CLK), constants 0/1,
 *   16 gates (G0..GF) of type BUF/NOT/AND/OR/NAND/NOR/XOR/XNOR,
 *   8 output LEDs (OUT0..OUT7).
 * Any gate input may come from any other gate, so feedback circuits
 * (latches, flip-flops) work; oscillating circuits are detected.
 *
 * Exit codes (interpreted by the boot /init script):
 *   0 restart editor, 10 shell, 11 reboot, 12 power off.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NGATES 16
#define NIN 8
#define NOUT 8
#define MAXITER 64

#define EXIT_SHELL 10
#define EXIT_REBOOT 11
#define EXIT_POWEROFF 12

/* ------------------------------------------------------------------ model */

enum { T_NONE, T_BUF, T_NOT, T_AND, T_OR, T_NAND, T_NOR, T_XOR, T_XNOR, T_COUNT };
static const char *TNAME[T_COUNT] = {"----", "BUF", "NOT", "AND", "OR", "NAND", "NOR", "XOR", "XNOR"};
static const char *TOP[T_COUNT] = {"?", "", "!", "&", "|", "!&", "!|", "^", "!^"};

enum { S_NONE, S_ZERO, S_ONE, S_CLK, S_IN0, S_G0 = S_IN0 + NIN, S_COUNT = S_G0 + NGATES };

struct gate {
	int type, a, b;
};

struct circuit {
	char name[24];
	char in_name[NIN][6];
	char out_name[NOUT][6];
	struct gate g[NGATES];
	int out[NOUT];
};

static struct circuit C;
static int in_val[NIN];
static int clk_val;
static int gv[NGATES];
static int sim_stable = 1;
static int sim_iters;

static int one_input(int type) { return type == T_BUF || type == T_NOT; }

static int src_val(int s)
{
	if (s == S_ONE) return 1;
	if (s == S_CLK) return clk_val;
	if (s >= S_IN0 && s < S_IN0 + NIN) return in_val[s - S_IN0];
	if (s >= S_G0 && s < S_G0 + NGATES) return gv[s - S_G0];
	return 0;
}

static int eval_gate(const struct gate *g)
{
	int a = src_val(g->a), b = src_val(g->b);
	switch (g->type) {
	case T_BUF: return a;
	case T_NOT: return !a;
	case T_AND: return a & b;
	case T_OR: return a | b;
	case T_NAND: return !(a & b);
	case T_NOR: return !(a | b);
	case T_XOR: return a ^ b;
	case T_XNOR: return !(a ^ b);
	}
	return 0;
}

/* Relax the network in place (Gauss-Seidel) until nothing changes.
 * In-place updates let cross-coupled latches settle instead of ringing. */
static void simulate(void)
{
	for (sim_iters = 1; sim_iters <= MAXITER; sim_iters++) {
		int changed = 0;
		for (int i = 0; i < NGATES; i++) {
			int v = eval_gate(&C.g[i]);
			if (v != gv[i]) {
				gv[i] = v;
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

static void src_name(int s, char *buf, size_t n)
{
	if (s == S_NONE) snprintf(buf, n, "--");
	else if (s == S_ZERO) snprintf(buf, n, "0");
	else if (s == S_ONE) snprintf(buf, n, "1");
	else if (s == S_CLK) snprintf(buf, n, "CLK");
	else if (s < S_G0) snprintf(buf, n, "%s", C.in_name[s - S_IN0]);
	else snprintf(buf, n, "G%X", s - S_G0);
}

static void clear_circuit(const char *name)
{
	memset(&C, 0, sizeof C);
	snprintf(C.name, sizeof C.name, "%s", name);
	for (int i = 0; i < NIN; i++) snprintf(C.in_name[i], sizeof C.in_name[i], "IN%d", i);
	for (int i = 0; i < NOUT; i++) snprintf(C.out_name[i], sizeof C.out_name[i], "OUT%d", i);
	memset(in_val, 0, sizeof in_val);
	memset(gv, 0, sizeof gv);
	clk_val = 0;
}

/* --------------------------------------------------------------- presets */

#define IN(n) (S_IN0 + (n))
#define G(n) (S_G0 + (n))

static void set_gate(int i, int type, int a, int b) { C.g[i] = (struct gate){type, a, b}; }
static void name_in(int i, const char *s) { snprintf(C.in_name[i], sizeof C.in_name[i], "%s", s); }
static void set_out(int i, const char *s, int src)
{
	snprintf(C.out_name[i], sizeof C.out_name[i], "%s", s);
	C.out[i] = src;
}

static void p_half_adder(void)
{
	clear_circuit("Half Adder");
	name_in(0, "A"); name_in(1, "B");
	set_gate(0, T_XOR, IN(0), IN(1));
	set_gate(1, T_AND, IN(0), IN(1));
	set_out(0, "SUM", G(0)); set_out(1, "CARRY", G(1));
}

static void p_full_adder(void)
{
	clear_circuit("Full Adder");
	name_in(0, "A"); name_in(1, "B"); name_in(2, "Cin");
	set_gate(0, T_XOR, IN(0), IN(1));
	set_gate(1, T_XOR, G(0), IN(2));
	set_gate(2, T_AND, IN(0), IN(1));
	set_gate(3, T_AND, G(0), IN(2));
	set_gate(4, T_OR, G(2), G(3));
	set_out(0, "SUM", G(1)); set_out(1, "Cout", G(4));
}

static void p_sr_latch(void)
{
	clear_circuit("SR Latch (NOR)");
	name_in(0, "S"); name_in(1, "R");
	set_gate(0, T_NOR, IN(1), G(1));
	set_gate(1, T_NOR, IN(0), G(0));
	set_out(0, "Q", G(0)); set_out(1, "/Q", G(1));
}

static void p_d_latch(void)
{
	clear_circuit("Gated D Latch");
	name_in(0, "D"); name_in(1, "EN");
	set_gate(0, T_NOT, IN(0), S_NONE);
	set_gate(1, T_NAND, IN(0), IN(1));
	set_gate(2, T_NAND, G(0), IN(1));
	set_gate(3, T_NAND, G(1), G(4));
	set_gate(4, T_NAND, G(2), G(3));
	set_out(0, "Q", G(3)); set_out(1, "/Q", G(4));
}

static void p_mux(void)
{
	clear_circuit("2:1 Multiplexer");
	name_in(0, "A"); name_in(1, "B"); name_in(2, "SEL");
	set_gate(0, T_NOT, IN(2), S_NONE);
	set_gate(1, T_AND, IN(0), G(0));
	set_gate(2, T_AND, IN(1), IN(2));
	set_gate(3, T_OR, G(1), G(2));
	set_out(0, "Y", G(3));
}

static void p_nand_xor(void)
{
	clear_circuit("XOR from 4 NANDs");
	name_in(0, "A"); name_in(1, "B");
	set_gate(0, T_NAND, IN(0), IN(1));
	set_gate(1, T_NAND, IN(0), G(0));
	set_gate(2, T_NAND, IN(1), G(0));
	set_gate(3, T_NAND, G(1), G(2));
	set_out(0, "Y", G(3));
}

static void p_comparator(void)
{
	clear_circuit("2-bit Comparator");
	name_in(0, "A0"); name_in(1, "A1"); name_in(2, "B0"); name_in(3, "B1");
	set_gate(0, T_XNOR, IN(0), IN(2));
	set_gate(1, T_XNOR, IN(1), IN(3));
	set_gate(2, T_AND, G(0), G(1));           /* A == B */
	set_gate(3, T_NOT, IN(3), S_NONE);
	set_gate(4, T_AND, IN(1), G(3));          /* A1 & !B1 */
	set_gate(5, T_NOT, IN(2), S_NONE);
	set_gate(6, T_AND, IN(0), G(5));          /* A0 & !B0 */
	set_gate(7, T_AND, G(1), G(6));
	set_gate(8, T_OR, G(4), G(7));            /* A > B */
	set_gate(9, T_NOR, G(2), G(8));           /* A < B */
	set_out(0, "A=B", G(2)); set_out(1, "A>B", G(8)); set_out(2, "A<B", G(9));
}

static void p_divider(void)
{
	clear_circuit("Clock Divide-by-2");
	/* master-slave D flip-flop with D = /Q: Q toggles on each rising CLK edge */
	set_gate(0, T_NOT, S_CLK, S_NONE);
	set_gate(1, T_NAND, G(8), G(0));          /* master */
	set_gate(2, T_NAND, G(7), G(0));
	set_gate(3, T_NAND, G(1), G(4));
	set_gate(4, T_NAND, G(2), G(3));
	set_gate(5, T_NAND, G(3), S_CLK);         /* slave */
	set_gate(6, T_NAND, G(4), S_CLK);
	set_gate(7, T_NAND, G(5), G(8));
	set_gate(8, T_NAND, G(6), G(7));
	set_out(0, "CLK", S_CLK); set_out(1, "Q", G(7)); set_out(2, "/Q", G(8));
}

static void p_empty(void) { clear_circuit("Untitled"); }

static const struct {
	const char *name;
	void (*load)(void);
} PRESETS[] = {
	{"Empty circuit", p_empty},
	{"Half Adder", p_half_adder},
	{"Full Adder", p_full_adder},
	{"SR Latch (NOR)", p_sr_latch},
	{"Gated D Latch", p_d_latch},
	{"2:1 Multiplexer", p_mux},
	{"XOR from 4 NANDs", p_nand_xor},
	{"2-bit Comparator", p_comparator},
	{"Clock Divide-by-2", p_divider},
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))

/* ------------------------------------------------------------ screen buf */

#define SW 80
#define SH 25

enum { A_NORM, A_TITLE, A_BORDER, A_HI, A_ON, A_OFF, A_SEL, A_HDR, A_ERR, A_DIM, A_KEY, A_COUNT };
static const char *ATTR_SEQ[A_COUNT] = {
	"\033[0;37;40m",    /* normal */
	"\033[0;30;46m",    /* title bar */
	"\033[0;36;40m",    /* borders */
	"\033[0;1;37;40m",  /* highlight */
	"\033[0;1;32;40m",  /* logic 1 */
	"\033[0;1;30;40m",  /* logic 0 */
	"\033[0;30;43m",    /* selected field */
	"\033[0;1;33;40m",  /* headers */
	"\033[0;1;31;40m",  /* error */
	"\033[0;1;30;40m",  /* dim */
	"\033[0;1;36;40m",  /* key hints */
};

struct cell {
	char ch[5];
	unsigned char attr;
};
static struct cell scr[SH][SW];
static int ttyfd = 1;

static void scr_clear(void)
{
	for (int r = 0; r < SH; r++)
		for (int c = 0; c < SW; c++) {
			strcpy(scr[r][c].ch, " ");
			scr[r][c].attr = A_NORM;
		}
}

/* Write a UTF-8 string at (r, c); returns the column after it. */
static int put(int r, int c, int attr, const char *s)
{
	while (*s) {
		int len = 1;
		unsigned char u = (unsigned char)*s;
		if (u >= 0xf0) len = 4;
		else if (u >= 0xe0) len = 3;
		else if (u >= 0xc0) len = 2;
		if (r >= 0 && r < SH && c >= 0 && c < SW) {
			memcpy(scr[r][c].ch, s, len);
			scr[r][c].ch[len] = 0;
			scr[r][c].attr = attr;
		}
		s += len;
		c++;
	}
	return c;
}

static int putf(int r, int c, int attr, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return put(r, c, attr, buf);
}

static void fill(int r, int c, int w, int attr)
{
	for (int i = 0; i < w; i++) put(r, c + i, attr, " ");
}

static void box(int r, int c, int h, int w, const char *title)
{
	put(r, c, A_BORDER, "┌");
	put(r, c + w - 1, A_BORDER, "┐");
	put(r + h - 1, c, A_BORDER, "└");
	put(r + h - 1, c + w - 1, A_BORDER, "┘");
	for (int i = 1; i < w - 1; i++) {
		put(r, c + i, A_BORDER, "─");
		put(r + h - 1, c + i, A_BORDER, "─");
	}
	for (int i = 1; i < h - 1; i++) {
		put(r + i, c, A_BORDER, "│");
		put(r + i, c + w - 1, A_BORDER, "│");
		fill(r + i, c + 1, w - 2, A_NORM);
	}
	if (title) putf(r, c + 2, A_HDR, " %s ", title);
}

static void console_size(int *rows, int *cols)
{
	struct winsize ws = {0};
	*rows = SH;
	*cols = SW;
	if (ioctl(ttyfd, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	}
}

/* On a large framebuffer console (high-resolution UEFI screens) switch to
 * the kernel's built-in 16x32 font so the 80x25 editor is readable.
 * Harmless on VGA text mode, where the ioctl simply fails. */
static void pick_font(void)
{
	int rows, cols;
	console_size(&rows, &cols);
	if (cols < 2 * SW || rows < 2 * SH) return;
	struct console_font_op op = {.op = KD_FONT_OP_SET_DEFAULT, .data = (unsigned char *)"TER16x32"};
	ioctl(ttyfd, KDFONTOP, &op);
}

static void scr_flush(void)
{
	static char out[SH * SW * 24 + 4096];
	static int last_rows, last_cols;
	size_t n = 0;
	int rows, cols;
	console_size(&rows, &cols);
	if (rows != last_rows || cols != last_cols) {
		/* the console changed size under us (e.g. the kernel switched from
		 * VGA text to a framebuffer): maybe pick a bigger font, then wipe
		 * the old picture */
		pick_font();
		console_size(&rows, &cols);
		n += sprintf(out + n, "%s\033[2J", ATTR_SEQ[A_NORM]);
		last_rows = rows;
		last_cols = cols;
	}
	int r0 = rows > SH ? (rows - SH) / 2 : 0;
	int c0 = cols > SW ? (cols - SW) / 2 : 0;
	int last = -1;
	for (int r = 0; r < SH && r < rows; r++) {
		n += sprintf(out + n, "\033[%d;%dH", r0 + r + 1, c0 + 1);
		for (int c = 0; c < SW && c < cols; c++) {
			/* skip the bottom-right cell so the console never scrolls */
			if (r == rows - 1 && c == cols - 1) break;
			if (scr[r][c].attr != last) {
				last = scr[r][c].attr;
				n += sprintf(out + n, "%s", ATTR_SEQ[last]);
			}
			n += sprintf(out + n, "%s", scr[r][c].ch);
		}
	}
	n += sprintf(out + n, "%s", ATTR_SEQ[A_NORM]);
	for (size_t off = 0; off < n;) {
		ssize_t w = write(ttyfd, out + off, n - off);
		if (w < 0) {
			if (errno == EINTR) continue;
			break;
		}
		off += w;
	}
}

/* ---------------------------------------------------------------- input */

enum {
	K_NONE = 0, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN,
	K_HOME, K_END, K_DEL, K_INS, K_F1, K_F2, K_F3, K_F4, K_F5, K_ESC
};

static struct termios saved_tio;
static int tio_saved;

static void tty_restore(void)
{
	if (tio_saved) tcsetattr(ttyfd, TCSAFLUSH, &saved_tio);
	/* reset attributes, show cursor, clear */
	if (write(ttyfd, "\033[0m\033[?25h\033[2J\033[H", 18) < 0) {}
}

static void tty_raw(void)
{
	struct termios t;
	if (tcgetattr(0, &saved_tio) == 0) {
		tio_saved = 1;
		t = saved_tio;
		t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
		t.c_oflag &= ~OPOST;
		t.c_cflag |= CS8;
		t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		tcsetattr(0, TCSAFLUSH, &t);
	}
	/* hide cursor, clear screen */
	if (write(ttyfd, "\033[?25l\033[0m\033[2J", 14) < 0) {}
}

static int rd_byte(int timeout_ms)
{
	fd_set fds;
	struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	unsigned char ch;
	FD_ZERO(&fds);
	FD_SET(0, &fds);
	if (select(1, &fds, NULL, NULL, &tv) <= 0) return -1;
	if (read(0, &ch, 1) != 1) return -1;
	return ch;
}

/* Read one key, decoding Linux console / xterm escape sequences. */
static int read_key(int timeout_ms)
{
	int c = rd_byte(timeout_ms);
	if (c < 0) return K_NONE;
	if (c != 27) return c;
	int c1 = rd_byte(30);
	if (c1 < 0) return K_ESC;
	if (c1 != '[' && c1 != 'O') return K_ESC;
	int c2 = rd_byte(30);
	if (c1 == 'O') {
		switch (c2) {
		case 'P': return K_F1;
		case 'Q': return K_F2;
		case 'R': return K_F3;
		case 'S': return K_F4;
		case 'H': return K_HOME;
		case 'F': return K_END;
		}
		return K_NONE;
	}
	switch (c2) {
	case 'A': return K_UP;
	case 'B': return K_DOWN;
	case 'C': return K_RIGHT;
	case 'D': return K_LEFT;
	case 'H': return K_HOME;
	case 'F': return K_END;
	case '[': /* linux console F1-F5: ESC [ [ A..E */
		switch (rd_byte(30)) {
		case 'A': return K_F1;
		case 'B': return K_F2;
		case 'C': return K_F3;
		case 'D': return K_F4;
		case 'E': return K_F5;
		}
		return K_NONE;
	}
	if (c2 >= '0' && c2 <= '9') {
		int num = c2 - '0', ch;
		while ((ch = rd_byte(30)) >= '0' && ch <= '9') num = num * 10 + ch - '0';
		if (ch != '~') return K_NONE;
		switch (num) {
		case 1: case 7: return K_HOME;
		case 4: case 8: return K_END;
		case 2: return K_INS;
		case 3: return K_DEL;
		case 5: return K_PGUP;
		case 6: return K_PGDN;
		case 11: return K_F1;
		case 12: return K_F2;
		case 13: return K_F3;
		case 14: return K_F4;
		case 15: return K_F5;
		}
	}
	return K_NONE;
}

/* ------------------------------------------------------------- UI state */

static int focus;            /* 0 = gate table, 1 = outputs */
static int sel_gate, sel_col; /* sel_col: 0 type, 1 A, 2 B */
static int sel_out;
static char message[80];
static int msg_attr = A_HI;

static const double CLK_HZ[] = {0.5, 1, 2, 4, 8};
#define NCLK ((int)(sizeof CLK_HZ / sizeof CLK_HZ[0]))
static int clk_rate = 1;
static int clk_run;

static void msg(int attr, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof message, fmt, ap);
	va_end(ap);
	msg_attr = attr;
}

static const char *lamp(int v) { return v ? "■" : "·"; }
static int vattr(int v) { return v ? A_ON : A_OFF; }

/* Boolean expression for a source, expanding gates recursively. */
static void expr(int s, char *buf, size_t n, unsigned seen, int depth)
{
	char name[8];
	if (s < S_G0) {
		src_name(s, name, sizeof name);
		snprintf(buf, n, "%s", name);
		return;
	}
	int i = s - S_G0;
	const struct gate *g = &C.g[i];
	if (g->type == T_NONE) {
		snprintf(buf, n, "0");
		return;
	}
	if ((seen & (1u << i)) || depth > 6) { /* feedback loop or too deep */
		snprintf(buf, n, "G%X", i);
		return;
	}
	seen |= 1u << i;
	char a[160], b[160];
	expr(g->a, a, sizeof a, seen, depth + 1);
	if (g->type == T_BUF) {
		snprintf(buf, n, "%s", a);
		return;
	}
	if (g->type == T_NOT) {
		snprintf(buf, n, "!%s", a);
		return;
	}
	expr(g->b, b, sizeof b, seen, depth + 1);
	switch (g->type) {
	case T_NAND: snprintf(buf, n, "!(%s&%s)", a, b); break;
	case T_NOR: snprintf(buf, n, "!(%s|%s)", a, b); break;
	case T_XNOR: snprintf(buf, n, "!(%s^%s)", a, b); break;
	default: snprintf(buf, n, "(%s%s%s)", a, TOP[g->type], b); break;
	}
}

static void draw_src(int r, int c, int width, int s, int selected)
{
	char name[8];
	src_name(s, name, sizeof name);
	int attr = selected ? A_SEL : (s == S_NONE ? A_DIM : vattr(src_val(s)));
	fill(r, c, width, selected ? A_SEL : A_NORM);
	put(r, c, attr, name);
}

static void draw_main(void)
{
	char buf[256];
	scr_clear();

	/* title bar */
	fill(0, 0, SW, A_TITLE);
	put(0, 1, A_TITLE, "GATE16  Logic Gate Editor");
	putf(0, 30, A_TITLE, "Circuit: %s", C.name);
	if (sim_stable) putf(0, 66, A_TITLE, "[ STABLE ]");
	else put(0, 64, A_ERR, "[OSCILLATING]");

	/* gate table */
	box(2, 0, 20, 40, focus == 0 ? "GATES  (editing)" : "GATES");
	put(3, 2, A_HDR, "#   TYPE   IN A    IN B    OUT");
	for (int i = 0; i < NGATES; i++) {
		int r = 4 + i;
		const struct gate *g = &C.g[i];
		int here = focus == 0 && sel_gate == i;
		putf(r, 2, here ? A_HI : A_KEY, "G%X", i);
		if (here) put(r, 1, A_HI, "►");
		fill(r, 6, 6, here && sel_col == 0 ? A_SEL : A_NORM);
		put(r, 6, here && sel_col == 0 ? A_SEL : (g->type ? A_HI : A_DIM), TNAME[g->type]);
		if (g->type == T_NONE) {
			if (here && sel_col) fill(r, sel_col == 1 ? 13 : 21, 6, A_SEL);
			continue;
		}
		draw_src(r, 13, 6, g->a, here && sel_col == 1);
		if (one_input(g->type)) {
			fill(r, 21, 6, here && sel_col == 2 ? A_SEL : A_NORM);
			put(r, 21, here && sel_col == 2 ? A_SEL : A_DIM, "-");
		} else {
			draw_src(r, 21, 6, g->b, here && sel_col == 2);
		}
		putf(r, 29, vattr(gv[i]), "%d %s", gv[i], lamp(gv[i]));
		/* mark gates that drive an output */
		for (int o = 0; o < NOUT; o++)
			if (C.out[o] == S_G0 + i) {
				putf(r, 33, A_DIM, "►%.5s", C.out_name[o]);
				break;
			}
	}
	if (sim_stable) putf(20, 2, A_DIM, "settled in %d pass%s", sim_iters, sim_iters == 1 ? "" : "es");
	else put(20, 2, A_ERR, "no stable state: circuit oscillates");

	/* inputs */
	box(2, 41, 6, 39, "INPUTS  keys 1-8 toggle");
	for (int i = 0; i < NIN; i++) {
		int r = 3 + i / 4, c = 43 + (i % 4) * 9;
		putf(r, c, A_KEY, "%d", i + 1);
		putf(r, c + 2, vattr(in_val[i]), "%-4.4s%s", C.in_name[i], lamp(in_val[i]));
	}
	putf(5, 43, A_KEY, "CLK");
	putf(5, 47, vattr(clk_val), "%s", lamp(clk_val));
	putf(5, 49, clk_run ? A_ON : A_DIM, clk_run ? "RUN " : "STOP");
	putf(5, 54, A_NORM, "%gHz", CLK_HZ[clk_rate]);
	put(6, 43, A_DIM, "C run/stop  K step  [ ] speed");

	/* outputs */
	box(8, 41, 10, 39, focus == 1 ? "OUTPUTS  (editing)" : "OUTPUTS");
	for (int o = 0; o < NOUT; o++) {
		int r = 9 + o;
		int here = focus == 1 && sel_out == o;
		int v = src_val(C.out[o]);
		if (here) put(r, 42, A_HI, "►");
		putf(r, 43, here ? A_HI : A_KEY, "O%d", o);
		putf(r, 46, A_NORM, "%-5.5s", C.out_name[o]);
		put(r, 52, A_DIM, "◄");
		draw_src(r, 54, 6, C.out[o], here);
		if (C.out[o] != S_NONE) {
			put(r, 62, vattr(v), v ? "████ ON " : "░░░░ off");
		}
	}

	/* detail of the selected item */
	box(18, 41, 4, 39, "DETAIL");
	if (focus == 0) {
		const struct gate *g = &C.g[sel_gate];
		if (g->type == T_NONE) {
			putf(19, 43, A_DIM, "G%X is empty. Space or + sets a type.", sel_gate);
		} else {
			char an[8], bn[8];
			src_name(g->a, an, sizeof an);
			src_name(g->b, bn, sizeof bn);
			int c = putf(19, 43, A_NORM, "%s", an);
			c = putf(19, c, vattr(src_val(g->a)), "=%d", src_val(g->a));
			if (!one_input(g->type)) {
				c = put(19, c, A_NORM, " ");
				c = put(19, c, A_HI, TNAME[g->type]);
				c = putf(19, c, A_NORM, " %s", bn);
				c = putf(19, c, vattr(src_val(g->b)), "=%d", src_val(g->b));
			} else {
				c = put(19, c, A_NORM, " -> ");
				c = put(19, c, A_HI, TNAME[g->type]);
			}
			c = putf(19, c, A_NORM, " -> G%X", sel_gate);
			putf(19, c, vattr(gv[sel_gate]), "=%d", gv[sel_gate]);
			expr(S_G0 + sel_gate, buf, sizeof buf, 0, 0);
			putf(20, 43, A_DIM, "%.35s", buf);
		}
	} else {
		int s = C.out[sel_out];
		if (s == S_NONE) {
			putf(19, 43, A_DIM, "O%d unconnected. Space or + picks", sel_out);
			put(20, 43, A_DIM, "a source (input, CLK or gate).");
		} else {
			expr(s, buf, sizeof buf, 0, 0);
			putf(19, 43, A_NORM, "%s = %.28s", C.out_name[sel_out], buf);
			if (strlen(buf) > 28) putf(20, 43, A_NORM, "%.35s", buf + 28);
		}
	}

	/* key help + message line */
	int c = 0;
	static const char *help1[] = {"Arrows", "move", "Tab", "gates/outputs", "Space/+ -", "change",
				      "PgUp/Dn", "jump", "Del", "clear", NULL};
	static const char *help2[] = {"T", "truth table", "P", "presets", "R", "rename", "N", "new",
				      "F1/H", "help", "Q", "menu", NULL};
	for (int i = 0; help1[i]; i += 2) {
		c = put(22, c + 1, A_KEY, help1[i]);
		c = put(22, c + 1, A_NORM, help1[i + 1]);
	}
	c = 0;
	for (int i = 0; help2[i]; i += 2) {
		c = put(23, c + 1, A_KEY, help2[i]);
		c = put(23, c + 1, A_NORM, help2[i + 1]);
	}
	if (message[0]) putf(24, 1, msg_attr, "%.78s", message);
}

/* ----------------------------------------------------------- overlays */

static void draw_help(int page)
{
	static const char *text[] = {
		"GATE16 is a logic circuit editor with 16 gate slots (G0..GF).",
		"",
		"Each gate has a TYPE and up to two inputs (IN A, IN B). An input can",
		"be a switch (IN0..IN7, toggled with keys 1..8), the clock CLK, the",
		"constants 0/1, or the output of ANY gate - including itself, so",
		"latches and flip-flops with feedback work. The circuit is simulated",
		"live; if it never settles the title bar shows OSCILLATING.",
		"",
		"Eight output LEDs (O0..O7) can each show any input, CLK or gate.",
		"",
		"EDITING",
		"  Arrows       move between gates and fields (TYPE / IN A / IN B)",
		"  Tab          switch between the gate table and the outputs",
		"  Space, +     next value      Backspace, -   previous value",
		"  PgUp/PgDn    jump by 8 values             Del  clear gate/output",
		"  Gate types   BUF NOT AND OR NAND NOR XOR XNOR",
		"  R            rename circuit / input / output name under cursor",
		"",
		"SIMULATION",
		"  1..8  toggle input switch   C  run/stop clock   K  single clock step",
		"  [ ]   slower / faster clock T  truth table of all outputs",
		"  P presets (adders, latches, mux, comparator, divider)  N new circuit",
		"  Q menu: resume / shell / reboot / power off",
	};
	(void)page;
	scr_clear();
	box(0, 0, SH, SW, "HELP  (any key to close)");
	for (size_t i = 0; i < sizeof text / sizeof text[0] && i < SH - 2; i++)
		put(1 + i, 3, text[i][0] && !strpbrk(text[i], "abcdefghijklmnopqrstuvwxyz") ? A_HDR : A_NORM, text[i]);
}

/* Returns the number of truth-table variables (used inputs + CLK). */
static int tt_vars(int *vars)
{
	int used[S_COUNT] = {0}, n = 0;
	for (int i = 0; i < NGATES; i++) {
		if (C.g[i].type == T_NONE) continue;
		used[C.g[i].a] = 1;
		if (!one_input(C.g[i].type)) used[C.g[i].b] = 1;
	}
	for (int o = 0; o < NOUT; o++) used[C.out[o]] = 1;
	for (int i = 0; i < NIN; i++)
		if (used[S_IN0 + i]) vars[n++] = S_IN0 + i;
	if (used[S_CLK]) vars[n++] = S_CLK;
	return n;
}

/* Evaluate outputs for one input combination from a reset state.
 * Returns 1 if the circuit settled. Saves and restores live state. */
static int tt_row(const int *vars, int nv, unsigned bits, int *outs)
{
	int s_in[NIN], s_clk = clk_val, s_gv[NGATES], s_st = sim_stable, s_it = sim_iters;
	memcpy(s_in, in_val, sizeof s_in);
	memcpy(s_gv, gv, sizeof s_gv);
	for (int i = 0; i < nv; i++) {
		int v = (bits >> (nv - 1 - i)) & 1;
		if (vars[i] == S_CLK) clk_val = v;
		else in_val[vars[i] - S_IN0] = v;
	}
	memset(gv, 0, sizeof gv);
	simulate();
	int ok = sim_stable;
	for (int o = 0; o < NOUT; o++) outs[o] = src_val(C.out[o]);
	memcpy(in_val, s_in, sizeof s_in);
	memcpy(gv, s_gv, sizeof s_gv);
	clk_val = s_clk;
	sim_stable = s_st;
	sim_iters = s_it;
	return ok;
}

static void draw_truth(int top)
{
	int vars[NIN + 1], nv = tt_vars(vars), nrows = 1 << nv;
	char name[8];
	scr_clear();
	box(0, 0, SH, SW, "TRUTH TABLE  (arrows/PgUp/PgDn scroll, Esc close)");
	int c = 2;
	for (int i = 0; i < nv; i++) {
		src_name(vars[i], name, sizeof name);
		c = putf(1, c, A_HDR, "%-4.4s", name) + 1;
	}
	int split = c;
	put(1, c, A_BORDER, "│");
	c += 2;
	int nout = 0;
	for (int o = 0; o < NOUT; o++)
		if (C.out[o] != S_NONE) {
			c = putf(1, c, A_HDR, "%-5.5s", C.out_name[o]) + 1;
			nout++;
		}
	if (!nout) {
		put(3, 3, A_DIM, "No outputs are connected. Connect O0..O7 to gates first.");
		return;
	}
	for (int i = 2; i < SH - 2; i++) put(i, split, A_BORDER, "│");
	int vis = SH - 4;
	for (int r = 0; r < vis && top + r < nrows; r++) {
		unsigned bits = top + r;
		int outs[NOUT], row = 2 + r;
		int ok = tt_row(vars, nv, bits, outs);
		c = 2;
		for (int i = 0; i < nv; i++) {
			int v = (bits >> (nv - 1 - i)) & 1;
			c = putf(row, c, vattr(v), "%-4d", v) + 1;
		}
		c = split + 2;
		for (int o = 0; o < NOUT; o++)
			if (C.out[o] != S_NONE) {
				if (ok) c = putf(row, c, vattr(outs[o]), "%-5d", outs[o]) + 1;
				else c = put(row, c, A_ERR, "~     ");
			}
	}
	putf(SH - 2, 2, A_DIM, "rows %d-%d of %d.  Evaluated from reset state; ~ = oscillates.",
	     top + 1, top + vis < nrows ? top + vis : nrows, nrows);
}

static void draw_presets(int sel)
{
	draw_main();
	int h = NPRESETS + 4, w = 36, r = 4, c = 22;
	box(r, c, h, w, "PRESETS");
	for (int i = 0; i < NPRESETS; i++) {
		fill(r + 2 + i, c + 2, w - 4, i == sel ? A_SEL : A_NORM);
		put(r + 2 + i, c + 3, i == sel ? A_SEL : A_NORM, PRESETS[i].name);
	}
	put(r + h - 1, c + 2, A_DIM, " Enter load  Esc cancel ");
}

static void draw_menu(void)
{
	draw_main();
	box(7, 24, 9, 32, "MENU");
	put(9, 27, A_KEY, "R");  put(9, 29, A_NORM, "Resume editing");
	put(10, 27, A_KEY, "S"); put(10, 29, A_NORM, "Linux shell (busybox)");
	put(11, 27, A_KEY, "B"); put(11, 29, A_NORM, "Reboot");
	put(12, 27, A_KEY, "P"); put(12, 29, A_NORM, "Power off");
	put(14, 27, A_DIM, "Esc returns to the editor");
}

/* Single-line text entry in a small box; returns 1 if confirmed. */
static int prompt_text(const char *title, char *dst, size_t cap)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%s", dst);
	size_t maxlen = cap - 1 < sizeof buf - 1 ? cap - 1 : sizeof buf - 1;
	for (;;) {
		draw_main();
		box(9, 20, 5, 40, title);
		fill(11, 22, 36, A_SEL);
		putf(11, 22, A_SEL, "%s_", buf);
		putf(12, 22, A_DIM, "max %zu chars, Enter ok, Esc cancel", maxlen);
		scr_flush();
		int k = read_key(1000);
		size_t len = strlen(buf);
		if (k == K_ESC) return 0;
		if (k == 13 || k == 10) {
			snprintf(dst, cap, "%s", buf);
			return 1;
		}
		if ((k == 127 || k == 8) && len) buf[len - 1] = 0;
		else if (k >= 32 && k < 127 && len < maxlen) {
			buf[len] = (char)k;
			buf[len + 1] = 0;
		}
	}
}

/* ------------------------------------------------------------- editing */

static void cycle(int *v, int n, int d)
{
	*v = ((*v + d) % n + n) % n;
}

static void change_field(int d)
{
	if (focus == 1) {
		cycle(&C.out[sel_out], S_COUNT, d);
		return;
	}
	struct gate *g = &C.g[sel_gate];
	if (sel_col == 0) {
		int was = g->type;
		cycle(&g->type, T_COUNT, d);
		/* give a fresh gate sensible inputs so it does something at once */
		if (was == T_NONE && g->type != T_NONE && g->a == S_NONE) {
			g->a = S_IN0;
			if (g->b == S_NONE) g->b = S_IN0 + 1;
		}
	} else if (g->type == T_NONE) {
		msg(A_ERR, "Set a gate type first (move to TYPE and press Space).");
	} else if (sel_col == 1) {
		cycle(&g->a, S_COUNT, d);
	} else if (one_input(g->type)) {
		msg(A_ERR, "%s has only one input.", TNAME[g->type]);
	} else {
		cycle(&g->b, S_COUNT, d);
	}
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void clock_tick(void)
{
	clk_val = !clk_val;
	simulate();
}

static void load_preset(int i)
{
	PRESETS[i].load();
	sel_gate = sel_col = sel_out = 0;
	focus = 0;
	simulate();
	msg(A_HI, "Loaded preset: %s", PRESETS[i].name);
}

/* modal loops return a key to re-dispatch or K_NONE */
static int run_presets(void)
{
	static int sel = 1;
	for (;;) {
		draw_presets(sel);
		scr_flush();
		int k = read_key(1000);
		if (k == K_UP) cycle(&sel, NPRESETS, -1);
		else if (k == K_DOWN || k == 'p' || k == 'P') cycle(&sel, NPRESETS, 1);
		else if (k == 13 || k == 10 || k == ' ') {
			load_preset(sel);
			return K_NONE;
		} else if (k == K_ESC || k == 'q' || k == 'Q') return K_NONE;
	}
}

static void run_truth(void)
{
	int vars[NIN + 1], nv = tt_vars(vars), nrows = 1 << nv, top = 0, vis = SH - 4;
	for (;;) {
		draw_truth(top);
		scr_flush();
		int k = read_key(1000);
		int maxtop = nrows > vis ? nrows - vis : 0;
		if (k == K_DOWN) top++;
		else if (k == K_UP) top--;
		else if (k == K_PGDN || k == ' ') top += vis;
		else if (k == K_PGUP) top -= vis;
		else if (k == K_HOME) top = 0;
		else if (k == K_END) top = maxtop;
		else if (k != K_NONE) return;
		if (top > maxtop) top = maxtop;
		if (top < 0) top = 0;
	}
}

static int run_menu(void)
{
	for (;;) {
		draw_menu();
		scr_flush();
		switch (read_key(1000)) {
		case 'r': case 'R': case K_ESC: return -1;
		case 's': case 'S': return EXIT_SHELL;
		case 'b': case 'B': return EXIT_REBOOT;
		case 'p': case 'P': return EXIT_POWEROFF;
		}
	}
}

static void rename_here(void)
{
	if (focus == 1) {
		prompt_text("Output name", C.out_name[sel_out], sizeof C.out_name[sel_out]);
		return;
	}
	struct gate *g = &C.g[sel_gate];
	int s = sel_col == 1 ? g->a : sel_col == 2 ? g->b : S_NONE;
	if (g->type != T_NONE && s >= S_IN0 && s < S_IN0 + NIN)
		prompt_text("Input name", C.in_name[s - S_IN0], sizeof C.in_name[s - S_IN0]);
	else
		prompt_text("Circuit name", C.name, sizeof C.name);
}

/* --------------------------------------------------------------- tests */

/* Non-interactive check of the simulator against the presets. */
static int selftest(void)
{
	int fails = 0;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)
	p_full_adder();
	for (int v = 0; v < 8; v++) {
		in_val[0] = v & 1; in_val[1] = v >> 1 & 1; in_val[2] = v >> 2 & 1;
		simulate();
		int sum = in_val[0] + in_val[1] + in_val[2];
		CHECK(sim_stable && src_val(C.out[0]) == (sum & 1) && src_val(C.out[1]) == (sum >> 1), "full adder");
	}
	p_sr_latch();
	simulate();
	in_val[0] = 1; simulate(); in_val[0] = 0; simulate();
	CHECK(sim_stable && src_val(C.out[0]) == 1 && src_val(C.out[1]) == 0, "SR latch set/hold");
	in_val[1] = 1; simulate(); in_val[1] = 0; simulate();
	CHECK(sim_stable && src_val(C.out[0]) == 0 && src_val(C.out[1]) == 1, "SR latch reset/hold");
	p_d_latch();
	simulate();
	in_val[0] = 1; in_val[1] = 1; simulate(); in_val[1] = 0; in_val[0] = 0; simulate();
	CHECK(sim_stable && src_val(C.out[0]) == 1, "D latch holds 1");
	in_val[1] = 1; simulate();
	CHECK(sim_stable && src_val(C.out[0]) == 0, "D latch follows D");
	p_mux();
	for (int v = 0; v < 8; v++) {
		in_val[0] = v & 1; in_val[1] = v >> 1 & 1; in_val[2] = v >> 2 & 1;
		simulate();
		CHECK(src_val(C.out[0]) == (in_val[2] ? in_val[1] : in_val[0]), "mux");
	}
	p_nand_xor();
	for (int v = 0; v < 4; v++) {
		in_val[0] = v & 1; in_val[1] = v >> 1;
		simulate();
		CHECK(src_val(C.out[0]) == (in_val[0] ^ in_val[1]), "nand xor");
	}
	p_comparator();
	for (int v = 0; v < 16; v++) {
		for (int i = 0; i < 4; i++) in_val[i] = v >> i & 1;
		simulate();
		int a = in_val[0] | in_val[1] << 1, b = in_val[2] | in_val[3] << 1;
		CHECK(src_val(C.out[0]) == (a == b) && src_val(C.out[1]) == (a > b) && src_val(C.out[2]) == (a < b),
		      "comparator");
	}
	p_divider();
	simulate();
	int q0 = src_val(C.out[1]), toggles = 0;
	for (int i = 0; i < 8; i++) {
		int before = src_val(C.out[1]);
		clock_tick();
		CHECK(sim_stable, "divider stable");
		if (src_val(C.out[1]) != before) {
			toggles++;
			CHECK(clk_val == 1, "divider toggles on rising edge");
		}
	}
	CHECK(toggles == 4 && src_val(C.out[1]) == q0, "divider halves clock");
	clear_circuit("osc");
	C.g[0] = (struct gate){T_NOT, S_G0, S_NONE};
	simulate();
	CHECK(!sim_stable, "ring oscillator detected");
	printf(fails ? "%d check(s) failed\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
	if (!isatty(0)) {
		fprintf(stderr, "gate16: stdin must be a terminal\n");
		return 1;
	}
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
	tty_raw();
	atexit(tty_restore);

	load_preset(2);
	msg(A_HI, "Welcome! Toggle inputs with 1-8, press P for presets, H for help.");
	double next_clk = now_s();
	int rc = 0;

	for (;;) {
		double t = now_s();
		if (clk_run && t >= next_clk) {
			clock_tick();
			next_clk = t + 0.5 / CLK_HZ[clk_rate];
		}
		draw_main();
		scr_flush();
		int k = read_key(clk_run ? 20 : 250);
		if (k == K_NONE) continue;
		message[0] = 0;
		switch (k) {
		case K_UP:
			if (focus == 0) cycle(&sel_gate, NGATES, -1);
			else cycle(&sel_out, NOUT, -1);
			break;
		case K_DOWN:
			if (focus == 0) cycle(&sel_gate, NGATES, 1);
			else cycle(&sel_out, NOUT, 1);
			break;
		case K_LEFT:
			if (focus == 1) focus = 0, sel_col = 2;
			else if (sel_col > 0) sel_col--;
			break;
		case K_RIGHT:
			if (focus == 0 && sel_col < 2) sel_col++;
			else if (focus == 0) focus = 1;
			break;
		case 9:
			focus = !focus;
			break;
		case ' ': case '+': case '=': case 13: case 10:
			change_field(1);
			break;
		case '-': case '_': case 127: case 8:
			change_field(-1);
			break;
		case K_PGUP:
			change_field(-8);
			break;
		case K_PGDN:
			change_field(8);
			break;
		case K_DEL: case 'd': case 'D':
			if (focus == 0) C.g[sel_gate] = (struct gate){T_NONE, S_NONE, S_NONE};
			else C.out[sel_out] = S_NONE;
			break;
		case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
			in_val[k - '1'] = !in_val[k - '1'];
			break;
		case 'c': case 'C':
			clk_run = !clk_run;
			next_clk = now_s();
			msg(A_HI, clk_run ? "Clock running at %gHz" : "Clock stopped", CLK_HZ[clk_rate]);
			break;
		case 'k': case 'K':
			clock_tick();
			break;
		case '[':
			if (clk_rate > 0) clk_rate--;
			msg(A_HI, "Clock %gHz", CLK_HZ[clk_rate]);
			break;
		case ']':
			if (clk_rate < NCLK - 1) clk_rate++;
			msg(A_HI, "Clock %gHz", CLK_HZ[clk_rate]);
			break;
		case 't': case 'T':
			run_truth();
			break;
		case 'p': case 'P':
			run_presets();
			break;
		case 'n': case 'N':
			load_preset(0);
			msg(A_HI, "New empty circuit. Select a gate TYPE and press Space.");
			break;
		case 'r': case 'R':
			rename_here();
			break;
		case 'h': case 'H': case K_F1: case '?':
			draw_help(0);
			scr_flush();
			while (read_key(1000) == K_NONE) {}
			break;
		case 'q': case 'Q': case K_ESC: case 3:
			rc = run_menu();
			if (rc >= 0) goto out;
			break;
		}
		simulate();
	}
out:
	return rc;
}
