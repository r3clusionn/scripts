# scripts

The small shell scripts that run a minimal Gentoo laptop with dwm: power plans from battery to gaming, audio that starts only when something needs it, volume, backlight and Bluetooth keys, and a one-command system update. They are made for one laptop (HP OmniBook 7, Ryzen AI 9 HX 475, Gentoo with dwm and no desktop environment), where PipeWire and Bluetooth don't run until used and nothing polls in the background. The status bar ([dwmstat](https://github.com/r3clusionn/dwmstat)) is told about every change by a signal.

**Status:** in daily use on the target laptop. The power plans were measured on it (see Measurements).

![Terminal output on a dark background: "$ balanced; status" prints "plan: balanced (pinned)", "cpu: epp balance_power, boost 1, max 3600 MHz", "amd-pmf: balanced", "hp-wmi: balanced", "gpu: auto"; then a plan-bench table measured on battery: battery 2.1 W idle, 973 MB/s on one core at 3.7 W, 17814 MB/s on 24 cores at 9.7 W; balanced 2.3 W, 1727 MB/s at 4.4 W, 26922 MB/s at 21.0 W; performance 2.2 W, 1847 MB/s at 4.7 W, 27642 MB/s at 24.6 W; gaming 4.7 W, 1858 MB/s at 7.8 W, 27418 MB/s at 28.5 W](docs/images/plans.png)

## Features

- **Power plans** (`powerprofile`, and `battery`, `balanced`, `performance`, `gaming`, `auto`, `status` in `cmds/`), from least to most power:

  | Plan | CPU preference | Boost | Platform (amd-pmf / hp-wmi) | GPU clocks |
  |---|---|---|---|---|
  | battery | power | off (~2 GHz) | low-power / cool | automatic |
  | balanced | balance_power | up to 3.6 GHz | balanced / balanced | automatic |
  | performance | balance_performance | full (5.3 GHz) | performance / performance | automatic |
  | gaming | performance | full | performance / performance | pinned high |

  Every plan also runs the SSD's PCIe link at Gen4. The firmware sets Gen3, which halves its read speed; Gen4 costs nothing measurable at idle and uses half the energy per gigabyte read (see Measurements).

  A plan you pick stays when you plug in or unplug. `auto` follows the charger: balanced on battery, performance plugged in. A launcher can ask for `game` and `restore`; those leave a picked plan alone. The bar shows `⚙ bal` (picked) or `⚙ auto:bal`.
- **`audio on|off|toggle|ensure`:** PipeWire and WirePlumber on demand (bound to F2). `ensure` starts them for whoever needs sound: the volume keys, Bluetooth, media players.
- **`vol`:** a `wpctl` wrapper for the volume and mute keys that starts audio first.
- **`bl +5|-5|N`:** the backlight in percent, without xbacklight. A udev rule makes `brightness` writable by the video group.
- **`bt-toggle`:** Bluetooth power, starting and stopping `bluetoothd` with it, so it only runs while Bluetooth is on. Every `bluetoothctl` call has a timeout.
- **`colormode srgb|native|status`:** accurate colours on the wide-gamut OLED. Nearly everything (desktop, browser, video) is sRGB or BT.709, which looks oversaturated on a panel with a gamut near DCI-P3. `srgb` (set at login by `xinitrc`) converts it in the display hardware, from the primaries in the panel's EDID; `native` shows the panel's full gamut.
- **`grab [-g WxH+X+Y] FILE|-`:** a screen region as PNG at any X depth (shotgun writes nothing at depth 30). The region screenshot key uses it with hacksaw and xclip.
- **`update`:** sync, review and update @world, rebuild what links to replaced libraries, depclean, news, and list config updates without merging them.
- **udev:** power plans follow the charger; runtime power management for every PCI device except the Wi-Fi card.
- **dwm:** the `config.h` that binds these to keys (media keys, F1 Bluetooth, F2 audio, Alt+W browser, Alt+F fullscreen), and the `xinitrc` that starts the bar and a session bus without a resident parent.

## How to install

Requires a POSIX shell, doas, OpenRC, PipeWire with WirePlumber (`wpctl`), BlueZ, `pkill`, and a C compiler with libX11, libXrandr and zlib (for `colormode` and `grab`). The benchmark needs openssl and a C compiler with libX11 and libXext.

```sh
git clone https://github.com/r3clusionn/scripts
cd scripts
doas sh install.sh                     # scripts, udev rules, boot script, doas rules for you
cp -r cmds ~/cmds                      # the power plan commands
cc -O2 -o ~/cmds/root/keepawake ~/cmds/root/keepawake.c -lX11 -lXext   # for ./bench
```

`install.sh` adds doas rules so that you can run `powerprofile` and start or stop the Bluetooth service without a password.

## How to use

```sh
cd ~/cmds
./balanced          # pick a plan (battery, balanced, performance, gaming)
./auto              # back to following the charger
./status            # what is set now
./bench             # measure every plan's power and speed (about 3 minutes)

audio toggle        # PipeWire on or off
bl +10              # backlight up 10%
colormode status    # sRGB or native colours, and the panel's primaries
doas update         # full system update
```

In dwm the keys call them directly (see `dwm/config.h`).

## How it works

- **Power plans** write sysfs:
  - the CPU energy-performance preference and maximum frequency per policy (amd-pstate active);
  - the global boost switch;
  - the two platform profiles, amd-pmf (the chip's power limits) and hp-wmi (the firmware's fan and thermal profile);
  - the GPU's DPM level;
  - the SSD's PCIe link speed, through the kernel's link-speed control for its root port (the `PCIe_Port_Link_Speed_<port>` cooling device: 0 is the fastest speed both ends support, 1 one step lower). `status` prints the current link speed.

  The current plan, and whether it was picked, is kept in `/run/powerprofile`. udev calls `powerprofile plug`, which does nothing while a plan is picked. `/etc/local.d/power.start` applies the automatic plan at boot.
- **Audio on demand:** `audio ensure` starts the PipeWire launcher detached (closing the toggle lock's descriptor so the daemons don't keep it) and waits until the default sink exists. One press of the F2 key fires the key macro several times, so `toggle` holds a lock for a second and drops repeats.
- **Bluetooth:** `bt-toggle` checks that `bluetoothd` is running before asking `bluetoothctl` anything, because `bluetoothctl` waits forever for a missing daemon.
- **Colour:** the amdgpu X driver exposes the CRTC's colour pipeline as RandR properties. `colormode srgb` sets DEGAMMA_LUT to the sRGB curve, CTM to a matrix from sRGB to the panel's primaries (white kept at D65, so greys are unchanged), and GAMMA_LUT back to the sRGB curve. The display engine does this after the framebuffer at more than 10 bits, so it costs no GPU time or power and adds no banding. The curves are sRGB's rather than a pure 2.2 power: the kernel builds the hardware curve by linear interpolation of a LUT spaced evenly in linear light, which keeps sRGB's straight segment near black exact (a pure power curve lost 10-bit codes 1–4 to black in a simulation). At depth 24 the driver composes GAMMA_LUT with X's legacy gamma ramp, so `colormode` also sets that ramp to full scale; X's default ramp shifts every lookup one entry low and turns the darkest shade black.
- **The bar** is updated with `pkill -USR1 -x dwmstat` after every change, so it never polls.

## Measurements

`./bench` runs `root/plan-bench` for each plan: 20 s idle, then `openssl speed` SHA-256 on 16 kB blocks for 8 s on one core and on all 24. It reads CPU package power from RAPL and, on battery, whole-laptop power from the battery. `root/keepawake` keeps the screen on meanwhile (no screen saver or DPMS), so a blanked screen doesn't lower the laptop's numbers, and restores the timeout after.

Plugged in (backlight 100%):

| Plan | Idle CPU | 1 core | 24 cores |
|---|---|---|---|
| battery | 3.6 W | 968 MB/s at 4.3 W | 17010 MB/s at 11.8 W |
| balanced | 3.7 W | 1690 MB/s at 5.5 W | 29155 MB/s at 31.2 W |
| performance | 3.8 W | 2431 MB/s at 9.7 W | 34201 MB/s at 63.0 W |
| gaming | 5.3 W | 2536 MB/s at 13.5 W | 34557 MB/s at 63.0 W |

On battery (earlier version of the benchmark, 5 s runs):

| Plan | Idle | 1 core | 24 cores |
|---|---|---|---|
| battery | 2.1 W | 973 MB/s at 3.7 W | 17814 MB/s at 9.7 W |
| balanced | 2.3 W | 1727 MB/s at 4.4 W | 26922 MB/s at 21.0 W |
| performance | 2.2 W | 1847 MB/s at 4.7 W | 27642 MB/s at 24.6 W |
| gaming | 4.7 W | 1858 MB/s at 7.8 W | 27418 MB/s at 28.5 W |

- **Plugged in,** the firmware allows 63 W in performance and gaming. On battery it holds the package near 25–28 W even in performance, which is why balanced and performance are close there.
- **Gaming** keeps the GPU clocks high, which helps games and costs about 1.5 W at idle.
- **SSD link** (KIOXIA BG6, PCIe Gen4 x4, set to Gen3 by the firmware), on battery with the balanced plan:

  | Link | 4 parallel 2 GiB reads | One 6 GiB read | Idle, whole laptop (4 alternating rounds) | Package energy to read 6 GiB |
  |---|---|---|---|---|
  | Gen3 (firmware) | 3441 MiB/s | 3327 MiB/s | 4.55 W | 8.7 J in 1.87 s |
  | Gen4 | 6599 MiB/s | 6319 MiB/s | 4.44 W | 4.7 J in 0.99 s |

  At idle the link sits in ASPM L1 at either speed. Under load Gen4 draws the same power for half the time.
- **Whole-laptop power on battery** is being re-measured: the earlier readings were probably taken with the screen blanked by its 10-minute timeout.

## Verification

- Every plan applied on the target laptop and read back with `status`: the EPP, boost, frequency cap, both platform profiles and the GPU level.
- A picked plan stayed through simulated charger events (`powerprofile plug ac` and `plug battery`). After `auto` it followed the charger. `game` and `restore` switched only while automatic.
- The bar showed `⚙ game` and `⚙ auto:bal` within 0.3 s of switching.
- `bl +5` and `bl -5` changed the backlight and the bar within 0.2 s.
- SSD link: the root port and the SSD both advertise Gen4 x4; after switching, `current_link_speed` read 16.0 GT/s and back to 8.0 GT/s. The PCIe AER error counters of the SSD and its root port stayed at 0 through the read tests.

## Limits

- Written for one laptop: the backlight device (`amdgpu_bl1`), the charger (`ACAD`), and the HP and AMD platform profiles are this machine's. Other laptops need those names changed.
- The refresh rate isn't part of the plans: the system has no `xrandr`.
- OpenRC and doas, not systemd and sudo.

## License

MIT (see `LICENSE`).
