/*
 * grab: a screen region as PNG, at any X depth.
 *
 *   grab [-g WxH+X+Y] FILE|-
 *
 * shotgun reads only 8-bit framebuffers and writes nothing at X depth 30.
 * This reads the root window with XGetImage at whatever depth X runs,
 * converts each pixel through its channel masks to 8 bits per channel, and
 * writes an RGB PNG (zlib for compression and CRCs). Used by dwm's region
 * screenshot key with hacksaw for the selection and xclip for the clipboard.
 *
 * It reads in strips of 64 rows: X (glamor) reads the screen back through a GPU
 * buffer the size of the request, and mesa keeps that buffer cached, so one
 * full-screen request left ~20 MiB in the X server until it exited.
 *
 * Build: cc -O2 -o grab grab.c -lX11 -lz
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static int chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
	unsigned char hdr[8];
	put32(hdr, len);
	memcpy(hdr + 4, type, 4);
	uLong crc = crc32(0, (const Bytef *)type, 4);
	if (len)
		crc = crc32(crc, data, len);
	unsigned char tail[4];
	put32(tail, (uint32_t)crc);
	return fwrite(hdr, 1, 8, f) == 8 && (!len || fwrite(data, 1, len, f) == len) &&
	       fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

static int shift_of(unsigned long m)
{
	int s = 0;
	while (m && !(m & 1)) {
		m >>= 1;
		s++;
	}
	return s;
}

static int bits_of(unsigned long m)
{
	int b = 0;
	for (; m; m >>= 1)
		b += m & 1;
	return b;
}

int main(int argc, char **argv)
{
	int x = 0, y = 0;
	unsigned w = 0, h = 0;
	const char *out = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-g") && i + 1 < argc) {
			if (sscanf(argv[++i], "%ux%u+%d+%d", &w, &h, &x, &y) != 4) {
				fprintf(stderr, "grab: bad geometry %s\n", argv[i]);
				return 2;
			}
		} else {
			out = argv[i];
		}
	}
	if (!out) {
		fprintf(stderr, "usage: grab [-g WxH+X+Y] FILE|-\n");
		return 2;
	}
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "grab: cannot open the X display\n");
		return 1;
	}
	Window root = DefaultRootWindow(d);
	XWindowAttributes ra;
	XGetWindowAttributes(d, root, &ra);
	if (!w || !h) {
		w = ra.width;
		h = ra.height;
		x = y = 0;
	}
	/* Rows prefixed with filter type 0, then deflated. */
	size_t raw_len = (size_t)h * (1 + 3 * (size_t)w);
	unsigned char *raw = malloc(raw_len), *p = raw;
	enum { STRIP = 64 }; /* rows per XGetImage, see the top comment */
	for (unsigned y0 = 0; y0 < h; y0 += STRIP) {
		unsigned n = h - y0 < STRIP ? h - y0 : STRIP;
		XImage *im = XGetImage(d, root, x, y + (int)y0, w, n, AllPlanes, ZPixmap);
		if (!im) {
			fprintf(stderr, "grab: XGetImage failed\n");
			return 1;
		}
		int rs = shift_of(im->red_mask), gs = shift_of(im->green_mask), bs = shift_of(im->blue_mask);
		int rb = bits_of(im->red_mask), gb = bits_of(im->green_mask), bb = bits_of(im->blue_mask);
		for (unsigned yy = 0; yy < n; yy++) {
			*p++ = 0;
			for (unsigned xx = 0; xx < w; xx++) {
				unsigned long px = XGetPixel(im, xx, yy);
				unsigned long r = (px & im->red_mask) >> rs, g = (px & im->green_mask) >> gs,
					      b = (px & im->blue_mask) >> bs;
				*p++ = rb > 8 ? r >> (rb - 8) : r << (8 - rb);
				*p++ = gb > 8 ? g >> (gb - 8) : g << (8 - gb);
				*p++ = bb > 8 ? b >> (bb - 8) : b << (8 - bb);
			}
		}
		XDestroyImage(im);
	}
	XCloseDisplay(d);
	uLongf zlen = compressBound(raw_len);
	unsigned char *z = malloc(zlen);
	if (compress2(z, &zlen, raw, raw_len, 6) != Z_OK) {
		fprintf(stderr, "grab: compression failed\n");
		return 1;
	}
	free(raw);

	FILE *f = strcmp(out, "-") ? fopen(out, "wb") : stdout;
	if (!f) {
		perror(out);
		return 1;
	}
	static const unsigned char sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
	unsigned char ihdr[13];
	put32(ihdr, w);
	put32(ihdr + 4, h);
	ihdr[8] = 8;  /* bits per channel */
	ihdr[9] = 2;  /* RGB */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	int bad = fwrite(sig, 1, 8, f) != 8 || chunk(f, "IHDR", ihdr, 13) ||
		  chunk(f, "IDAT", z, (uint32_t)zlen) || chunk(f, "IEND", NULL, 0);
	free(z);
	if (f != stdout)
		bad |= fclose(f) != 0;
	else
		bad |= fflush(f) != 0;
	if (bad) {
		fprintf(stderr, "grab: write failed\n");
		return 1;
	}
	return 0;
}
