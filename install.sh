#!/bin/sh
# Installs everything (needs root): the scripts to /usr/local/bin and
# /usr/local/sbin (colormode and grab are compiled there; the xcb8bit.so Thorium shim and the colormode-hdr.so mpv plugin go to /usr/local/lib), the udev rules, the boot script, and the doas rules they
# need for the user running doas. The power plan commands (cmds/) are the
# user's: copy them to ~/cmds and put that on PATH.
# Run: doas sh install.sh
set -eu
[ "$(id -u)" = 0 ] || { echo "run with: doas sh $0" >&2; exit 1; }
user=${DOAS_USER:-${SUDO_USER:-}}
[ -n "$user" ] || { echo "run it through doas (or sudo) so the doas rules name you" >&2; exit 1; }
cd "$(dirname "$(readlink -f "$0")")"
install -m 755 bin/audio bin/vol bin/bl bin/bt-toggle /usr/local/bin/
cc -O2 -o /usr/local/bin/colormode bin/colormode.c -lX11 -lXrandr -lm
cc -O2 -shared -fPIC -o /usr/local/lib/colormode-hdr.so bin/colormode-hdr.c -lX11
cc -O2 -o /usr/local/bin/grab bin/grab.c -lX11 -lz
mkdir -p /usr/local/lib
cc -O2 -shared -fPIC -o /usr/local/lib/xcb8bit.so bin/xcb8bit.c -ldl -lxcb
install -m 755 sbin/update sbin/powerprofile /usr/local/sbin/
install -m 644 udev/*.rules /etc/udev/rules.d/
install -m 755 local.d/power.start /etc/local.d/
udevadm control --reload 2>/dev/null || true
for rule in \
  "permit nopass $user as root cmd /usr/bin/rc-service args bluetooth start" \
  "permit nopass $user as root cmd /usr/bin/rc-service args bluetooth stop" \
  "permit nopass $user as root cmd /usr/local/sbin/powerprofile"; do
  grep -qxF "$rule" /etc/doas.conf || echo "$rule" >> /etc/doas.conf
done
doas -C /etc/doas.conf
/usr/local/sbin/powerprofile auto
echo "installed; power plan: $(cut -d' ' -f1 /run/powerprofile)"
