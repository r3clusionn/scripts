/* keepawake COMMAND...: keeps the screen on (no screen saver, no DPMS) while
 * COMMAND runs, then puts the previous settings back. Run it as the X user.
 * Build: cc -O2 -o keepawake keepawake.c -lX11 -lXext */
#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: keepawake COMMAND...\n");
		return 2;
	}
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "keepawake: no X display\n");
		return 1;
	}
	int timeout, interval, blank, expose, dummy, dpms = 0;
	CARD16 power, standby = 0, suspend = 0, off = 0;
	BOOL dpms_on = False;
	XGetScreenSaver(d, &timeout, &interval, &blank, &expose);
	if (DPMSQueryExtension(d, &dummy, &dummy) && DPMSCapable(d)) {
		dpms = 1;
		DPMSInfo(d, &power, &dpms_on);
		DPMSGetTimeouts(d, &standby, &suspend, &off);
	}
	fprintf(stderr, "keepawake: screen saver %ds, DPMS %s (%u/%u/%u s) -> off until done\n",
		timeout, dpms_on ? "on" : "off", standby, suspend, off);
	XSetScreenSaver(d, 0, interval, blank, expose);
	if (dpms)
		DPMSDisable(d);
	XForceScreenSaver(d, ScreenSaverReset); /* wake the screen if it's blank */
	XSync(d, False);

	signal(SIGINT, SIG_IGN); /* Ctrl-C reaches the command; we restore after it */
	signal(SIGHUP, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	int status = 1;
	pid_t pid = fork();
	if (pid == 0) {
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		close(ConnectionNumber(d));
		execvp(argv[1], argv + 1);
		perror(argv[1]);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, &status, 0) < 0)
			;
	XSetScreenSaver(d, timeout, interval, blank, expose);
	if (dpms) {
		DPMSSetTimeouts(d, standby, suspend, off);
		if (dpms_on)
			DPMSEnable(d);
	}
	XSync(d, False);
	XCloseDisplay(d);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
