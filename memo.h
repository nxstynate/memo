/* See LICENSE for license details. */

#ifndef MEMO_H
#define MEMO_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ── limits ──────────────────────────────────────────────────── */

#define MAX_BODY       (1 << 20)   /* 1 MiB max message */
#define MAX_PATH_LEN   4096
#define MAX_TITLE      256

/* ── exit codes ──────────────────────────────────────────────── */

#define EXIT_OK        0
#define EXIT_USAGE     1
#define EXIT_FS        2
#define EXIT_WRITE     3

/* ── gap buffer ──────────────────────────────────────────────── */

typedef struct {
	char  *buf;
	size_t cap;
	size_t gap_start;
	size_t gap_end;
} GapBuf;

void   gb_init(GapBuf *g, size_t cap);
void   gb_free(GapBuf *g);
size_t gb_len(const GapBuf *g);
void   gb_insert(GapBuf *g, char c);
void   gb_insert_str(GapBuf *g, const char *s);
void   gb_delete_back(GapBuf *g);
void   gb_delete_fwd(GapBuf *g);
void   gb_delete_line(GapBuf *g);
char   gb_char_at(const GapBuf *g, size_t pos);
char  *gb_to_str(const GapBuf *g);
void   gb_move_gap(GapBuf *g, size_t pos);

/* cursor movement */
void   gb_cursor_left(GapBuf *g);
void   gb_cursor_right(GapBuf *g);
void   gb_cursor_up(GapBuf *g);
void   gb_cursor_down(GapBuf *g);
void   gb_cursor_bol(GapBuf *g);
void   gb_cursor_eol(GapBuf *g);
void   gb_cursor_top(GapBuf *g);
void   gb_cursor_bottom(GapBuf *g);

/* ── modes ───────────────────────────────────────────────────── */

enum { MODE_INSERT, MODE_NORMAL };

/* ── UI context ──────────────────────────────────────────────── */

typedef struct {
	const char *agent;
	const char *suffix;     /* "in" or "out" */
	const char *from;
	const char *to;
	char       *inbound;    /* NULL for ask, message text for tell */
	int         is_tell;
} UICtx;

/* ── color helper ────────────────────────────────────────────── */

COLORREF hex_to_colorref(const char *s);

#endif /* MEMO_H */
