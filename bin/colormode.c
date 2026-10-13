/*
 * colormode: accurate sRGB colours on a wide-gamut panel, in the display
 * hardware.
 *
 *   colormode srgb     sRGB content shows as sRGB (the default at login)
 *   colormode native   the panel's full gamut, colours unmanaged
 *   colormode status   what is set, and the panel's primaries
 *
 * Almost everything (the desktop, browsers, video) is sRGB or BT.709, which
 * share primaries. Sent unchanged to a panel with a wider gamut (this
 * laptop's OLED is about DCI-P3 with a greener green), every colour comes out
 * oversaturated. `srgb` makes the GPU's display pipeline convert each pixel:
 * the sRGB curve is undone (DEGAMMA_LUT), a 3x3 matrix maps sRGB primaries
 * onto the panel's (CTM), and the same curve is applied again (GAMMA_LUT).
 * The matrix comes from the primaries in the panel's EDID, with the white
 * point kept at D65, so greys come out exactly as they went in. The work is
 * done by fixed-function hardware after the framebuffer, at more than 10 bits,
 * so it costs no GPU time or power and adds no banding.
 *
 * The curves are sRGB's, not a pure gamma 2.2: the kernel turns GAMMA_LUT
 * (4096 points evenly spaced in linear light) into the hardware's curve by
 * linear interpolation, and sRGB's straight segment near black survives that
 * exactly. A pure power curve would lose the darkest shades (10-bit codes 1-4
 * came out black when simulated; with sRGB every grey is within 0.1 code).
 *
 * The amdgpu X driver exposes these as RandR output properties. `native`
 * removes the LUTs and sets an identity matrix (for HDR, which needs the full
 * gamut, or to compare).
 *
 * Build: cc -O2 -o colormode colormode.c -lX11 -lXrandr -lm
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <glob.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef double mat3[3][3];

static int lut_size(Display *d, RROutput o, const char *name)
{
	Atom a = XInternAtom(d, name, True), type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o, a, 0, 1, False, False, AnyPropertyType, &type,
					      &fmt, &n, &after, &data) != Success || !data || n < 1)
		return 0;
	int v = (int)((long *)data)[0];
	XFree(data);
	return v;
}

/* The output's EDID, from sysfs: the amdgpu X driver doesn't publish it in
 * RandR, but it does publish the DRM connector's id. */
static int read_edid(Display *d, RROutput o, unsigned char *e, size_t n)
{
	Atom a = XInternAtom(d, "CONNECTOR_ID", True), type;
	int fmt;
	unsigned long cnt, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o, a, 0, 1, False, False, AnyPropertyType, &type,
					      &fmt, &cnt, &after, &data) != Success || !data || cnt < 1)
		return -1;
	long want = ((long *)data)[0];
	XFree(data);
	glob_t g;
	int ok = -1;
	if (glob("/sys/class/drm/card*-*/connector_id", 0, NULL, &g) != 0)
		return -1;
	for (size_t i = 0; i < g.gl_pathc && ok < 0; i++) {
		FILE *f = fopen(g.gl_pathv[i], "r");
		long id = -1;
		if (f) {
			if (fscanf(f, "%ld", &id) != 1)
				id = -1;
			fclose(f);
		}
		if (id != want)
			continue;
		char path[512];
		snprintf(path, sizeof path, "%.*s/edid", (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]),
			 g.gl_pathv[i]);
		f = fopen(path, "rb");
		if (f) {
			ok = fread(e, 1, n, f) >= 128 ? 0 : -1;
			fclose(f);
		}
	}
	globfree(&g);
	return ok;
}

/* EDID base block chromaticity: R, G, B, W as x, y with 10-bit precision. */
static int edid_primaries(Display *d, RROutput o, double xy[4][2])
{
	unsigned char e[128];
	if (read_edid(d, o, e, sizeof e))
		return -1;
	int lo1 = e[25], lo2 = e[26];
	int bits[8] = {(lo1 >> 6) & 3, (lo1 >> 4) & 3, (lo1 >> 2) & 3, lo1 & 3,
		       (lo2 >> 6) & 3, (lo2 >> 4) & 3, (lo2 >> 2) & 3, lo2 & 3};
	for (int i = 0; i < 8; i++)
		xy[i / 2][i % 2] = ((e[27 + i] << 2) | bits[i]) / 1024.0;
	return 0;
}

static void invert(mat3 m, mat3 out)
{
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
		     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			int a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, e = (i + 2) % 3;
			out[i][j] = (m[a][c] * m[b][e] - m[a][e] * m[b][c]) / det;
		}
}

/* RGB -> XYZ for primaries (x, y) and white point (x, y). */
static void rgb_to_xyz(const double p[3][2], const double w[2], mat3 out)
{
	mat3 m, inv;
	for (int c = 0; c < 3; c++) {
		m[0][c] = p[c][0] / p[c][1];
		m[1][c] = 1;
		m[2][c] = (1 - p[c][0] - p[c][1]) / p[c][1];
	}
	invert(m, inv);
	double W[3] = {w[0] / w[1], 1, (1 - w[0] - w[1]) / w[1]}, s[3];
	for (int i = 0; i < 3; i++)
		s[i] = inv[i][0] * W[0] + inv[i][1] * W[1] + inv[i][2] * W[2];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			out[r][c] = m[r][c] * s[c];
}

/* DRM's S31.32 sign-magnitude, as two 32-bit halves (low first) for RandR. */
static void put_s3132(long *out, double v)
{
	uint64_t mag = (uint64_t)llround(fabs(v) * 4294967296.0);
	if (v < 0)
		mag |= 1ULL << 63;
	out[0] = (long)(mag & 0xffffffffu);
	out[1] = (long)(mag >> 32);
}

static void set_ctm(Display *d, RROutput o, mat3 m)
{
	long data[18];
	for (int i = 0; i < 9; i++)
		put_s3132(data + 2 * i, m[i / 3][i % 3]);
	XRRChangeOutputProperty(d, o, XInternAtom(d, "CTM", False), XA_INTEGER, 32,
				PropModeReplace, (unsigned char *)data, 18);
}

static double srgb_eotf(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double srgb_oetf(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055; }

/* curve NULL: back to the default (no LUT). */
static void set_lut(Display *d, RROutput o, const char *name, int n, double (*curve)(double))
{
	Atom a = XInternAtom(d, name, False);
	if (!curve || n < 2) {
		short zero = 0;
		XRRChangeOutputProperty(d, o, a, XA_INTEGER, 16, PropModeReplace, (unsigned char *)&zero, 1);
		return;
	}
	short *lut = calloc((size_t)n * 4, sizeof *lut);
	for (int i = 0; i < n; i++) {
		unsigned short v = (unsigned short)lround(curve((double)i / (n - 1)) * 65535.0);
		lut[4 * i] = lut[4 * i + 1] = lut[4 * i + 2] = (short)v;
	}
	XRRChangeOutputProperty(d, o, a, XA_INTEGER, 16, PropModeReplace, (unsigned char *)lut, n * 4);
	free(lut);
}

/*
 * At depth 24 the X driver composes GAMMA_LUT with the legacy gamma ramp
 * (1024 entries), and its default ramp (k * 64, up to 65472) is short of full
 * scale: lookups land one entry low from the very first, which turns the
 * darkest shade black. A ramp rounded up to full scale keeps the darkest
 * entries exact; above about 0.4% of full light the lookup can still land one
 * entry (at most 0.3%) low. At depth 30 the legacy ramp isn't used and
 * GAMMA_LUT reaches the hardware as it is.
 */
static void exact_legacy_ramp(Display *d, RRCrtc crtc)
{
	int n = XRRGetCrtcGammaSize(d, crtc);
	if (n < 2)
		return;
	XRRCrtcGamma *g = XRRAllocGamma(n);
	for (int k = 0; k < n; k++)
		g->red[k] = g->green[k] = g->blue[k] =
			(unsigned short)((k * 65535UL + (unsigned long)(n - 2)) / (unsigned long)(n - 1));
	XRRSetCrtcGamma(d, crtc, g);
	XRRFreeGamma(g);
}

static int ctm_is_identity(Display *d, RROutput o)
{
	Atom a = XInternAtom(d, "CTM", True), type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o, a, 0, 18, False, False, AnyPropertyType, &type,
					      &fmt, &n, &after, &data) != Success || !data || n != 18)
		return 1;
	long *v = (long *)data;
	int id = 1;
	for (int i = 0; i < 9; i++) {
		uint64_t m = ((uint64_t)(uint32_t)v[2 * i + 1] << 32) | (uint32_t)v[2 * i];
		if (m != (i % 4 == 0 ? 1ULL << 32 : 0))
			id = 0;
	}
	XFree(data);
	return id;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "status";
	if (strcmp(mode, "srgb") && strcmp(mode, "native") && strcmp(mode, "status")) {
		fprintf(stderr, "usage: colormode srgb|native|status\n");
		return 2;
	}
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "colormode: cannot open the X display\n");
		return 1;
	}
	XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, DefaultRootWindow(d));
	int done = 0;
	for (int i = 0; res && i < res->noutput; i++) {
		RROutput o = res->outputs[i];
		XRROutputInfo *info = XRRGetOutputInfo(d, res, o);
		int connected = info && info->connection == RR_Connected && info->crtc;
		RRCrtc crtc = info ? info->crtc : None;
		char name[64];
		snprintf(name, sizeof name, "%s", info ? info->name : "?");
		if (info)
			XRRFreeOutputInfo(info);
		int glut = lut_size(d, o, "GAMMA_LUT_SIZE"), dlut = lut_size(d, o, "DEGAMMA_LUT_SIZE");
		double xy[4][2];
		if (!connected || !glut || !dlut || edid_primaries(d, o, xy))
			continue;
		const double srgb[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
		const double d65[2] = {0.3127, 0.3290};
		if (!strcmp(mode, "status")) {
			printf("%s: %s\n", name, ctm_is_identity(d, o) ? "native" : "srgb");
			printf("  panel primaries R %.3f,%.3f  G %.3f,%.3f  B %.3f,%.3f  white %.4f,%.4f\n",
			       xy[0][0], xy[0][1], xy[1][0], xy[1][1], xy[2][0], xy[2][1], xy[3][0], xy[3][1]);
			done = 1;
			continue;
		}
		if (!strcmp(mode, "native")) {
			mat3 id = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
			exact_legacy_ramp(d, crtc);
			set_lut(d, o, "DEGAMMA_LUT", dlut, NULL);
			set_ctm(d, o, id);
			set_lut(d, o, "GAMMA_LUT", glut, NULL);
		} else {
			mat3 to_xyz, panel, from_xyz, m;
			rgb_to_xyz(srgb, d65, to_xyz);
			rgb_to_xyz((const double(*)[2])xy, d65, panel);
			invert(panel, from_xyz);
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					m[r][c] = from_xyz[r][0] * to_xyz[0][c] + from_xyz[r][1] * to_xyz[1][c] +
						  from_xyz[r][2] * to_xyz[2][c];
			exact_legacy_ramp(d, crtc);
			set_lut(d, o, "DEGAMMA_LUT", dlut, srgb_eotf);
			set_ctm(d, o, m);
			set_lut(d, o, "GAMMA_LUT", glut, srgb_oetf);
		}
		printf("%s: %s\n", name, mode);
		done = 1;
	}
	XSync(d, False);
	XCloseDisplay(d);
	if (!done) {
		fprintf(stderr, "colormode: no connected output with colour management\n");
		return 1;
	}
	return 0;
}
