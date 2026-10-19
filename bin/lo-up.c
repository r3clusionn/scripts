/*
 * lo-up: bring the loopback interface up. The kernel adds 127.0.0.1/8 and ::1
 * itself when lo goes up (net/ipv4/devinet.c inetdev_event, NETDEV_UP), so this
 * is all OpenRC's loopback service needs; it replaces iproute2 for that.
 */
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

int main(void)
{
	struct ifreq r;
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (s < 0) {
		perror("lo-up: socket");
		return 1;
	}
	memset(&r, 0, sizeof r);
	strcpy(r.ifr_name, "lo");
	if (ioctl(s, SIOCGIFFLAGS, &r) < 0) {
		perror("lo-up: SIOCGIFFLAGS");
		return 1;
	}
	if (r.ifr_flags & IFF_UP)
		return 0;
	r.ifr_flags |= IFF_UP;
	if (ioctl(s, SIOCSIFFLAGS, &r) < 0) {
		perror("lo-up: SIOCSIFFLAGS");
		return 1;
	}
	return 0;
}
