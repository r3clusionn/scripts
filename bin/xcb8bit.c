/*
 * xcb8bit: LD_PRELOAD shim that gives Thorium 8-bit windows on a depth-30 X.
 *
 *   LD_PRELOAD=/usr/local/lib/xcb8bit.so XCB8BIT_EXE=/opt/thorium/thorium \
 *     thorium --use-angle=vulkan --enable-features=Vulkan,VulkanFromANGLE,\
 *     DefaultANGLEVulkan,VaapiVideoDecoder,VaapiIgnoreDriverChecks,\
 *     AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL
 *
 * At depth 30 Chromium's ANGLE finds no EGL config for the 10-bit root
 * visual, turns GPU compositing off and composites on the CPU (and converts
 * every video frame to RGB in the renderer). This makes the process see the
 * 32-bit ARGB visual as the root visual, with a default colormap made for it,
 * so its windows are 8-bit and ANGLE composites on the GPU.
 * Chromium leaves the colormap out of CreateWindow for the default visual
 * (CopyFromParent would BadMatch against the 30-bit root), so those requests
 * get ours added. The server stays at depth 30 and composites the 32-bit
 * windows itself. ANGLE over EGL still has no 8-bit config, and with ANGLE's
 * GLX backend every fullscreen drops out after 1.5 s, so use ANGLE over
 * Vulkan; it needs the Vulkan/VA-API interop features above, or the GPU
 * process dies on the first hardware-decoded frame.
 *
 * It only acts in the program named by XCB8BIT_EXE: LD_PRELOAD is inherited
 * by whatever Thorium starts (xdg-open, mpv), and those keep 10 bits.
 *
 * Build: cc -O2 -shared -fPIC -o xcb8bit.so xcb8bit.c -ldl -lxcb
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#define MAXCONN 16
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { xcb_connection_t *c; xcb_visualid_t vis; xcb_colormap_t cmap; } conns[MAXCONN];
static int enabled;

__attribute__((constructor)) static void init(void) {
	const char *want = getenv("XCB8BIT_EXE");
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (!want || n <= 0)
		return;
	exe[n] = 0;
	enabled = !strcmp(exe, want);
}

/* Make the 8888 TrueColor visual of depth 32 the root visual of a depth-30 screen. */
static void patch(xcb_connection_t *c, xcb_setup_t *setup, int slot) {
	xcb_screen_t *scr = xcb_setup_roots_iterator(setup).data;
	if (!scr || scr->root_depth != 30)
		return;
	for (xcb_depth_iterator_t d = xcb_screen_allowed_depths_iterator(scr); d.rem; xcb_depth_next(&d)) {
		if (d.data->depth != 32)
			continue;
		for (xcb_visualtype_iterator_t v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v)) {
			xcb_visualtype_t *vt = v.data;
			if (vt->_class != XCB_VISUAL_CLASS_TRUE_COLOR || vt->red_mask != 0xff0000 ||
			    vt->green_mask != 0xff00 || vt->blue_mask != 0xff)
				continue;
			xcb_colormap_t cmap = xcb_generate_id(c);
			xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, cmap, scr->root, vt->visual_id);
			scr->root_depth = 32;
			scr->root_visual = vt->visual_id;
			scr->default_colormap = cmap;
			scr->white_pixel = 0xffffffff;
			scr->black_pixel = 0xff000000;
			conns[slot].vis = vt->visual_id;
			conns[slot].cmap = cmap;
			return;
		}
	}
}

static int find(xcb_connection_t *c) {
	for (int i = 0; i < MAXCONN; i++)
		if (conns[i].c == c)
			return i;
	return -1;
}

const struct xcb_setup_t *xcb_get_setup(xcb_connection_t *c) {
	static const struct xcb_setup_t *(*real)(xcb_connection_t *);
	if (!real)
		real = dlsym(RTLD_NEXT, "xcb_get_setup");
	xcb_setup_t *setup = (xcb_setup_t *)real(c);
	if (!enabled || !setup || xcb_connection_has_error(c))
		return setup;
	pthread_mutex_lock(&lock);
	if (find(c) < 0) {
		int slot = find(NULL);
		if (slot >= 0) {
			conns[slot].c = c;
			patch(c, setup, slot);
		}
	}
	pthread_mutex_unlock(&lock);
	return setup;
}

void xcb_disconnect(xcb_connection_t *c) {
	static void (*real)(xcb_connection_t *);
	if (!real)
		real = dlsym(RTLD_NEXT, "xcb_disconnect");
	if (enabled) {
		pthread_mutex_lock(&lock);
		int i = find(c);
		if (i >= 0)
			memset(&conns[i], 0, sizeof conns[i]);
		pthread_mutex_unlock(&lock);
	}
	real(c);
}

/* Add our colormap to CreateWindow requests for the substituted visual. */
unsigned int xcb_send_request(xcb_connection_t *c, int flags, struct iovec *vector,
                              const xcb_protocol_request_t *req) {
	static unsigned int (*real)(xcb_connection_t *, int, struct iovec *, const xcb_protocol_request_t *);
	if (!real)
		real = dlsym(RTLD_NEXT, "xcb_send_request");
	if (!enabled)
		return real(c, flags, vector, req);
	/* Chromium sends raw requests: the opcode is only in the header bytes. */
	int raw = flags & XCB_REQUEST_RAW;
	if (raw ? !req->count || !vector[0].iov_len || ((uint8_t *)vector[0].iov_base)[0] != XCB_CREATE_WINDOW
	        : req->ext || req->opcode != XCB_CREATE_WINDOW)
		return real(c, flags, vector, req);
	pthread_mutex_lock(&lock);
	int i = find(c);
	xcb_visualid_t vis = i >= 0 ? conns[i].vis : 0;
	xcb_colormap_t cmap = i >= 0 ? conns[i].cmap : 0;
	pthread_mutex_unlock(&lock);
	if (!vis)
		return real(c, flags, vector, req);

	size_t len = 0;
	for (size_t k = 0; k < req->count; k++)
		len += vector[k].iov_len;
	if (len < 32)
		return real(c, flags, vector, req);
	uint8_t *buf = malloc(len + 4);
	if (!buf)
		return real(c, flags, vector, req);
	size_t off = 0;
	for (size_t k = 0; k < req->count; k++) {
		memcpy(buf + off, vector[k].iov_base, vector[k].iov_len);
		off += vector[k].iov_len;
	}
	/* CreateWindow: visual at 24, value-mask at 28, then one word per mask bit in bit order. */
	uint32_t visual, mask;
	memcpy(&visual, buf + 24, 4);
	memcpy(&mask, buf + 28, 4);
	if (visual != vis || len != 32 + 4 * (size_t)__builtin_popcount(mask)) {
		free(buf);
		return real(c, flags, vector, req);
	}
	uint32_t *vals = (uint32_t *)(buf + 32);
	int idx = __builtin_popcount(mask & (XCB_CW_COLORMAP - 1));
	if (mask & XCB_CW_COLORMAP) {
		if (!vals[idx])
			vals[idx] = cmap;
	} else {
		memmove(&vals[idx + 1], &vals[idx], 4 * (__builtin_popcount(mask) - idx));
		vals[idx] = cmap;
		mask |= XCB_CW_COLORMAP;
		memcpy(buf + 28, &mask, 4);
		len += 4;
		if (raw) {
			uint16_t words = len / 4;
			memcpy(buf + 2, &words, 2);
		}
	}
	/* xcb may write to vector[-1] and vector[-2]. */
	struct iovec iov[3] = {{0}, {0}, {buf, len}};
	xcb_protocol_request_t r = *req;
	r.count = 1;
	unsigned int seq = real(c, flags, &iov[2], &r);
	free(buf);
	return seq;
}
