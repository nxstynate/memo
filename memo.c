/* See LICENSE for license details. */
/*
 * memo - minimal agent communication utility
 *
 * A filesystem-based message transport for human<->agent communication.
 * Two commands: ask (send to agent), tell (respond from agent).
 * Optional GDI popup UI when stdin is not a pipe.
 *
 * Build: ./build.ps1
 * Usage: echo "msg" | memo ask <agent>
 *        memo ask <agent>          # opens popup UI
 *        memo tell <agent>         # opens popup with latest inbound
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <time.h>
#include <sys/timeb.h>

#include "memo.h"
#include "config.h"

/* ── globals ─────────────────────────────────────────────────── */

static HWND    g_hwnd;
static HFONT   g_font;
static HFONT   g_font_bold;
static int     g_dpi = 96;
static int     g_mode = MODE_INSERT;
static GapBuf  g_editor;
static UICtx   g_ctx;
static int     g_result = -1;   /* -1 running, 0 send, 1 cancel */
static int     g_scroll = 0;
static int     g_cursor_visible = 1;
static int     g_line_height;

/* pending vim sequences */
static int     g_pending_g = 0;
static int     g_pending_d = 0;
static int     g_pending_colon = 0;

/* ── helpers ─────────────────────────────────────────────────── */

COLORREF
hex_to_colorref(const char *s)
{
	unsigned long val;
	unsigned int r, g, b;
	if (s[0] == '#') s++;
	val = strtoul(s, NULL, 16);
	r = (val >> 16) & 0xFF;
	g = (val >> 8) & 0xFF;
	b = val & 0xFF;
	return RGB(r, g, b);
}

static void
init_colors(void)
{
	col_bg          = hex_to_colorref(THEME_BG);
	col_fg          = hex_to_colorref(THEME_FG);
	col_border      = hex_to_colorref(THEME_BORDER);
	col_title       = hex_to_colorref(THEME_TITLE);
	col_separator   = hex_to_colorref(THEME_SEPARATOR);
	col_hint        = hex_to_colorref(THEME_HINT);
	col_cursor      = hex_to_colorref(THEME_CURSOR);
	col_readonly_fg = hex_to_colorref(THEME_READONLY_FG);
	col_mode_normal = hex_to_colorref(THEME_MODE_NORMAL);
	col_mode_insert = hex_to_colorref(THEME_MODE_INSERT);
}

static int
dpi_scale(int val)
{
	return MulDiv(val, g_dpi, 96);
}

/* ── filesystem ──────────────────────────────────────────────── */

static int
mkdirp(const char *path)
{
	char tmp[MAX_PATH_LEN];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);

	for (p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			*p = '\0';
			_mkdir(tmp);
			*p = '/';
		}
	}
	_mkdir(tmp);
	return 0;
}

static const char *
resolve_memo_dir(void)
{
	const char *d;
	static char home_memo[MAX_PATH_LEN];

	d = getenv("MEMO_DIR");
	if (d && d[0])
		return d;

	d = getenv("USERPROFILE");
	if (!d || !d[0])
		d = getenv("HOME");
	if (d && d[0]) {
		snprintf(home_memo, sizeof(home_memo), "%s/.memo", d);
		return home_memo;
	}

	return ".\\memo";
}

/*
 * Atomic write: temp file -> write -> flush -> rename
 * Windows equivalent of the Unix pattern.
 */
static int
atomic_write(const char *dir, const char *filename,
             const char *data, size_t len)
{
	char tmp_path[MAX_PATH_LEN], final_path[MAX_PATH_LEN];
	char tmp_name[MAX_PATH_LEN];
	FILE *f;

	snprintf(tmp_name, sizeof(tmp_name), ".memo_tmp_%lu",
	         (unsigned long)GetTickCount());
	snprintf(tmp_path, sizeof(tmp_path), "%s/%s", dir, tmp_name);
	snprintf(final_path, sizeof(final_path), "%s/%s", dir, filename);

	f = fopen(tmp_path, "wb");
	if (!f) {
		fprintf(stderr, "memo: cannot create %s: %lu\n",
		        tmp_path, GetLastError());
		return -1;
	}

	if (fwrite(data, 1, len, f) != len) {
		fprintf(stderr, "memo: write failed\n");
		fclose(f);
		DeleteFileA(tmp_path);
		return -1;
	}

	fflush(f);
	fclose(f);

	/* Windows rename fails if dest exists; delete first */
	DeleteFileA(final_path);
	if (!MoveFileA(tmp_path, final_path)) {
		fprintf(stderr, "memo: rename failed: %lu\n", GetLastError());
		DeleteFileA(tmp_path);
		return -1;
	}

	return 0;
}

/* ── message building ────────────────────────────────────────── */

static char *
build_message(const char *from, const char *to,
              const char *body, size_t *out_len)
{
	time_t now;
	struct tm *tm_info;
	char ts[64], *msg;
	size_t cap, body_len;
	int n;

	time(&now);
	tm_info = gmtime(&now);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm_info);

	body_len = strlen(body);
	cap = 256 + body_len;
	msg = malloc(cap);
	if (!msg) return NULL;

	n = snprintf(msg, cap,
		"from: %s\n"
		"to: %s\n"
		"at: %s\n"
		"\n"
		"%s",
		from, to, ts, body);

	if (out_len) *out_len = (size_t)n;
	return msg;
}

static void
gen_paths(const char *memo_dir, const char *agent,
          const char *suffix,
          char *dir_out, size_t dir_sz,
          char *fname_out, size_t fname_sz,
          char *full_out, size_t full_sz)
{
	struct _timeb tb;
	struct tm *tm_info;
	char datedir[32], timepart[32];

	_ftime(&tb);
	tm_info = gmtime(&tb.time);

	strftime(datedir, sizeof(datedir), "%Y-%m-%d", tm_info);
	strftime(timepart, sizeof(timepart), "%H%M%S", tm_info);

	snprintf(dir_out, dir_sz, "%s/%s/%s", memo_dir, agent, datedir);
	snprintf(fname_out, fname_sz, "%s-%03d-%s.txt",
	         timepart, (int)tb.millitm, suffix);
	snprintf(full_out, full_sz, "%s/%s", dir_out, fname_out);
}

/* ── find latest inbound message ─────────────────────────────── */

static char *
find_latest_inbound(const char *memo_dir, const char *agent)
{
	char agent_dir[MAX_PATH_LEN];
	char search[MAX_PATH_LEN];
	WIN32_FIND_DATAA fd;
	HANDLE hFind;
	char latest_date[64] = "";
	char latest_in[256] = "";
	char daydir[MAX_PATH_LEN + 128];
	char full[MAX_PATH_LEN + 384];
	FILE *f;
	long sz;
	char *content;

	snprintf(agent_dir, sizeof(agent_dir), "%s/%s", memo_dir, agent);

	/* find latest date directory */
	snprintf(search, sizeof(search), "%s/*", agent_dir);
	hFind = FindFirstFileA(search, &fd);
	if (hFind == INVALID_HANDLE_VALUE) return NULL;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (fd.cFileName[0] == '.') continue;
		if (strlen(fd.cFileName) == 10 && fd.cFileName[4] == '-') {
			if (strcmp(fd.cFileName, latest_date) > 0)
				snprintf(latest_date, sizeof(latest_date),
				         "%s", fd.cFileName);
		}
	} while (FindNextFileA(hFind, &fd));
	FindClose(hFind);

	if (!latest_date[0]) return NULL;

	/* find latest -in.txt in that day */
	snprintf(daydir, sizeof(daydir), "%s/%s", agent_dir, latest_date);
	snprintf(search, sizeof(search), "%s/*-in.txt", daydir);
	hFind = FindFirstFileA(search, &fd);
	if (hFind == INVALID_HANDLE_VALUE) return NULL;
	do {
		if (strcmp(fd.cFileName, latest_in) > 0)
			snprintf(latest_in, sizeof(latest_in), "%s", fd.cFileName);
	} while (FindNextFileA(hFind, &fd));
	FindClose(hFind);

	if (!latest_in[0]) return NULL;

	snprintf(full, sizeof(full), "%s/%s", daydir, latest_in);
	f = fopen(full, "rb");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	content = malloc(sz + 1);
	if (!content) { fclose(f); return NULL; }
	if (fread(content, 1, sz, f) != (size_t)sz) {
		free(content);
		fclose(f);
		return NULL;
	}
	content[sz] = '\0';
	fclose(f);
	return content;
}

/* ── read stdin ──────────────────────────────────────────────── */

static char *
read_stdin(size_t *out_len)
{
	size_t cap = 4096, len = 0;
	char *buf;
	int n;

	_setmode(_fileno(stdin), _O_BINARY);
	buf = malloc(cap);
	if (!buf) return NULL;

	while ((n = (int)fread(buf + len, 1, cap - len, stdin)) > 0) {
		len += n;
		if (len == cap) {
			cap *= 2;
			if (cap > MAX_BODY) {
				fprintf(stderr, "memo: input exceeds limit\n");
				free(buf);
				return NULL;
			}
			buf = realloc(buf, cap);
			if (!buf) return NULL;
		}
	}
	buf[len] = '\0';
	if (out_len) *out_len = len;
	return buf;
}

/* ── pipe mode ───────────────────────────────────────────────── */

static int
pipe_mode(const char *agent, const char *suffix,
          const char *from, const char *to)
{
	const char *memo_dir = resolve_memo_dir();
	size_t body_len, msg_len;
	char *body, *msg;
	char dir[MAX_PATH_LEN], fname[256], full[MAX_PATH_LEN];

	body = read_stdin(&body_len);
	if (!body) {
		fprintf(stderr, "memo: failed to read stdin\n");
		return EXIT_WRITE;
	}

	/* strip trailing newline */
	if (body_len > 0 && body[body_len - 1] == '\n')
		body[--body_len] = '\0';

	msg = build_message(from, to, body, &msg_len);
	free(body);
	if (!msg) {
		fprintf(stderr, "memo: failed to build message\n");
		return EXIT_WRITE;
	}

	gen_paths(memo_dir, agent, suffix,
	          dir, sizeof(dir), fname, sizeof(fname), full, sizeof(full));

	if (mkdirp(dir) != 0) {
		fprintf(stderr, "memo: cannot create directory\n");
		free(msg);
		return EXIT_FS;
	}

	if (atomic_write(dir, fname, msg, msg_len) != 0) {
		free(msg);
		return EXIT_WRITE;
	}

	printf("%s\n", full);
	free(msg);
	return EXIT_OK;
}

/* ── GDI UI ──────────────────────────────────────────────────── */

/* skip message headers to get body */
static const char *
skip_headers(const char *msg)
{
	const char *p = strstr(msg, "\n\n");
	return p ? p + 2 : msg;
}

/* compute layout regions */
typedef struct {
	RECT title;
	RECT sep1;
	RECT inbound;    /* only used in tell mode */
	RECT sep2;       /* only used in tell mode */
	RECT label;      /* "Response:" label in tell mode */
	RECT editor;
	RECT sep3;
	RECT status;
	int  win_h;
} Layout;

static Layout
compute_layout(int w, int has_inbound)
{
	Layout L;
	int y = 0;
	int pad = dpi_scale(padding);
	int lh = g_line_height;

	/* title bar */
	L.title.left = pad;
	L.title.top = pad;
	L.title.right = w - pad;
	L.title.bottom = pad + lh + dpi_scale(4);
	y = L.title.bottom;

	/* separator 1 */
	L.sep1.left = 0;
	L.sep1.top = y;
	L.sep1.right = w;
	L.sep1.bottom = y + 1;
	y += 1;

	if (has_inbound) {
		/* inbound message area (roughly 40% of remaining) */
		int remaining = dpi_scale(win_min_height) - y - lh - pad - 1;
		int inbound_h = remaining * 2 / 5;
		if (inbound_h < lh * 3) inbound_h = lh * 3;

		L.inbound.left = pad;
		L.inbound.top = y + dpi_scale(4);
		L.inbound.right = w - pad;
		L.inbound.bottom = y + inbound_h;
		y = L.inbound.bottom + dpi_scale(4);

		/* separator 2 */
		L.sep2.left = 0;
		L.sep2.top = y;
		L.sep2.right = w;
		L.sep2.bottom = y + 1;
		y += 1;

		/* "Response:" label */
		L.label.left = pad;
		L.label.top = y + dpi_scale(4);
		L.label.right = w - pad;
		L.label.bottom = y + dpi_scale(4) + lh;
		y = L.label.bottom;
	} else {
		memset(&L.inbound, 0, sizeof(RECT));
		memset(&L.sep2, 0, sizeof(RECT));
		memset(&L.label, 0, sizeof(RECT));
	}

	/* status bar at bottom */
	int status_h = lh + dpi_scale(8);

	/* separator 3 (above status) */
	/* editor fills the middle */
	L.editor.left = pad;
	L.editor.top = y + dpi_scale(4);
	L.editor.right = w - pad;
	/* bottom is computed from window height later */

	/* these get finalized after we know total height */
	L.win_h = 0; /* placeholder */
	memset(&L.sep3, 0, sizeof(RECT));
	memset(&L.status, 0, sizeof(RECT));

	return L;
}

static void
finalize_layout(Layout *L, int win_h)
{
	int pad = dpi_scale(padding);
	int lh = g_line_height;
	int status_h = lh + dpi_scale(8);

	L->win_h = win_h;

	L->status.left = pad;
	L->status.bottom = win_h - pad;
	L->status.top = L->status.bottom - status_h;
	L->status.right = L->editor.right;

	L->sep3.left = 0;
	L->sep3.top = L->status.top - 1;
	L->sep3.right = L->editor.right + pad;
	L->sep3.bottom = L->status.top;

	L->editor.bottom = L->sep3.top - dpi_scale(4);
}

/* draw a horizontal separator line */
static void
draw_sep(HDC hdc, const RECT *rc, int w)
{
	HPEN pen = CreatePen(PS_SOLID, 1, col_separator);
	HPEN old = SelectObject(hdc, pen);
	MoveToEx(hdc, 0, rc->top, NULL);
	LineTo(hdc, w, rc->top);
	SelectObject(hdc, old);
	DeleteObject(pen);
}

/* render read-only text in a rect */
static void
draw_readonly(HDC hdc, const char *text, const RECT *rc)
{
	RECT r = *rc;
	SetTextColor(hdc, col_readonly_fg);
	SelectObject(hdc, g_font);
	DrawTextA(hdc, text, -1, &r,
	          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

/* render the gap buffer editor with cursor */
static void
draw_editor(HDC hdc, const GapBuf *g, const RECT *rc)
{
	size_t len = gb_len(g);
	size_t cursor_pos = g->gap_start;
	int lh = g_line_height;
	int max_cols = (rc->right - rc->left) / (lh / 2); /* rough char width */
	int x = rc->left;
	int y = rc->top;
	int cur_x = x, cur_y = y;
	size_t i;
	char ch;
	SIZE sz;

	SelectObject(hdc, g_font);
	SetTextColor(hdc, col_fg);

	/* measure actual character width */
	GetTextExtentPoint32A(hdc, "M", 1, &sz);
	int char_w = sz.cx;
	int area_w = rc->right - rc->left;

	(void)max_cols;

	for (i = 0; i < len; i++) {
		ch = gb_char_at(g, i);

		if (i == cursor_pos) {
			cur_x = x;
			cur_y = y;
		}

		if (ch == '\n') {
			x = rc->left;
			y += lh;
			continue;
		}

		if (y >= rc->top - lh && y < rc->bottom) {
			char buf[2] = { ch, '\0' };
			TextOutA(hdc, x, y, buf, 1);
		}
		x += char_w;

		/* wrap */
		if (x + char_w > rc->right) {
			x = rc->left;
			y += lh;
		}
	}

	/* cursor at end */
	if (cursor_pos >= len) {
		cur_x = x;
		cur_y = y;
	}

	/* draw cursor */
	if (g_cursor_visible && cur_y >= rc->top && cur_y < rc->bottom) {
		if (g_mode == MODE_INSERT) {
			/* thin line cursor */
			HPEN cpen = CreatePen(PS_SOLID, dpi_scale(2), col_cursor);
			HPEN old = SelectObject(hdc, cpen);
			MoveToEx(hdc, cur_x, cur_y, NULL);
			LineTo(hdc, cur_x, cur_y + lh);
			SelectObject(hdc, old);
			DeleteObject(cpen);
		} else {
			/* block cursor */
			RECT cr = { cur_x, cur_y, cur_x + char_w, cur_y + lh };
			HBRUSH cb = CreateSolidBrush(col_cursor);
			FillRect(hdc, &cr, cb);
			DeleteObject(cb);
			/* draw char under cursor in bg color */
			if (cursor_pos < len) {
				char buf[2] = { gb_char_at(g, cursor_pos), '\0' };
				SetTextColor(hdc, col_bg);
				TextOutA(hdc, cur_x, cur_y, buf, 1);
				SetTextColor(hdc, col_fg);
			}
		}
	}
}

static Layout g_layout;

static void
paint(HWND hwnd)
{
	PAINTSTRUCT ps;
	HDC hdc_win = BeginPaint(hwnd, &ps);
	RECT rc;
	GetClientRect(hwnd, &rc);
	int w = rc.right, h = rc.bottom;

	/* double buffer */
	HDC hdc = CreateCompatibleDC(hdc_win);
	HBITMAP bmp = CreateCompatibleBitmap(hdc_win, w, h);
	SelectObject(hdc, bmp);

	/* background */
	HBRUSH bg = CreateSolidBrush(col_bg);
	FillRect(hdc, &rc, bg);
	DeleteObject(bg);

	SetBkMode(hdc, TRANSPARENT);

	/* title */
	SelectObject(hdc, g_font_bold);
	SetTextColor(hdc, col_title);
	{
		char title[256];
		const char *arrow = g_ctx.is_tell
			? "memo \xe2\x86\x90 " : "memo \xe2\x86\x92 ";
		snprintf(title, sizeof(title), "%s%s", arrow, g_ctx.agent);
		TextOutA(hdc, g_layout.title.left, g_layout.title.top,
		         title, (int)strlen(title));
	}

	/* separator under title */
	draw_sep(hdc, &g_layout.sep1, w);

	/* tell mode: show inbound + response label */
	if (g_ctx.is_tell && g_ctx.inbound) {
		const char *body = skip_headers(g_ctx.inbound);
		draw_readonly(hdc, body, &g_layout.inbound);
		draw_sep(hdc, &g_layout.sep2, w);

		SelectObject(hdc, g_font);
		SetTextColor(hdc, col_hint);
		TextOutA(hdc, g_layout.label.left, g_layout.label.top,
		         "Response:", 9);
	}

	/* editor */
	draw_editor(hdc, &g_editor, &g_layout.editor);

	/* separator above status */
	draw_sep(hdc, &g_layout.sep3, w);

	/* status bar */
	SelectObject(hdc, g_font);
	{
		const char *mode_str;
		COLORREF mode_col;
		const char *hint;

		if (g_mode == MODE_INSERT) {
			mode_str = "-- INSERT --";
			mode_col = col_mode_insert;
			hint = "[Esc] Normal  [Ctrl+S] Send";
		} else {
			mode_str = "-- NORMAL --";
			mode_col = col_mode_normal;
			hint = "[i] Insert  [:w] Send  [:q] Cancel";
		}

		SetTextColor(hdc, mode_col);
		TextOutA(hdc, g_layout.status.left, g_layout.status.top,
		         mode_str, (int)strlen(mode_str));

		/* right-aligned hint */
		SIZE sz;
		GetTextExtentPoint32A(hdc, hint, (int)strlen(hint), &sz);
		SetTextColor(hdc, col_hint);
		TextOutA(hdc, g_layout.status.right - sz.cx,
		         g_layout.status.top, hint, (int)strlen(hint));
	}

	/* border */
	{
		HPEN bpen = CreatePen(PS_SOLID, dpi_scale(border_width), col_border);
		HPEN old = SelectObject(hdc, bpen);
		HBRUSH null_brush = (HBRUSH)GetStockObject(NULL_BRUSH);
		HBRUSH old_brush = SelectObject(hdc, null_brush);
		Rectangle(hdc, 0, 0, w, h);
		SelectObject(hdc, old);
		SelectObject(hdc, old_brush);
		DeleteObject(bpen);
	}

	/* flip */
	BitBlt(hdc_win, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
	DeleteObject(bmp);
	DeleteDC(hdc);
	EndPaint(hwnd, &ps);
}

/* ── send the message ────────────────────────────────────────── */

static int
send_message(void)
{
	char *body = gb_to_str(&g_editor);
	const char *memo_dir;
	size_t msg_len;
	char *msg;
	char dir[MAX_PATH_LEN], fname[256], full[MAX_PATH_LEN];

	if (!body) return EXIT_WRITE;
	if (strlen(body) == 0) { free(body); return EXIT_OK; }

	memo_dir = resolve_memo_dir();
	msg = build_message(g_ctx.from, g_ctx.to, body, &msg_len);
	free(body);
	if (!msg) return EXIT_WRITE;

	gen_paths(memo_dir, g_ctx.agent, g_ctx.suffix,
	          dir, sizeof(dir), fname, sizeof(fname), full, sizeof(full));

	if (mkdirp(dir) != 0) {
		free(msg);
		return EXIT_FS;
	}

	if (atomic_write(dir, fname, msg, msg_len) != 0) {
		free(msg);
		return EXIT_WRITE;
	}

	printf("%s\n", full);
	free(msg);
	return EXIT_OK;
}

/* ── keyboard handling ───────────────────────────────────────── */

static void
handle_insert_key(WPARAM wParam)
{
	switch (wParam) {
	case VK_ESCAPE:
		if (vim_mode) {
			g_mode = MODE_NORMAL;
			InvalidateRect(g_hwnd, NULL, FALSE);
		}
		break;
	case VK_BACK:
		gb_delete_back(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_DELETE:
		gb_delete_fwd(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_LEFT:
		gb_cursor_left(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_RIGHT:
		gb_cursor_right(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_UP:
		gb_cursor_up(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_DOWN:
		gb_cursor_down(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_HOME:
		gb_cursor_bol(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_END:
		gb_cursor_eol(&g_editor);
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	case VK_RETURN:
		gb_insert(&g_editor, '\n');
		InvalidateRect(g_hwnd, NULL, FALSE);
		break;
	}
}

static void
handle_normal_key(WPARAM wParam)
{
	/* handle pending sequences */
	if (g_pending_colon) {
		g_pending_colon = 0;
		if (wParam == 'W' || wParam == 'w') {
			g_result = 0;
			PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		} else if (wParam == 'Q' || wParam == 'q') {
			g_result = 1;
			PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		}
		return;
	}
	if (g_pending_g) {
		g_pending_g = 0;
		if (wParam == 'G' || wParam == 'g') {
			gb_cursor_top(&g_editor);
			InvalidateRect(g_hwnd, NULL, FALSE);
		}
		return;
	}
	if (g_pending_d) {
		g_pending_d = 0;
		if (wParam == 'D' || wParam == 'd') {
			gb_delete_line(&g_editor);
			InvalidateRect(g_hwnd, NULL, FALSE);
		}
		return;
	}
}

static void
handle_normal_char(char ch)
{
	if (g_pending_colon || g_pending_g || g_pending_d)
		return; /* handled in handle_normal_key via WM_KEYDOWN */

	switch (ch) {
	case 'i':
		g_mode = MODE_INSERT;
		break;
	case 'a':
		gb_cursor_right(&g_editor);
		g_mode = MODE_INSERT;
		break;
	case 'A':
		gb_cursor_eol(&g_editor);
		g_mode = MODE_INSERT;
		break;
	case 'I':
		gb_cursor_bol(&g_editor);
		g_mode = MODE_INSERT;
		break;
	case 'o':
		gb_cursor_eol(&g_editor);
		gb_insert(&g_editor, '\n');
		g_mode = MODE_INSERT;
		break;
	case 'O':
		gb_cursor_bol(&g_editor);
		gb_insert(&g_editor, '\n');
		gb_cursor_left(&g_editor);
		g_mode = MODE_INSERT;
		break;
	case 'h':
		gb_cursor_left(&g_editor);
		break;
	case 'l':
		gb_cursor_right(&g_editor);
		break;
	case 'j':
		gb_cursor_down(&g_editor);
		break;
	case 'k':
		gb_cursor_up(&g_editor);
		break;
	case '0':
		gb_cursor_bol(&g_editor);
		break;
	case '$':
		gb_cursor_eol(&g_editor);
		break;
	case 'G':
		gb_cursor_bottom(&g_editor);
		break;
	case 'g':
		g_pending_g = 1;
		return; /* don't invalidate yet */
	case 'x':
		gb_delete_fwd(&g_editor);
		break;
	case 'd':
		g_pending_d = 1;
		return;
	case ':':
		g_pending_colon = 1;
		return;
	case 27: /* Esc in normal = cancel */
		g_result = 1;
		PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		return;
	default:
		return;
	}
	InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK
wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_PAINT:
		paint(hwnd);
		return 0;

	case WM_KEYDOWN:
		/* Ctrl+S = send in any mode */
		if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
			g_result = 0;
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return 0;
		}
		/* Ctrl+Q = cancel in any mode */
		if (wParam == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000)) {
			g_result = 1;
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return 0;
		}

		if (g_mode == MODE_INSERT)
			handle_insert_key(wParam);
		else
			handle_normal_key(wParam);
		return 0;

	case WM_CHAR:
		if (g_mode == MODE_INSERT) {
			char ch = (char)wParam;
			/* Ctrl+S and Enter are handled in WM_KEYDOWN */
			if (ch == 19) return 0; /* Ctrl+S */
			if (ch == 17) return 0; /* Ctrl+Q */
			if (ch == '\r' || ch == '\n') return 0; /* handled above */
			if (ch == '\b') return 0; /* handled in WM_KEYDOWN */
			if (ch == 27) return 0; /* Esc handled above */
			if (ch >= 32) {
				gb_insert(&g_editor, ch);
				InvalidateRect(hwnd, NULL, FALSE);
			}
		} else {
			handle_normal_char((char)wParam);
		}
		return 0;

	case WM_TIMER:
		if (wParam == 1 && cursor_blink_ms > 0) {
			g_cursor_visible = !g_cursor_visible;
			InvalidateRect(hwnd, NULL, FALSE);
		}
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, 1);
		PostQuitMessage(0);
		return 0;

	case WM_ERASEBKGND:
		return 1; /* we handle it in WM_PAINT */
	}

	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ── UI entry ────────────────────────────────────────────────── */

static int
ui_run(UICtx *ctx)
{
	WNDCLASSEXA wc = {0};
	MSG msg;
	int screen_w, screen_h, wx, wy, ww, wh;
	HDC hdc;
	TEXTMETRICA tm;
	HMONITOR hMon;
	MONITORINFO mi;
	POINT pt;
	DWORD style, exstyle;

	g_ctx = *ctx;
	init_colors();

	/* DPI awareness */
	SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

	/* get DPI from monitor under cursor */
	GetCursorPos(&pt);
	hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	{
		UINT dx, dy;
		if (GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK)
			g_dpi = (int)dx;
	}
	mi.cbSize = sizeof(mi);
	GetMonitorInfoA(hMon, &mi);
	screen_w = mi.rcWork.right - mi.rcWork.left;
	screen_h = mi.rcWork.bottom - mi.rcWork.top;

	/* register class */
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = "memo";
	RegisterClassExA(&wc);

	/* create fonts */
	g_font = CreateFontA(
		-MulDiv(fontsize, g_dpi, 72), 0, 0, 0,
		FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, font);

	g_font_bold = CreateFontA(
		-MulDiv(fontsize, g_dpi, 72), 0, 0, 0,
		FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, font);

	/* measure line height */
	hdc = GetDC(NULL);
	SelectObject(hdc, g_font);
	GetTextMetricsA(hdc, &tm);
	g_line_height = tm.tmHeight + tm.tmExternalLeading + dpi_scale(line_spacing);
	ReleaseDC(NULL, hdc);

	/* compute layout */
	ww = dpi_scale(win_width);
	if (ww > screen_w - 40) ww = screen_w - 40;

	g_layout = compute_layout(ww, ctx->is_tell && ctx->inbound != NULL);

	wh = dpi_scale(win_min_height);
	if (ctx->is_tell && ctx->inbound)
		wh = (int)(wh * 1.4); /* taller for tell mode */
	if (wh > screen_h - 40) wh = screen_h - 40;

	finalize_layout(&g_layout, wh);

	/* center on screen */
	wx = mi.rcWork.left + (screen_w - ww) / 2;
	wy = mi.rcWork.top + (screen_h - wh) / 2;

	/* borderless popup */
	style = WS_POPUP;
	exstyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

	/* init editor */
	gb_init(&g_editor, 1024);
	g_mode = start_insert ? MODE_INSERT : MODE_NORMAL;

	g_hwnd = CreateWindowExA(
		exstyle, "memo", "memo", style,
		wx, wy, ww, wh,
		NULL, NULL, GetModuleHandleA(NULL), NULL);

	if (!g_hwnd) {
		fprintf(stderr, "memo: CreateWindowEx failed: %lu\n", GetLastError());
		return EXIT_WRITE;
	}

	ShowWindow(g_hwnd, SW_SHOW);
	SetForegroundWindow(g_hwnd);
	SetFocus(g_hwnd);

	/* cursor blink timer */
	if (cursor_blink_ms > 0)
		SetTimer(g_hwnd, 1, cursor_blink_ms, NULL);

	/* message loop */
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	/* cleanup */
	DeleteObject(g_font);
	DeleteObject(g_font_bold);

	int rc = EXIT_OK;
	if (g_result == 0) {
		rc = send_message();
	}

	gb_free(&g_editor);
	free(g_ctx.inbound);

	return rc;
}

/* ── usage ───────────────────────────────────────────────────── */

static void
usage(void)
{
	fprintf(stderr,
		"usage: memo ask <agent>    send message to agent\n"
		"       memo tell <agent>   respond to agent\n"
		"\n"
		"Reads message body from stdin, or opens popup UI if stdin is a terminal.\n"
		"Writes created file path to stdout.\n"
		"\n"
		"Flags:\n"
		"  --ui    force popup UI even when stdin is a pipe\n"
		"\n"
		"Environment:\n"
		"  MEMO_DIR    message store (default: %%USERPROFILE%%/.memo)\n"
		"  MEMO_USER   sender identity (default: %%USERNAME%%)\n"
	);
}

/* ── main ────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
	const char *cmd, *agent;
	const char *user;
	const char *from, *to, *suffix;
	int force_ui = 0;
	int is_ask, is_tell;
	int use_ui;
	int i;

	if (argc < 3) {
		usage();
		return EXIT_USAGE;
	}

	cmd = argv[1];
	agent = argv[2];

	for (i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--ui") == 0)
			force_ui = 1;
	}

	is_ask = (strcmp(cmd, "ask") == 0);
	is_tell = (strcmp(cmd, "tell") == 0);

	if (!is_ask && !is_tell) {
		fprintf(stderr, "memo: unknown command '%s'\n", cmd);
		usage();
		return EXIT_USAGE;
	}

	user = getenv("MEMO_USER");
	if (!user || !user[0]) user = getenv("USERNAME");
	if (!user || !user[0]) user = getenv("USER");
	if (!user || !user[0]) user = "unknown";

	if (is_ask) {
		from = user;
		to = agent;
		suffix = "in";
	} else {
		from = agent;
		to = user;
		suffix = "out";
	}

	/* detect if stdin is a pipe or console */
	{
		HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
		DWORD mode;
		int is_console = GetConsoleMode(hStdin, &mode);
		use_ui = force_ui || is_console;
	}

	if (use_ui) {
		UICtx ctx = {0};
		ctx.agent = agent;
		ctx.suffix = suffix;
		ctx.from = from;
		ctx.to = to;
		ctx.inbound = NULL;
		ctx.is_tell = is_tell;

		if (is_tell)
			ctx.inbound = find_latest_inbound(resolve_memo_dir(), agent);

		return ui_run(&ctx);
	}

	return pipe_mode(agent, suffix, from, to);
}
