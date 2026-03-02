/* See LICENSE for license details. */

/*
 * memo - minimal agent communication utility
 *
 * Copy this file to config.h and edit to taste.
 * config.h is .gitignored; config.def.h is the upstream reference.
 */

/* ── appearance ──────────────────────────────────────────────── */

static const char *font       = "Hack Nerd Font Mono";
static int fontsize           = 16;

/* window geometry */
static int win_width          = 600;       /* popup width in pixels */
static int win_min_height     = 300;       /* minimum popup height */
static int padding            = 12;        /* inner padding */
static int border_width       = 2;         /* window border */
static int line_spacing       = 4;         /* extra space between lines */

/* ── colors — #RRGGBB hex ───────────────────────────────────── */

/* resolved at runtime via init_colors() */
static COLORREF col_bg          = 0;
static COLORREF col_fg          = 0;
static COLORREF col_border      = 0;
static COLORREF col_title       = 0;
static COLORREF col_separator   = 0;
static COLORREF col_hint        = 0;
static COLORREF col_cursor      = 0;
static COLORREF col_readonly_fg = 0;
static COLORREF col_mode_normal = 0;
static COLORREF col_mode_insert = 0;

/* gruvbox material */
#define THEME_BG          "#282828"
#define THEME_FG          "#d4be98"
#define THEME_BORDER      "#3c3836"
#define THEME_TITLE       "#89b482"
#define THEME_SEPARATOR   "#504945"
#define THEME_HINT        "#7c6f64"
#define THEME_CURSOR      "#d4be98"
#define THEME_READONLY_FG "#a89984"
#define THEME_MODE_NORMAL "#d8a657"
#define THEME_MODE_INSERT "#89b482"

/* ── behavior ────────────────────────────────────────────────── */

/* start in insert mode (1) or normal mode (0) */
static int start_insert       = 1;

/* duration in ms to show cursor blink (0 = no blink) */
static int cursor_blink_ms    = 500;

/* vim mode: enable vim keybindings in normal mode */
static int vim_mode           = 1;
