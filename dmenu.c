/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define LENGTH(X)             (sizeof X / sizeof X[0])
#define TEXTW(X)              (drw_fontset_getwidth(drw, (X)) + lrpad)

/* enums */
enum { SchemeNorm, SchemeSel, SchemeHp, SchemeOut, SchemeLast }; /* color schemes */

struct item {
	char *text;
	struct item *left, *right;
	int out, hp;
};

static char **hpitems = NULL;
static int hplength = 0;
static char text[BUFSIZ] = "";
static char *embed;
static int bh, mw, mh;
static int inputw = 0, promptw;
static int lrpad; /* sum of left and right padding */
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;

static Atom clip, utf8;
static Display *display;
static Window root, parentWindow, window;
static XIC inputContext;

static Drw *drw;
static Clr *scheme[SchemeLast];

#include "config.h"

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

static char**
tokenize(char *source, const char *delim, int *llen) {
	int listlength = 0;
	char **list = malloc(1 * sizeof(char*));
	char *token = strtok(source, delim);

	while (token) {
		if (!(list = realloc(list, sizeof(char*) * (listlength + 1))))
			die("Unable to realloc %d bytes\n", sizeof(char*) * (listlength + 1));
		if (!(list[listlength] = strdup(token)))
			die("Unable to strdup %d bytes\n", strlen(token) + 1);
		token = strtok(NULL, delim);
		listlength++;
	}

	*llen = listlength;
	return list;
}

static int
arrayhas(char **list, int length, char *item) {
	for (int i = 0; i < length; i++) {
		int len1 = strlen(list[i]);
		int len2 = strlen(item);
		if (fstrncmp(list[i], item, len1 > len2 ? len2 : len1) == 0)
			return 1;
	}
	return 0;
}

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * bh;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : MIN(TEXTW(next->text), n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : MIN(TEXTW(prev->left->text), n)) > n)
			break;
}

static void
cleanup(void)
{
	size_t i;

	XUngrabKey(display, AnyKey, AnyModifier, root);
	for (i = 0; i < SchemeLast; i++)
		free(scheme[i]);
	drw_free(drw);
	XSync(display, False);
	XCloseDisplay(display);
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	if (item == sel)
		drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->hp)
		drw_setscheme(drw, scheme[SchemeHp]);
	else if (item->out)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		drw_setscheme(drw, scheme[SchemeNorm]);

	return drw_text(drw, x, y, w, bh, lrpad / 2, item->text, 0);
}

static void
drawmenu(void)
{
	unsigned int curpos;
	struct item *item;
	int x = 0, y = 0, w;

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, mw, mh, 1, 1);

	if (prompt && *prompt) {
		drw_setscheme(drw, scheme[SchemeSel]);
		x = drw_text(drw, x, 0, promptw, bh, lrpad / 2, prompt, 0);
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_text(drw, x, 0, w, bh, lrpad / 2, text, 0);

	curpos = TEXTW(text) - TEXTW(&text[cursor]);
	if ((curpos += lrpad / 2 - 1) < w) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x + curpos, 2, 2, bh - 4, 1, 0);
	}

	if (lines > 0) {
		/* draw vertical list */
		for (item = curr; item != next; item = item->right)
			drawitem(item, x, y += bh, mw - x);
	} else if (matches) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW("<");
		if (curr->left) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, lrpad / 2, "<", 0);
		}
		x += w;
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, 0, MIN(TEXTW(item->text), mw - x - TEXTW(">")));
		if (next) {
			w = TEXTW(">");
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w, 0, w, bh, lrpad / 2, ">", 0);
		}
	}
	drw_map(drw, window, 0, 0, mw, mh);
}

static void
grabfocus(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000  };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(display, &focuswin, &revertwin);
		if (focuswin == window)
			return;
		XSetInputFocus(display, window, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	die("cannot grab focus");
}

static void grabKeyboard(void) {
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	if (embed)
		return;
	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(display, DefaultRootWindow(display), True, GrabModeAsync,
                          GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	die("cannot grab keyboard");
}

static void
match(void)
{
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lhpprefix, *lprefix, *lsubstr, *hpprefixend, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %u bytes:", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lhpprefix = lprefix = lsubstr = matchend = hpprefixend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++) {
		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes with high priority, then prefixes, then substrings */
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (item->hp && !fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lhpprefix, &hpprefixend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if (lhpprefix) {
		if (matches) {
			matchend->right = lhpprefix;
			lhpprefix->left = matchend;
		} else
			matches = lhpprefix;
		matchend = hpprefixend;
	}
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr) {
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if (n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) { /* move cursor to the start of the word*/
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else { /* move cursor to the end of the word */
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

static void keyPress(XKeyEvent *keyEvent) {
	char buffer[32];
	int length;
	KeySym keySymbol;
	Status status;

    length = XmbLookupString(inputContext, keyEvent, buffer, sizeof buffer, &keySymbol, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return;
	case XLookupChars:
		goto insert;
	case XLookupKeySym:
	case XLookupBoth:
		break;
	}

	if (keyEvent->state & ControlMask) {
		switch(keySymbol) {
		case XK_a: keySymbol = XK_Home;      break;
		case XK_b: keySymbol = XK_Left;      break;
		case XK_c: keySymbol = XK_Escape;    break;
		case XK_d: keySymbol = XK_Delete;    break;
		case XK_e: keySymbol = XK_End;       break;
		case XK_f: keySymbol = XK_Right;     break;
		case XK_g: keySymbol = XK_Escape;    break;
		case XK_h: keySymbol = XK_BackSpace; break;
		case XK_i: keySymbol = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: keySymbol = XK_Return; keyEvent->state &= ~ControlMask; break;
		case XK_n: keySymbol = XK_Down;      break;
		case XK_p: keySymbol = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			insert(NULL, 0 - cursor);
			break;
		case XK_w: /* delete word */
			while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_y: /* paste selection */
		case XK_Y:
			XConvertSelection(display, (keyEvent->state & ShiftMask) ? clip : XA_PRIMARY,
                              utf8, utf8, window, CurrentTime);
			return;
		case XK_Left:
		case XK_KP_Left:
			movewordedge(-1);
			goto draw;
		case XK_Right:
		case XK_KP_Right:
			movewordedge(+1);
			goto draw;
		case XK_Return:
		case XK_KP_Enter:
			break;
		case XK_bracketleft:
			cleanup();
			exit(1);
		default:
			return;
		}
	} else if (keyEvent->state & Mod1Mask) {
		switch(keySymbol) {
		case XK_b:
			movewordedge(-1);
			goto draw;
		case XK_f:
			movewordedge(+1);
			goto draw;
		case XK_g: keySymbol = XK_Home;  break;
		case XK_G: keySymbol = XK_End;   break;
		case XK_h: keySymbol = XK_Up;    break;
		case XK_j: keySymbol = XK_Next;  break;
		case XK_k: keySymbol = XK_Prior; break;
		case XK_l: keySymbol = XK_Down;  break;
		default:
			return;
		}
	}

	switch(keySymbol) {
	default:
insert:
		if (!iscntrl(*buffer))
			insert(buffer, length);
		break;
	case XK_Delete:
	case XK_KP_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XK_BackSpace:
		if (cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XK_End:
	case XK_KP_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if (next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while (next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XK_Escape:
		cleanup();
		exit(1);
	case XK_Home:
	case XK_KP_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XK_Left:
	case XK_KP_Left:
		if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Up:
	case XK_KP_Up:
		if (sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XK_Next:
	case XK_KP_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
	case XK_KP_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		puts((sel && !(keyEvent->state & ShiftMask)) ? sel->text : text);
		if (!(keyEvent->state & ControlMask)) {
			cleanup();
			exit(0);
		}
		if (sel)
			sel->out = 1;
		break;
	case XK_Right:
	case XK_KP_Right:
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Down:
	case XK_KP_Down:
		if (sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
		break;
	case XK_Tab:
		if (!sel)
			return;
		strncpy(text, sel->text, sizeof text - 1);
		text[sizeof text - 1] = '\0';
		cursor = strlen(text);
		match();
		break;
	}

draw:
	drawmenu();
}

static void
paste(void)
{
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	if (XGetWindowProperty(display, window, utf8, 0, (sizeof text / 4) + 1, False,
                           utf8, &da, &di, &dl, &dl, (unsigned char **)&p)
	    == Success && p) {
		insert(p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
		XFree(p);
	}
	drawmenu();
}

static void
readstdin(void)
{
	char buf[sizeof text], *p;
	size_t i, imax = 0, size = 0;
	unsigned int tmpmax = 0;

	/* read each line from stdin and add it to the item list */
	for (i = 0; fgets(buf, sizeof buf, stdin); i++) {
		if (i + 1 >= size / sizeof *items)
			if (!(items = realloc(items, (size += BUFSIZ))))
				die("cannot realloc %u bytes:", size);
		if ((p = strchr(buf, '\n')))
			*p = '\0';
		if (!(items[i].text = strdup(buf)))
			die("cannot strdup %u bytes:", strlen(buf) + 1);
		items[i].out = 0;
		items[i].hp = arrayhas(hpitems, hplength, items[i].text);
		drw_font_getexts(drw->fonts, buf, strlen(buf), &tmpmax, NULL);
		if (tmpmax > inputw) {
			inputw = tmpmax;
			imax = i;
		}
	}
	if (items)
		items[i].text = NULL;
	inputw = items ? TEXTW(items[imax].text) : 0;
	lines = MIN(lines, i);
}

static void run(void) {
	XEvent event;

	while (!XNextEvent(display, &event)) {
		if (XFilterEvent(&event, window))
			continue;
		switch(event.type) {
		case DestroyNotify:
			if (event.xdestroywindow.window != window)
				break;
			cleanup();
			exit(1);
		case Expose:
			if (event.xexpose.count == 0)
				drw_map(drw, window, 0, 0, mw, mh);
			break;
		case FocusIn:
			/* regrab focus from parent window */
			if (event.xfocus.window != window)
				grabfocus();
			break;
		case KeyPress:
            keyPress(&event.xkey);
			break;
		case SelectionNotify:
			if (event.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if (event.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(display, window);
			break;
		}
	}
}

static void
setup(void)
{
	int x, y, i, j;
	unsigned int du;
	XSetWindowAttributes swa;
	XIM xim;
	Window w, dw, *dws;
	XWindowAttributes wa;
	XClassHint ch = {"dmenu", "dmenu"};
#ifdef XINERAMA
	XineramaScreenInfo *info;
	Window pw;
	int a, di, n, area = 0;
#endif
	/* init appearance */
	for (j = 0; j < SchemeLast; j++)
		scheme[j] = drw_scm_create(drw, colors[j], 2);

	clip = XInternAtom(display, "CLIPBOARD", False);
	utf8 = XInternAtom(display, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = drw->fonts->h + 2;
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh;
#ifdef XINERAMA
	i = 0;
	if (parentWindow == root && (info = XineramaQueryScreens(display, &n))) {
		XGetInputFocus(display, &w, &di);
		if (mon >= 0 && mon < n)
			i = mon;
		else if (w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if (XQueryTree(display, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while (w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if (XGetWindowAttributes(display, pw, &wa))
				for (j = 0; j < n; j++)
					if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if (mon < 0 && !area && XQueryPointer(display, root, &dw, &dw, &x, &y, &di, &di, &du))
			for (i = 0; i < n; i++)
				if (INTERSECT(x, y, 1, 1, info[i]))
					break;

		x = info[i].x_org;
		y = info[i].y_org + (topbar ? 0 : info[i].height - mh);
		mw = info[i].width;
		XFree(info);
	} else
#endif
	{
		if (!XGetWindowAttributes(display, parentWindow, &wa))
			die("could not get embedding window attributes: 0x%lx",
                parentWindow);
		x = 0;
		y = topbar ? 0 : wa.height - mh;
		mw = wa.width;
	}
	promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;
	inputw = MIN(inputw, mw/3);
	match();

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;
    window = XCreateWindow(display, parentWindow, x, y, mw, mh, 0,
                           CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	XSetClassHint(display, window, &ch);


	/* input methods */
	if ((xim = XOpenIM(display, NULL, NULL, NULL)) == NULL)
		die("XOpenIM failed: could not open input device");

    inputContext = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                             XNClientWindow, window, XNFocusWindow, window, NULL);

	XMapRaised(display, window);
	if (embed) {
		XSelectInput(display, parentWindow, FocusChangeMask | SubstructureNotifyMask);
		if (XQueryTree(display, parentWindow, &dw, &w, &dws, &du) && dws) {
			for (i = 0; i < du && dws[i] != window; ++i)
				XSelectInput(display, dws[i], FocusChangeMask);
			XFree(dws);
		}
		grabfocus();
	}
	drw_resize(drw, mw, mh);
	drawmenu();
}

static void
usage(void)
{
	fputs("usage: dmenu [-bfiv] [-l lines] [-p prompt] [-fn font] [-m monitor]\n"
	      "             [-nb color] [-nf color] [-sb color] [-sf color] [-w windowid]\n"
		  "             [-hb color] [-hf color] [-hp items]\n", stderr);
	exit(1);
}

int main(int argc, char *argv[]) {
	XWindowAttributes windowAttributes;
	int i, fast = 0;

	for (i = 1; i < argc; i++)
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) {      /* prints version information */
			puts("dmenu-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-b")) /* appears at the bottom of the screen */
			topbar = 0;
		else if (!strcmp(argv[i], "-f"))   /* grabs keyboard before reading stdin */
			fast = 1;
		else if (!strcmp(argv[i], "-i")) { /* case-insensitive item matching */
			fstrncmp = strncasecmp;
			fstrstr = strcasestr;
		} else if (i + 1 == argc)
			usage();
		/* these options take one argument */
		else if (!strcmp(argv[i], "-l"))   /* number of lines in vertical list */
			lines = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-m"))
			mon = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p"))   /* adds prompt to left of input field */
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-fn"))  /* font or font set */
			fonts[0] = argv[++i];
		else if (!strcmp(argv[i], "-nb"))  /* normal background color */
			colors[SchemeNorm][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-nf"))  /* normal foreground color */
			colors[SchemeNorm][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-sb"))  /* selected background color */
			colors[SchemeSel][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-sf"))  /* selected foreground color */
			colors[SchemeSel][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-hb"))  /* high priority background color */
			colors[SchemeHp][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-hf")) /* low priority background color */
			colors[SchemeHp][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-w"))   /* embedding window id */
			embed = argv[++i];
		else if (!strcmp(argv[i], "-hp"))
			hpitems = tokenize(argv[++i], ",", &hplength);
		else
			usage();

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(display = XOpenDisplay(NULL)))
		die("cannot open display");
	screen = DefaultScreen(display);
	root = RootWindow(display, screen);
	if (!embed || !(parentWindow = strtol(embed, NULL, 0)))
        parentWindow = root;
	if (!XGetWindowAttributes(display, parentWindow, &windowAttributes))
		die("could not get embedding window attributes: 0x%lx",
            parentWindow);
	drw = drw_create(display, screen, root, windowAttributes.width, windowAttributes.height);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;

#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		die("pledge");
#endif

	if (fast && !isatty(0)) {
        grabKeyboard();
		readstdin();
	} else {
		readstdin();
        grabKeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}
