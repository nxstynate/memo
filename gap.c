/* See LICENSE for license details. */
/*
 * gap.c — gap buffer for text editing
 *
 * Same data structure vim uses internally.
 * Simple, predictable, no allocations during normal editing.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "memo.h"

void
gb_init(GapBuf *g, size_t cap)
{
	g->buf = calloc(cap, 1);
	if (!g->buf) {
		fprintf(stderr, "memo: malloc failed\n");
		exit(EXIT_WRITE);
	}
	g->cap = cap;
	g->gap_start = 0;
	g->gap_end = cap;
}

void
gb_free(GapBuf *g)
{
	free(g->buf);
	g->buf = NULL;
}

size_t
gb_len(const GapBuf *g)
{
	return g->cap - (g->gap_end - g->gap_start);
}

static void
gb_grow(GapBuf *g)
{
	size_t new_cap = g->cap * 2;
	char *new_buf = calloc(new_cap, 1);
	if (!new_buf) {
		fprintf(stderr, "memo: realloc failed\n");
		exit(EXIT_WRITE);
	}

	size_t tail = g->cap - g->gap_end;
	memcpy(new_buf, g->buf, g->gap_start);
	memcpy(new_buf + new_cap - tail, g->buf + g->gap_end, tail);

	g->gap_end = new_cap - tail;
	g->cap = new_cap;
	free(g->buf);
	g->buf = new_buf;
}

void
gb_move_gap(GapBuf *g, size_t pos)
{
	if (pos == g->gap_start)
		return;
	if (pos < g->gap_start) {
		size_t n = g->gap_start - pos;
		memmove(g->buf + g->gap_end - n, g->buf + pos, n);
		g->gap_start = pos;
		g->gap_end -= n;
	} else {
		size_t n = pos - g->gap_start;
		memmove(g->buf + g->gap_start, g->buf + g->gap_end, n);
		g->gap_start += n;
		g->gap_end += n;
	}
}

void
gb_insert(GapBuf *g, char c)
{
	if (g->gap_start == g->gap_end)
		gb_grow(g);
	g->buf[g->gap_start++] = c;
}

void
gb_insert_str(GapBuf *g, const char *s)
{
	while (*s)
		gb_insert(g, *s++);
}

void
gb_delete_back(GapBuf *g)
{
	if (g->gap_start > 0)
		g->gap_start--;
}

void
gb_delete_fwd(GapBuf *g)
{
	if (g->gap_end < g->cap)
		g->gap_end++;
}

char
gb_char_at(const GapBuf *g, size_t pos)
{
	if (pos < g->gap_start)
		return g->buf[pos];
	return g->buf[g->gap_end + (pos - g->gap_start)];
}

char *
gb_to_str(const GapBuf *g)
{
	size_t len = gb_len(g);
	char *s = malloc(len + 1);
	if (!s)
		return NULL;
	if (g->gap_start > 0)
		memcpy(s, g->buf, g->gap_start);
	size_t tail = g->cap - g->gap_end;
	if (tail > 0)
		memcpy(s + g->gap_start, g->buf + g->gap_end, tail);
	s[len] = '\0';
	return s;
}

void
gb_delete_line(GapBuf *g)
{
	size_t len = gb_len(g);
	size_t pos = g->gap_start;
	size_t line_start = 0;
	size_t line_end;
	size_t i;

	if (len == 0)
		return;

	/* find start of current line */
	for (i = pos; i > 0; i--) {
		if (gb_char_at(g, i - 1) == '\n') {
			line_start = i;
			break;
		}
	}

	/* find end of current line */
	line_end = len;
	for (i = pos; i < len; i++) {
		if (gb_char_at(g, i) == '\n') {
			line_end = i + 1;
			break;
		}
	}

	gb_move_gap(g, line_start);
	g->gap_end += (line_end - line_start);
}

/* ── cursor movement ─────────────────────────────────────────── */

void
gb_cursor_left(GapBuf *g)
{
	if (g->gap_start > 0) {
		g->gap_end--;
		g->buf[g->gap_end] = g->buf[g->gap_start - 1];
		g->gap_start--;
	}
}

void
gb_cursor_right(GapBuf *g)
{
	if (g->gap_end < g->cap) {
		g->buf[g->gap_start] = g->buf[g->gap_end];
		g->gap_start++;
		g->gap_end++;
	}
}

void
gb_cursor_up(GapBuf *g)
{
	size_t pos = g->gap_start;
	int col = 0;
	int prev_len = 0;
	int target, i;

	/* find current column */
	for (i = (int)pos; i > 0; i--) {
		if (gb_char_at(g, i - 1) == '\n')
			break;
		col++;
	}

	/* move to start of current line */
	for (i = 0; i < col; i++)
		gb_cursor_left(g);

	if (g->gap_start == 0) {
		/* already on first line, move back */
		for (i = 0; i < col; i++)
			gb_cursor_right(g);
		return;
	}

	gb_cursor_left(g); /* skip newline */

	/* find length of previous line */
	for (i = (int)g->gap_start; i > 0; i--) {
		if (gb_char_at(g, i - 1) == '\n')
			break;
		prev_len++;
	}

	/* move to start of prev line */
	for (i = 0; i < prev_len; i++)
		gb_cursor_left(g);

	/* move right to target column */
	target = col < prev_len ? col : prev_len;
	for (i = 0; i < target; i++)
		gb_cursor_right(g);
}

void
gb_cursor_down(GapBuf *g)
{
	size_t len = gb_len(g);
	int col = 0;
	int next_len = 0;
	int target, i;
	size_t p;

	/* find current column */
	for (p = g->gap_start; p > 0; p--) {
		if (gb_char_at(g, p - 1) == '\n')
			break;
		col++;
	}

	/* move to end of current line */
	while (g->gap_start < len && gb_char_at(g, g->gap_start) != '\n')
		gb_cursor_right(g);

	if (g->gap_start >= len)
		return; /* no next line */

	gb_cursor_right(g); /* skip newline */

	/* find length of next line */
	for (p = g->gap_start; p < len; p++) {
		if (gb_char_at(g, p) == '\n')
			break;
		next_len++;
	}

	target = col < next_len ? col : next_len;
	for (i = 0; i < target; i++)
		gb_cursor_right(g);
}

void
gb_cursor_bol(GapBuf *g)
{
	while (g->gap_start > 0 && gb_char_at(g, g->gap_start - 1) != '\n')
		gb_cursor_left(g);
}

void
gb_cursor_eol(GapBuf *g)
{
	size_t len = gb_len(g);
	while (g->gap_start < len && gb_char_at(g, g->gap_start) != '\n')
		gb_cursor_right(g);
}

void
gb_cursor_top(GapBuf *g)
{
	gb_move_gap(g, 0);
}

void
gb_cursor_bottom(GapBuf *g)
{
	gb_move_gap(g, gb_len(g));
}
