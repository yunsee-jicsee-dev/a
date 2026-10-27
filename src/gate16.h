/* Shared between the text-mode editor (gate16.c) and the graphical board (board.c). */
#ifndef GATE16_H
#define GATE16_H

/* exit codes, interpreted by the boot /init script */
#define EXIT_SHELL 10
#define EXIT_REBOOT 11
#define EXIT_POWEROFF 12
#define EXIT_TUI 13   /* restart in the text-mode editor */
#define EXIT_BOARD 14 /* restart in the graphical board */

enum {
	K_NONE = 0, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN,
	K_HOME, K_END, K_DEL, K_INS, K_F1, K_F2, K_F3, K_F4, K_F5, K_ESC
};

extern int ttyfd;
void tty_raw(void);
void tty_restore(void);
int read_key(int timeout_ms);

/* graphical board: returns an exit code, or -1 if there is no usable framebuffer */
int board_main(void);
int board_selftest(void);

#endif
