/*
 * Headless test harness for the scrollback-reflow merge in st.
 *
 * Includes st.c directly so the static Term/Scrollback state is
 * reachable, stubs the win.h layer, and drives the terminal core with
 * twrite()/tresize() the same way x.c would.
 *
 * Build (from ~/.local/src/st):
 *   cc -std=c99 -D_XOPEN_SOURCE=600 -g -fsanitize=address,undefined \
 *      -I. /tmp/st-reflow-tests/test_st.c boxdraw.c -o /tmp/st-reflow-tests/test_st -lm
 */
#include "st.c"

#include <locale.h>
#include <fcntl.h>

/* boxdraw.c stubs (the real ones drag in Xft) */
int isboxdraw(Rune u) { return 0; }
ushort boxdrawindex(const Glyph *g) { return 0; }

/* ---- config globals normally provided by config.h via x.c ---- */
char *utmp = NULL;
char *scroll = NULL;
char *stty_args = "stty raw pass8 nl -echo -iexten -cstopb 38400";
char *vtiden = "\033[?6c";
wchar_t *worddelimiters = L" ";
int allowaltscreen = 1;
int allowwindowops = 0;
char *termname = "st-256color";
unsigned int tabspaces = 8;
unsigned int defaultfg = 259;
unsigned int defaultbg = 258;
unsigned int defaultcs = 256;
const int boxdraw = 1, boxdraw_bold = 0, boxdraw_braille = 0;
unsigned int scrollback_lines = 50; /* small on purpose: exercises ring wrap */

/* ---- win.h stubs ---- */
void xbell(void) {}
void xclipcopy(void) {}
void xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og, Line l, int len) {}
void xdrawline(Line line, int x1, int y, int x2) {}
void xfinishdraw(void) {}
void xloadcols(void) {}
int xsetcolorname(int x, const char *name) { return 0; }
int xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b) { return 0; }
void xseticontitle(char *p) {}
void xsettitle(char *p) {}
int xsetcursor(int cursor) { return 0; }
void xsetmode(int set, unsigned int flags) {}
void xsetpointermotion(int set) {}
void xsetsel(char *str) {}
int xstartdraw(void) { return 1; }
void xximspot(int x, int y) {}

/* ---- tiny test framework ---- */
static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __func__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static void
feed(const char *s)
{
	twrite(s, strlen(s), 0);
}

/* text of a screen row, trailing blanks trimmed */
static char *
rowtext(int y)
{
	static char buf[4096];
	Line l = term.line[y];
	int len = tlinelen(l), i, n = 0;

	for (i = 0; i < len && n < (int)sizeof(buf) - 5; i++)
		n += utf8encode(l[i].u, buf + n);
	buf[n] = '\0';
	return buf;
}

/* text of the row the *viewer* sees (through renderline) */
static char *
viewtext(int y)
{
	static char buf[4096];
	Line l = renderline(y);
	int len = tlinelen(l), i, n = 0;

	for (i = 0; i < len && n < (int)sizeof(buf) - 5; i++)
		n += utf8encode(l[i].u, buf + n);
	buf[n] = '\0';
	return buf;
}

/*
 * Flatten scrollback + live screen into one logical text stream,
 * joining ATTR_WRAP continuations - mirrors what externalpipe emits
 * and what a human reads top to bottom. Caller frees.
 */
static char *
dumpall(void)
{
	size_t cap = 65536, n = 0;
	char *out = malloc(cap);
	int i, x, len, wrapped;
	Line l;

	for (i = 0; i < sb.len + term.row; i++) {
		l = (i < sb.len) ? sb_get(i) : term.line[i - sb.len];
		len = tlinelen(l);
		wrapped = len > 0 && (l[term.col - 1].mode & ATTR_WRAP);
		for (x = 0; x < len; x++) {
			if (n + UTF_SIZ + 2 > cap)
				out = realloc(out, cap *= 2);
			n += utf8encode(l[x].u, out + n);
		}
		if (!wrapped)
			out[n++] = '\n';
	}
	/* trim trailing blank lines so comparisons are stable */
	while (n > 1 && out[n-1] == '\n' && out[n-2] == '\n')
		n--;
	out[n] = '\0';
	return out;
}

static void
reset_term(int col, int row)
{
	/* fresh terminal for each test */
	if (term.line) {
		int i;
		for (i = 0; i < term.row; i++) {
			free(term.line[i]);
			free(term.alt[i]);
		}
		free(term.line);
		free(term.alt);
		free(term.dirty);
		free(term.tabs);
		term.line = term.alt = NULL;
		term.dirty = NULL;
		term.tabs = NULL;
	}
	sb_clear();
	free(sb.buf);
	sb.buf = NULL;
	memset(&sel, 0, sizeof(sel));
	sel.ob.x = -1;
	tnew(col, row);
}

/* ---- tests ---- */

static void
test_basic_write(void)
{
	reset_term(80, 24);
	feed("hello world\r\n");
	feed("second line\r\n");
	CHECK(strcmp(rowtext(0), "hello world") == 0, "row0='%s'", rowtext(0));
	CHECK(strcmp(rowtext(1), "second line") == 0, "row1='%s'", rowtext(1));
	CHECK(term.c.y == 2, "cursor y=%d", term.c.y);
	CHECK(sb.len == 0, "sb.len=%d", sb.len);
}

static void
test_scrollback_fills(void)
{
	char buf[64], want[64];
	int i;

	reset_term(80, 24);
	for (i = 1; i <= 100; i++) {
		snprintf(buf, sizeof(buf), "line-%03d\r\n", i);
		feed(buf);
	}
	/* 100 lines + prompt row: rows show 78..100 + cursor row */
	snprintf(want, sizeof(want), "line-%03d", 78);
	CHECK(strcmp(rowtext(0), want) == 0, "top row='%s' want '%s'", rowtext(0), want);
	CHECK(sb.len == 50, "sb.len=%d (cap 50)", sb.len);
	/* ring wrapped: oldest retained is line 28 (77 scrolled, cap 50) */
	{
		Line l = sb_get(0);
		char got[64]; int x, n = 0;
		for (x = 0; x < tlinelen(l); x++)
			n += utf8encode(l[x].u, got + n);
		got[n] = 0;
		CHECK(strcmp(got, "line-028") == 0, "oldest sb='%s'", got);
	}
}

static void
test_kscroll_view(void)
{
	char buf[64];
	int i;
	Arg a;

	reset_term(80, 24);
	for (i = 1; i <= 60; i++) {
		snprintf(buf, sizeof(buf), "line-%03d\r\n", i);
		feed(buf);
	}
	/* 37 lines scrolled into history; screen top = line-038 */
	CHECK(strcmp(viewtext(0), "line-038") == 0, "view top='%s'", viewtext(0));

	a.i = 10;
	kscrollup(&a);
	CHECK(sb.view_offset == 10, "view_offset=%d", sb.view_offset);
	CHECK(strcmp(viewtext(0), "line-028") == 0, "scrolled view top='%s'", viewtext(0));

	a.i = -1; /* page */
	kscrollup(&a);
	CHECK(sb.view_offset == 34, "view_offset=%d (10+24)", sb.view_offset);

	/* clamped at top: 37 lines of history */
	a.i = 1000000;
	kscrollup(&a);
	CHECK(sb.view_offset == sb.len, "view_offset=%d sb.len=%d", sb.view_offset, sb.len);
	CHECK(strcmp(viewtext(0), "line-001") == 0, "top of history='%s'", viewtext(0));

	a.i = 1000000;
	kscrolldown(&a);
	CHECK(sb.view_offset == 0, "back to live, view_offset=%d", sb.view_offset);
	CHECK(strcmp(viewtext(0), "line-038") == 0, "live view top='%s'", viewtext(0));

	/* user input snaps view to bottom; terminal replies must not */
	a.i = 5;
	kscrollup(&a);
	ttywrite("\033[0n", 4, 0); /* reply: no snap */
	CHECK(sb.view_offset == 5, "reply must not snap, view_offset=%d", sb.view_offset);
	ttywrite("x", 1, 1); /* user input: snap */
	CHECK(sb.view_offset == 0, "user input snaps, view_offset=%d", sb.view_offset);
}

static void
test_width_shrink_grow_reflow(void)
{
	char *before, *after;
	int i;
	char buf[256];

	reset_term(80, 24);
	/* mix: short lines and a 200-char line that wraps at 80 */
	feed("short one\r\n");
	for (i = 0; i < 200; i++)
		buf[i] = 'a' + (i % 26);
	buf[200] = '\0';
	feed(buf);
	feed("\r\n");
	feed("tail line\r\n");
	before = dumpall();

	tresize(40, 24); /* shrink: 200-char line now wraps at 40 */
	after = dumpall();
	CHECK(strcmp(before, after) == 0,
	      "shrink lost content:\n--- before\n%s\n--- after\n%s", before, after);
	free(after);

	tresize(120, 24); /* grow past original: line should rejoin */
	after = dumpall();
	CHECK(strcmp(before, after) == 0,
	      "grow lost content:\n--- before\n%s\n--- after\n%s", before, after);
	/* the 200-char line must now occupy ceil(200/120)=2 rows, not 5 */
	{
		int rows_with_wrap = 0;
		for (i = 0; i < sb.len; i++)
			if (sb_get(i)[term.col-1].mode & ATTR_WRAP)
				rows_with_wrap++;
		for (i = 0; i < term.row; i++)
			if (term.line[i][term.col-1].mode & ATTR_WRAP)
				rows_with_wrap++;
		CHECK(rows_with_wrap == 1, "expected exactly 1 wrap row at 120 cols, got %d",
		      rows_with_wrap);
	}
	free(after);
	free(before);
}

static void
test_width_cycle_restores(void)
{
	char *before, *after;
	char buf[64];
	int i;

	reset_term(80, 24);
	for (i = 1; i <= 40; i++) {
		snprintf(buf, sizeof(buf), "row %02d filler text\r\n", i);
		feed(buf);
	}
	before = dumpall();
	tresize(37, 24);
	tresize(80, 24);
	after = dumpall();
	CHECK(strcmp(before, after) == 0,
	      "A->B->A width cycle:\n--- before\n%s\n--- after\n%s", before, after);
	free(before);
	free(after);
}

static void
test_height_cycle_restores(void)
{
	char *before, *after;
	char buf[64];
	int i;

	reset_term(80, 24);
	for (i = 1; i <= 30; i++) {
		snprintf(buf, sizeof(buf), "h-%02d\r\n", i);
		feed(buf);
	}
	before = dumpall();
	tresize(80, 10); /* shrink height: top rows pushed to scrollback */
	after = dumpall();
	CHECK(strcmp(before, after) == 0,
	      "height shrink:\n--- before\n%s\n--- after\n%s", before, after);
	free(after);
	tresize(80, 24); /* grow back: rows pop out of scrollback */
	after = dumpall();
	CHECK(strcmp(before, after) == 0,
	      "height grow:\n--- before\n%s\n--- after\n%s", before, after);
	/* cursor should sit on the row after "h-30" */
	CHECK(strcmp(rowtext(term.c.y - 1), "h-30") == 0,
	      "cursor row-1='%s' (c.y=%d)", rowtext(term.c.y - 1), term.c.y);
	free(before);
	free(after);
}

static void
test_altscreen(void)
{
	char buf[64];
	int i;

	reset_term(80, 24);
	for (i = 1; i <= 30; i++) {
		snprintf(buf, sizeof(buf), "main-%02d\r\n", i);
		feed(buf);
	}
	feed("\033[?1049h"); /* enter alt screen */
	CHECK(IS_SET(MODE_ALTSCREEN), "altscreen on");
	feed("\033[H"); /* apps home the cursor themselves after smcup */
	feed("alt content\r\n");
	CHECK(strcmp(rowtext(0), "alt content") == 0, "alt row0='%s'", rowtext(0));

	/* alt screen writes must not touch scrollback */
	{
		int before_len = sb.len;
		for (i = 0; i < 40; i++)
			feed("spam\r\n");
		CHECK(sb.len == before_len, "alt spam fed history: %d -> %d",
		      before_len, sb.len);
	}

	/* kscroll is a no-op on the alt screen */
	{
		Arg a = { .i = 5 };
		kscrollup(&a);
		CHECK(sb.view_offset == 0, "kscrollup on alt: view_offset=%d",
		      sb.view_offset);
	}

	/* resize on the alt screen, then leave: main screen must survive */
	tresize(70, 20);
	tresize(80, 24);
	feed("\033[?1049l"); /* leave alt screen */
	CHECK(!IS_SET(MODE_ALTSCREEN), "altscreen off");
	{
		char *all = dumpall();
		CHECK(strstr(all, "main-01") != NULL, "main-01 missing after alt cycle");
		CHECK(strstr(all, "main-30") != NULL, "main-30 missing after alt cycle");
		CHECK(strstr(all, "alt content") == NULL, "alt content leaked to main");
		free(all);
	}
}

static void
test_clear_scrollback(void)
{
	char buf[64];
	int i;

	reset_term(80, 24);
	for (i = 1; i <= 60; i++) {
		snprintf(buf, sizeof(buf), "line-%03d\r\n", i);
		feed(buf);
	}
	CHECK(sb.len > 0, "have history");
	feed("\033[3J"); /* ED 3: clear scrollback */
	CHECK(sb.len == 0, "ED3 left sb.len=%d", sb.len);
	/* screen content stays */
	CHECK(strcmp(rowtext(0), "line-038") == 0, "screen after ED3: '%s'", rowtext(0));
}

static void
test_selection_in_history(void)
{
	char buf[64], *s;
	int i;
	Arg a;

	reset_term(80, 24);
	for (i = 1; i <= 60; i++) {
		snprintf(buf, sizeof(buf), "line-%03d\r\n", i);
		feed(buf);
	}
	a.i = 10;
	kscrollup(&a);
	/* viewer top row is line-028; select it like x.c would */
	selstart(0, 0, 0);
	selextend(7, 0, SEL_REGULAR, 0);
	selextend(7, 0, SEL_REGULAR, 1);
	s = getsel();
	CHECK(s && strcmp(s, "line-028") == 0, "selected '%s'", s ? s : "(null)");
	free(s);

	/* selection must track further scrolling */
	a.i = 3;
	kscrollup(&a);
	s = getsel();
	CHECK(s && strcmp(s, "line-028") == 0, "after scroll selected '%s'", s ? s : "(null)");
	free(s);

	/* extended variant: persists even scrolled fully off-screen... */
	a.i = 1000000;
	kscrolldown(&a);
	CHECK(sel.ob.x != -1, "selection should persist when scrolled off screen");
	s = getsel(); /* off-screen rows render empty, but must not crash */
	free(s);

	/* ...and is intact again when scrolled back into view */
	a.i = 13;
	kscrollup(&a);
	s = getsel();
	CHECK(s && strcmp(s, "line-028") == 0,
	      "selection lost after off-screen round trip: '%s'", s ? s : "(null)");
	free(s);

	/* ...but a resize resets it (reflow invalidates the coords) */
	tresize(70, 24);
	CHECK(sel.ob.x == -1, "selection should clear on resize");
}

static void
test_wide_then_narrow_write(void)
{
	/* regression for the old "jumbled output" bug: write while wide,
	 * shrink, write more, grow back - nothing may be lost */
	char *all;
	char buf[256];
	int i;

	reset_term(100, 24);
	for (i = 0; i < 90; i++)
		buf[i] = '0' + (i % 10);
	buf[90] = '\0';
	feed(buf);
	feed("\r\n");
	tresize(50, 24);
	feed("narrow line\r\n");
	tresize(100, 24);
	all = dumpall();
	CHECK(strstr(all, buf) != NULL, "90-char line broken after cycle:\n%s", all);
	CHECK(strstr(all, "narrow line") != NULL, "narrow write lost:\n%s", all);
	free(all);
}

static void
test_ring_wrap_reflow(void)
{
	/* reflow once the ring buffer has wrapped (cap 50), with lines long
	 * enough that the 40-col step really rewraps them. Narrow reflow may
	 * evict the oldest rows (cap is counted in rows) - that's by design -
	 * but newest content must survive intact and invariants must hold. */
	char buf[128], *all;
	int i;

	reset_term(80, 24);
	for (i = 1; i <= 200; i++) {
		snprintf(buf, sizeof(buf),
		         "ring-%03d 0123456789012345678901234567890123456789012345678901234567890123\r\n", i);
		feed(buf);
	}
	CHECK(sb.len == 50, "sb.len=%d", sb.len);
	tresize(40, 24);
	CHECK(sb.len <= 50, "sb.len=%d exceeds cap after reflow", sb.len);
	tresize(80, 24);
	CHECK(sb.len <= 50, "sb.len=%d exceeds cap after grow", sb.len);
	all = dumpall();
	CHECK(strstr(all, "ring-200 0123456789012345678901234567890123456789012345678901234567890123") != NULL,
	      "ring-200 not intact:\n%.600s", all);
	free(all);

	feed("post-reflow write\r\n");
	all = dumpall();
	CHECK(strstr(all, "post-reflow write") != NULL, "term broken after ring reflow");
	free(all);
}

static void
test_many_random_resizes(void)
{
	/* fuzz: random resize storm, then content must still be coherent */
	char buf[128], *all;
	int i;
	unsigned seed = 12345;

	reset_term(80, 24);
	for (i = 1; i <= 80; i++) {
		snprintf(buf, sizeof(buf), "fuzz-%03d some padding text here\r\n", i);
		feed(buf);
	}
	for (i = 0; i < 60; i++) {
		seed = seed * 1103515245 + 12345;
		int c = 20 + (seed >> 16) % 140;
		seed = seed * 1103515245 + 12345;
		int r = 5 + (seed >> 16) % 45;
		tresize(c, r);
	}
	tresize(80, 24);
	all = dumpall();
	CHECK(strstr(all, "fuzz-080 some padding text here") != NULL,
	      "newest line lost after resize storm");
	free(all);

	/* and the terminal still works */
	feed("still alive\r\n");
	CHECK(strstr((all = dumpall()), "still alive") != NULL, "term dead after storm");
	free(all);
}

/*
 * Simulated shell SIGWINCH redraws during a window drag, modeled on the
 * real emit sequences (verified against readline display.c and zsh
 * zle_refresh.c, and against PTY traces of the real shells):
 *
 * readline >= 8.2: \r \e[K, then (\e[A \e[K) x botlin where botlin is
 * the STALE row count from its previous redisplay, then the prompt.
 * zsh: \r \r, \e[<n>A with stale n, sgr0, \e[J (clr_eos), then the full
 * prompt reprint.
 *
 * Both shells position with stale relative moves; the accepted-artifact
 * contract for a reflowing terminal (same as kitty/foot/alacritty) is:
 * lines just above the prompt may be erased by the shell while the
 * window is narrower than the prompt, but no prompt fragments may be
 * duplicated, stranded, or joined together.
 */
static const char drag_prompt[] = "[carbon@gaia ~]$ ";
#define DRAG_PLEN ((int)sizeof(drag_prompt) - 1)

static void
drag_with_readline(int from, int to, int lag, int *botlin)
{
	char esc[32];
	int w, i, step = (to > from) ? 1 : -1, n = 0;

	for (w = from; w != to + step; w += step) {
		tresize(w, 24);
		if (++n % lag != 0 && w != to)
			continue; /* SIGWINCH coalesced; shell redraw lags */
		feed("\r\033[K");
		for (i = 0; i < *botlin; i++)
			feed("\033[A\033[K");
		feed(drag_prompt);
		if (DRAG_PLEN % w == 0)
			feed(" \r"); /* deferred-autowrap forced */
		*botlin = DRAG_PLEN / w;
	}
}

static void
drag_with_zsh(int from, int to, int lag, int *vpos)
{
	char esc[32];
	int w, step = (to > from) ? 1 : -1, n = 0;

	for (w = from; w != to + step; w += step) {
		tresize(w, 24);
		if (++n % lag != 0 && w != to)
			continue;
		feed("\r\r");
		if (*vpos > 0) {
			snprintf(esc, sizeof(esc), "\033[%dA", *vpos);
			feed(esc);
		}
		feed("\033[0m\033[J");
		feed(drag_prompt);
		if (DRAG_PLEN % w == 0) {
			/* zsh's boundary forced wrap puts the cursor on the
			 * next row, and zsh remembers that in its vln */
			feed("\r\n\033[K");
			*vpos = DRAG_PLEN / w;
		} else {
			*vpos = (DRAG_PLEN - 1) / w;
		}
	}
}

static void
check_no_prompt_junk(const char *all)
{
	const char *p;
	int joined = 0, frags = 0;

	for (p = all; (p = strstr(p, "[carbon@gaia")) != NULL; p++) {
		const char *q = strstr(p + 1, "[carbon@gaia");
		const char *nl = strchr(p, '\n');
		frags++;
		if (q && (!nl || q < nl))
			joined++;
	}
	CHECK(joined == 0, "found %d joined prompt fragments:\n%s", joined, all);
	CHECK(frags <= 3, "prompt duplicated: %d fragments (expect <= 3):\n%s",
	      frags, all);
}

static void
test_narrow_drag_prompt_junk(void)
{
	char *all;
	int pos, lag, shell;

	/* both shells, redrawing on every resize and lagging behind */
	for (shell = 0; shell < 2; shell++) {
		for (lag = 1; lag <= 4; lag++) {
			reset_term(80, 24);
			feed("The Dhammapada\r\n");
			feed("320. Silently shall I endure abuse.\r\n");
			feed(drag_prompt);
			feed("echo hello test\r\n");
			feed("hello test\r\n");
			feed(drag_prompt);
			feed("echo hi\r\n");
			feed("hi\r\n");
			feed(drag_prompt);
			pos = 0;

			if (shell == 0) {
				drag_with_zsh(79, 8, lag, &pos);
				drag_with_zsh(9, 80, lag, &pos);
			} else {
				drag_with_readline(79, 8, lag, &pos);
				drag_with_readline(9, 80, lag, &pos);
			}

			all = dumpall();
			check_no_prompt_junk(all);
			free(all);
		}
	}
}

/*
 * A floating-toggle in dwm often changes the window's pixel size without
 * changing the cell grid; the kernel then sends no SIGWINCH, so a
 * full-screen app never repaints. The alt screen must survive tresize
 * verbatim in that case (the upstream patch blanked it).
 */
static void
test_alt_resize_preserves_screen(void)
{
	char buf[64];
	int i, hist;

	reset_term(80, 24);
	for (i = 1; i <= 30; i++) {
		snprintf(buf, sizeof(buf), "main-%02d\r\n", i);
		feed(buf);
	}
	hist = sb.len;
	feed("\033[?1049h");
	/* vim-like: address every row directly, no scrolling */
	for (i = 0; i < 24; i++) {
		snprintf(buf, sizeof(buf), "\033[%d;1Halt-row-%02d", i + 1, i);
		feed(buf);
	}

	/* same cell grid: app gets no SIGWINCH and repaints nothing */
	tresize(80, 24);
	for (i = 0; i < 24; i++) {
		snprintf(buf, sizeof(buf), "alt-row-%02d", i);
		if (strcmp(rowtext(i), buf) != 0) {
			CHECK(0, "alt row %d blanked: '%s'", i, rowtext(i));
			break;
		}
	}
	CHECK(term.dirty[12] == 1, "rows must be dirty so st repaints them");

	/* small width wiggle and back (floating-geometry case) */
	tresize(78, 24);
	tresize(80, 24);
	CHECK(strncmp(rowtext(5), "alt-row-05", 10) == 0,
	      "alt row 5 lost after width wiggle: '%s'", rowtext(5));

	/* main screen + history must survive the round trip */
	feed("\033[?1049l");
	CHECK(sb.len == hist, "history changed across alt resizes: %d -> %d",
	      hist, sb.len);
	{
		char *all = dumpall();
		CHECK(strstr(all, "main-01") != NULL, "main-01 lost");
		CHECK(strstr(all, "main-30") != NULL, "main-30 lost");
		CHECK(strstr(all, "alt-row") == NULL, "alt content leaked to main");
		free(all);
	}
}

static void
test_enter_alt_while_scrolled(void)
{
	char buf[64];
	int i;
	Arg a;

	reset_term(80, 24);
	for (i = 1; i <= 60; i++) {
		snprintf(buf, sizeof(buf), "line-%03d\r\n", i);
		feed(buf);
	}
	a.i = 10;
	kscrollup(&a);
	CHECK(sb.view_offset == 10, "scrolled");
	feed("\033[?1049h"); /* enter alt while viewing history */
	CHECK(sb.view_offset == 0, "view must snap to live on screen swap");
	feed("\033[Halt-here");
	CHECK(strcmp(viewtext(0), "alt-here") == 0,
	      "alt screen not rendered while scrolled: '%s'", viewtext(0));
	feed("\033[?1049l");
	CHECK(strcmp(viewtext(0), "line-038") == 0,
	      "main view wrong after alt exit: '%s'", viewtext(0));
}

/*
 * Replay a PTY trace captured from a real shell (pty_trace.py):
 * lines "R <cols> <rows>" apply a resize, lines "O <hex>" feed raw
 * shell output. Returns the number of junk lines (lines holding more
 * than one prompt fragment) found in the final scrollback dump.
 */
static int
hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int run_tests(void);

static int
replay_trace(const char *path, int verbose)
{
	static char lbuf[1 << 20];
	static char raw[1 << 19];
	FILE *f;
	char *all;
	const char *p;
	int junk = 0, c, r, len, hi, lo;
	char *h;

	f = fopen(path, "r");
	if (!f) {
		printf("cannot open %s\n", path);
		return -1;
	}
	reset_term(80, 24);
	while (fgets(lbuf, sizeof(lbuf), f)) {
		if (lbuf[0] == 'R') {
			if (sscanf(lbuf + 1, "%d %d", &c, &r) == 2)
				tresize(c, r);
		} else if (lbuf[0] == 'O' && lbuf[1] == ' ') {
			len = 0;
			for (h = lbuf + 2; (hi = hexval(h[0])) >= 0 &&
			     (lo = hexval(h[1])) >= 0 &&
			     len < (int)sizeof(raw); h += 2)
				raw[len++] = (hi << 4) | lo;
			twrite(raw, len, 0);
		}
	}
	fclose(f);

	all = dumpall();
	{
		int joined = 0, frags = 0;
		for (p = all; (p = strstr(p, "[carb")) != NULL; p++) {
			const char *q = strstr(p + 1, "[carb");
			const char *nl = strchr(p, '\n');
			frags++;
			if (q && (!nl || q < nl))
				joined++;
		}
		/* the session prints exactly 3 prompts (after the two echos
		 * and the final idle one); anything beyond that is a
		 * stranded fragment */
		junk = joined + (frags > 3 ? frags - 3 : 0);
		if (verbose)
			printf("=== %s ===\n%s\n", path, all);
		printf("%-50s joined: %d  prompt frags: %d (expect 3)  junk: %d\n",
		       path, joined, frags, junk);
	}
	free(all);
	return junk;
}

int
main(int argc, char *argv[])
{
	int i, junk = 0, verbose = 0, rc;

	setlocale(LC_CTYPE, "");
	cmdfd = open("/dev/null", O_WRONLY); /* sink for ttywrite */

	/* trace replay mode: test_st [-v] [-P] trace.txt... */
	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "-v") == 0) {
				verbose = 1;
				continue;
			}
			rc = replay_trace(argv[i], verbose);
			if (rc > 0)
				junk += rc;
		}
		return junk ? 1 : 0;
	}
	return run_tests();
}

static int
run_tests(void)
{

	test_basic_write();
	test_scrollback_fills();
	test_kscroll_view();
	test_width_shrink_grow_reflow();
	test_width_cycle_restores();
	test_height_cycle_restores();
	test_altscreen();
	test_clear_scrollback();
	test_selection_in_history();
	test_wide_then_narrow_write();
	test_ring_wrap_reflow();
	test_many_random_resizes();
	test_narrow_drag_prompt_junk();
	test_alt_resize_preserves_screen();
	test_enter_alt_while_scrolled();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
