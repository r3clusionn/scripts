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

  A plan you pick stays when you plug in or unplug. `auto` follows the charger: balanced on battery, performance plugged in. A launcher can ask for `game` and `restore`; those leave a picked plan alone. The bar shows `⚙ bal` (picked) or `⚙ auto:bal`.
- **`audio on|off|toggle|ensure`:** PipeWire and WirePlumber on demand (bound to F2). `ensure` starts them for whoever needs sound: the volume keys, Bluetooth, media players.
- **`vol`:** a `wpctl` wrapper for the volume and mute keys that starts audio first.
- **`bl +5|-5|N`:** the backlight in percent, without xbacklight. A udev rule makes `brightness` writable by the video group.
- **`bt-toggle`:** Bluetooth power, starting and stopping `bluetoothd` with it, so it only runs while Bluetooth is on. Every `bluetoothctl` call has a timeout.
- **`update`:** sync, review and update @world, rebuild what links to replaced libraries, depclean, news, and list config updates without merging them.
- **udev:** power plans follow the charger; runtime power management for every PCI device except the Wi-Fi card.
- **dwm:** the `config.h` that binds these to keys (media keys, F1 Bluetooth, F2 audio, Alt+W browser, Alt+F fullscreen), and the `xinitrc` that starts the bar and a session bus without a resident parent.

## How to install

Requires a POSIX shell, doas, OpenRC, PipeWire with WirePlumber (`wpctl`), BlueZ, and `pkill`. `plan-bench` needs openssl.

```sh
git clone https://github.com/r3clusionn/scripts
cd scripts
doas sh install.sh                     # scripts, udev rules, boot script, doas rules for you
mkdir -p ~/cmds && cp cmds/* ~/cmds/   # the power plan commands
echo 'export PATH="$HOME/cmds:$PATH"' >> ~/.bashrc
```

`install.sh` adds doas rules so that you can run `powerprofile` and start or stop the Bluetooth service without a password.

## How to use

```sh
balanced            # pick a plan (battery, balanced, performance, gaming)
auto                # back to following the charger
status              # what is set now
audio toggle        # PipeWire on or off
bl +10              # backlight up 10%
doas update         # full system update
doas sh sbin/plan-bench   # measure each plan's power and speed
```

In dwm the keys call them directly (see `dwm/config.h`).

## How it works

- **Power plans** write sysfs:
  - the CPU energy-performance preference and maximum frequency per policy (amd-pstate active);
  - the global boost switch;
  - the two platform profiles, amd-pmf (the chip's power limits) and hp-wmi (the firmware's fan and thermal profile);
  - the GPU's DPM level.

  The current plan, and whether it was picked, is kept in `/run/powerprofile`. udev calls `powerprofile plug`, which does nothing while a plan is picked. `/etc/local.d/power.start` applies the automatic plan at boot.
- **Audio on demand:** `audio ensure` starts the PipeWire launcher detached (closing the toggle lock's descriptor so the daemons don't keep it) and waits until the default sink exists. One press of the F2 key fires the key macro several times, so `toggle` holds a lock for a second and drops repeats.
- **Bluetooth:** `bt-toggle` checks that `bluetoothd` is running before asking `bluetoothctl` anything, because `bluetoothctl` waits forever for a missing daemon.
- **The bar** is updated with `pkill -USR1 -x dwmstat` after every change, so it never polls.

## Measurements

Power plans on battery (`plan-bench`; CPU package power from RAPL, throughput from `openssl speed` SHA-256 on 16 kB blocks, 5 s each):

| Plan | Idle | 1 core | 24 cores |
|---|---|---|---|
| battery | 2.1 W | 973 MB/s at 3.7 W | 17814 MB/s at 9.7 W |
| balanced | 2.3 W | 1727 MB/s at 4.4 W | 26922 MB/s at 21.0 W |
| performance | 2.2 W | 1847 MB/s at 4.7 W | 27642 MB/s at 24.6 W |
| gaming | 4.7 W | 1858 MB/s at 7.8 W | 27418 MB/s at 28.5 W |

- **Plugged in,** the firmware allows about 62 W in performance (34500 MB/s on 24 cores). On battery it holds the package near 25–28 W even in performance, which is why balanced and performance are close there.
- **Gaming** gives some CPU throughput to the GPU: its clocks are pinned high, which helps games and costs about 1.5 W at idle.
- **Whole laptop idle on battery** (the battery's own reading over 20 s): battery 3.9 W, balanced 3.7 W, performance 3.8 W, gaming 5.3 W. The screen dominates at idle; the plans differ under load.

## Verification

- Every plan applied on the target laptop and read back with `status`: the EPP, boost, frequency cap, both platform profiles and the GPU level.
- A picked plan stayed through simulated charger events (`powerprofile plug ac` and `plug battery`). After `auto` it followed the charger. `game` and `restore` switched only while automatic.
- The bar showed `⚙ game` and `⚙ auto:bal` within 0.3 s of switching.
- `bl +5` and `bl -5` changed the backlight and the bar within 0.2 s.

## Limits

- Written for one laptop: the backlight device (`amdgpu_bl1`), the charger (`ACAD`), and the HP and AMD platform profiles are this machine's. Other laptops need those names changed.
- The refresh rate isn't part of the plans: the system has no `xrandr`.
- OpenRC and doas, not systemd and sudo.

## License

MIT (see `LICENSE`).
