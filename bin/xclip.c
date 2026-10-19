/*
 * xclip: the part of xclip this system uses, with plain Xlib (no libXmu, so
 * x11-misc/xclip, libXmu, libXt, libSM and libICE can go).
 *
 *   xclip [-i] [-selection clipboard|primary|secondary] [-t TYPE] [file...]
 *       stores stdin (or the files) and serves it from the background until
 *       another client takes the selection (dwm's screenshot key: grab | xclip)
 *   xclip -o [-selection ...] [-t TYPE]
 *       prints the selection; -t TARGETS lists the offered types, one per line
 *       (what terminal apps ask before pasting an image)
 *
 * Default type is UTF8_STRING (also offered as STRING and TEXT). Data larger
 * than one X request goes both ways with the ICCCM INCR protocol. Options are
 * matched by prefix like xclip's (-o, -out, -sel, -t, -target, -d ...); -f and
 * -r work, -silent/-quiet/-verbose/-loops are accepted and ignored.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXINCR 16

static Display *dpy;
static Window win;
static Atom sel, type, a_targets, a_incr, a_utf8, a_text;
static unsigned char *data;
static size_t len, chunk;
static int text, lost;

static struct incr {
	Window w;
	Atom prop, type;
	size_t off;
	int done;
} xfer[MAXINCR];

static void die(const char *m)
{
	fprintf(stderr, "xclip: %s\n", m);
	exit(1);
}

static int ignore_errors(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	return 0; /* a requestor that vanished mid-transfer must not kill the server */
}

static void append(FILE *f)
{
	static size_t cap;
	size_t r;

	for (;;) {
		if (len == cap) {
			cap = cap ? cap * 2 : 1 << 16;
			if (!(data = realloc(data, cap)))
				die("out of memory");
		}
		if (!(r = fread(data + len, 1, cap - len, f)))
			break;
		len += r;
	}
}

static size_t bytes(int fmt, unsigned long n)
{
	return fmt == 32 ? n * sizeof(long) : fmt == 16 ? n * sizeof(short) : n;
}

static int out(void)
{
	Atom prop = XInternAtom(dpy, "XCLIP_OUT", False), t;
	unsigned long n, after;
	unsigned char *d;
	XEvent ev;
	int fmt;

	XSelectInput(dpy, win, PropertyChangeMask);
	XConvertSelection(dpy, sel, type, prop, win, CurrentTime);
	do
		XNextEvent(dpy, &ev);
	while (ev.type != SelectionNotify || ev.xselection.selection != sel);
	if (ev.xselection.property == None)
		goto unavailable;
	if (XGetWindowProperty(dpy, win, prop, 0, LONG_MAX / 4, True, AnyPropertyType,
			       &t, &fmt, &n, &after, &d) != Success || t == None)
		goto unavailable;
	if (t == a_incr) {
		/* deleting the INCR property (done above) starts the transfer */
		XFree(d);
		for (;;) {
			do
				XNextEvent(dpy, &ev);
			while (ev.type != PropertyNotify || ev.xproperty.atom != prop ||
			       ev.xproperty.state != PropertyNewValue);
			if (XGetWindowProperty(dpy, win, prop, 0, LONG_MAX / 4, True, AnyPropertyType,
					       &t, &fmt, &n, &after, &d) != Success)
				die("transfer failed");
			if (!n) {
				XFree(d);
				return 0;
			}
			fwrite(d, 1, bytes(fmt, n), stdout);
			XFree(d);
		}
	}
	if (type == a_targets && t == XA_ATOM) {
		for (unsigned long i = 0; i < n; i++) {
			char *name = XGetAtomName(dpy, ((Atom *)d)[i]);
			if (name) {
				puts(name);
				XFree(name);
			}
		}
	} else {
		fwrite(d, 1, bytes(fmt, n), stdout);
	}
	XFree(d);
	return 0;
unavailable:
	{
		char *name = XGetAtomName(dpy, type);
		fprintf(stderr, "Error: target %s not available\n", name ? name : "?");
		return 1;
	}
}

/* the property type to answer a request for target t with, or None */
static Atom serves(Atom t)
{
	if (t == type)
		return type;
	if (text && (t == XA_STRING || t == a_text))
		return t == a_text ? a_utf8 : XA_STRING;
	return None;
}

static void request(XSelectionRequestEvent *r)
{
	XSelectionEvent sn = {
		.type = SelectionNotify, .display = dpy, .requestor = r->requestor,
		.selection = r->selection, .target = r->target, .time = r->time,
		.property = None,
	};
	Atom prop = r->property != None ? r->property : r->target, t;

	if (r->target == a_targets) {
		Atom offer[] = { a_targets, type, XA_STRING, a_text };
		XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace,
				(unsigned char *)offer, text ? 4 : 2);
		sn.property = prop;
	} else if ((t = serves(r->target)) != None) {
		if (len > chunk) {
			int i;
			for (i = 0; i < MAXINCR && xfer[i].w; i++)
				;
			if (i < MAXINCR) {
				long size = (long)len;
				xfer[i] = (struct incr){ r->requestor, prop, t, 0, 0 };
				XSelectInput(dpy, r->requestor, PropertyChangeMask);
				XChangeProperty(dpy, r->requestor, prop, a_incr, 32, PropModeReplace,
						(unsigned char *)&size, 1);
				sn.property = prop;
			}
		} else {
			XChangeProperty(dpy, r->requestor, prop, t, 8, PropModeReplace, data, (int)len);
			sn.property = prop;
		}
	}
	XSendEvent(dpy, r->requestor, False, NoEventMask, (XEvent *)&sn);
	XFlush(dpy);
}

/* the requestor deleted the property: send the next INCR chunk (an empty one ends it) */
static void next_chunk(XPropertyEvent *p)
{
	for (int i = 0; i < MAXINCR; i++) {
		struct incr *x = &xfer[i];
		if (!x->w || x->w != p->window || x->prop != p->atom)
			continue;
		if (x->done) {
			*x = (struct incr){ 0 };
			return;
		}
		size_t n = len - x->off < chunk ? len - x->off : chunk;
		XChangeProperty(dpy, x->w, x->prop, x->type, 8, PropModeReplace, data + x->off, (int)n);
		x->off += n;
		x->done = !n;
		XFlush(dpy);
		return;
	}
}

static int busy(void)
{
	for (int i = 0; i < MAXINCR; i++)
		if (xfer[i].w)
			return 1;
	return 0;
}

static void serve(void)
{
	XEvent ev;
	pid_t pid;
	int fd;

	XSetErrorHandler(ignore_errors);
	XSetSelectionOwner(dpy, sel, win, CurrentTime);
	if (XGetSelectionOwner(dpy, sel) != win)
		die("could not take the selection");
	XSync(dpy, False);
	/* like xclip: the caller returns at once, a child serves the data */
	if ((pid = fork()) < 0)
		die("fork failed");
	if (pid)
		_exit(0);
	setsid();
	if ((fd = open("/dev/null", O_RDWR)) >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	for (;;) {
		XNextEvent(dpy, &ev);
		if (ev.type == SelectionRequest)
			request(&ev.xselectionrequest);
		else if (ev.type == PropertyNotify && ev.xproperty.state == PropertyDelete)
			next_chunk(&ev.xproperty);
		else if (ev.type == SelectionClear && ev.xselectionclear.selection == sel)
			lost = 1;
		if (lost && !busy())
			exit(0);
	}
}

int main(int argc, char **argv)
{
	const char *selname = "primary", *tname = NULL, *display = NULL;
	int o = 0, filter = 0, rmnl = 0, files = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' || !a[1]) {
			continue;
		} else if (!strncmp(a, "-se", 3) && i + 1 < argc) {
			selname = argv[++i];
		} else if (!strncmp(a, "-t", 2) && i + 1 < argc) {
			tname = argv[++i];
		} else if (!strncmp(a, "-d", 2) && i + 1 < argc) {
			display = argv[++i];
		} else if (!strncmp(a, "-l", 2) && i + 1 < argc) {
			i++; /* -loops: ignored */
		} else if (!strncmp(a, "-o", 2)) {
			o = 1;
		} else if (!strncmp(a, "-i", 2)) {
			o = 0;
		} else if (!strncmp(a, "-f", 2)) {
			filter = 1;
		} else if (!strncmp(a, "-r", 2)) {
			rmnl = 1;
		} else if (!strncmp(a, "-h", 2)) {
			puts("usage: xclip [-i|-o] [-selection primary|secondary|clipboard] [-t type] [-f] [-r] [file...]");
			return 0;
		}
	}
	if (!(dpy = XOpenDisplay(display)))
		die("can't open display");
	sel = selname[0] == 'c' ? XInternAtom(dpy, "CLIPBOARD", False)
	    : selname[0] == 's' ? XA_SECONDARY : XA_PRIMARY;
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_incr = XInternAtom(dpy, "INCR", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_text = XInternAtom(dpy, "TEXT", False);
	type = tname ? XInternAtom(dpy, tname, False) : a_utf8;
	text = type == a_utf8 || type == XA_STRING || type == a_text;
	win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
	if (o)
		return out();

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		FILE *f;
		if (a[0] == '-' && a[1]) {
			if ((!strncmp(a, "-se", 3) || !strncmp(a, "-t", 2) || !strncmp(a, "-d", 2) ||
			     !strncmp(a, "-l", 2)) && i + 1 < argc)
				i++;
			continue;
		}
		if (!(f = fopen(a, "rb"))) {
			fprintf(stderr, "xclip: %s: can't open\n", a);
			return 1;
		}
		append(f);
		fclose(f);
		files = 1;
	}
	if (!files)
		append(stdin);
	if (rmnl && len && data[len - 1] == '\n')
		len--;
	if (filter)
		fwrite(data, 1, len, stdout), fflush(stdout);
	chunk = (size_t)XMaxRequestSize(dpy) * 4 - 1024;
	serve();
	return 0;
}
