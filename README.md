# US-1800 driver for Apple Silicon

An unofficial, userspace driver for the **TASCAM US-1800** on Apple Silicon Macs. Tascam's last driver is an x86-only kernel
extension and does not load on current macOS. This replaces it with:

- **`us1800d`**, a small daemon that talks to the unit over USB (IOKit, no kernel extension), and
- **`US1800.driver`**, a CoreAudio AudioServerPlugIn that shows up as a normal audio device in System Settings, Logic, Spotify, etc.

The two share audio through shared memory (`/us1800`). The protocol was reverse-engineered; see [PROTOCOL.md](PROTOCOL.md).

> Not affiliated with TASCAM. Use at your own risk. Tested on **one unit** only (firmware 1.49).

## Status

| Works | Not implemented |
|---|---|
| 14 analog inputs (1-14), 24-bit | Mixer / monitor routing, sample-clock source (use the unit's front panel) |
| 4 analog outputs, 24-bit | MIDI in/out |
| 48 kHz, Float32 in CoreAudio | Other sample rates (44.1/88.2/96 are known in the protocol, untested here) |
| Runs at login, restarts if it crashes | S/PDIF in/out as separate channels |
| ~21.6 ms round trip at a 64-frame buffer | Meters |

Mic gain, pad and +48 V phantom are hardware knobs/switches on the unit, so no software control is needed for those.

## What it runs on

- Apple Silicon Mac, macOS 15 (Darwin 24.6) - the only configuration tested
- TASCAM US-1800 connected **directly** to a USB port (high-speed). It is a vendor-specific device, not USB Audio Class
- Xcode command line tools (`clang`) to build. No other dependencies

## How it works

```
Logic / Spotify ──► CoreAudio ──► US1800.driver (in coreaudiod) ──shm ring──► us1800d ──USB──► US-1800
                                         ▲                                         │
                                         └────────────── capture ring ◄────────────┘
```

- **Playback:** isochronous OUT endpoint `0x02`, 4 channels of 24-bit packed audio. A realtime timer thread submits transfers
  1 ms apart, because USB completion callbacks arrive up to 30 ms late on macOS.
- **Capture:** bulk IN `0x86`, bit-sliced 128-byte frames (18 slots), decoded in the daemon. 64 reads are kept in flight; a
  single outstanding read loses about 1% of the data.
- **Clock:** the unit's clock runs slightly off the USB frame clock. The daemon reads the unit's feedback endpoint `0x81` and
  adjusts frames per millisecond to match; the plug-in absorbs the remaining drift with a 32-tap Kaiser-windowed-sinc resampler
  (at most ±0.1%). Without this the unit's FIFO overflows every ~51 s and you hear ~6 s of garbage.
- **Input mapping:** input *n* is capture slot *n-1*. Slot 14 carries a copy of output 1 and slots 15-17 are silent, so they
  are not exposed.

```
src/us1800d.c         the daemon (USB, rate matching, realtime output thread)
src/us1800_shm.h      shared-memory layout and the latency constants the plug-in reports
src/us1800_client.c   test client: rec / tone / stat
plugin/               the CoreAudio plug-in (resample.h = drift-correction filter)
launchd/              login item (plist + installer)
tools/                probes, loopback round-trip/tone tests, resampler test
PROTOCOL.md           the reverse-engineered USB protocol and the latency/stability findings
```

## Build and install

```sh
mkdir -p build
clang -O2 -Wall -o build/us1800d src/us1800d.c -framework IOKit -framework CoreFoundation -lpthread
clang -O2 -o build/us1800_client src/us1800_client.c -lm
sh plugin/build.sh                          # ad-hoc signed build/US1800.driver

sudo plugin/install.sh                      # copies the plug-in to /Library/Audio/Plug-Ins/HAL, restarts coreaudiod
launchd/install.sh                          # installs the login item and starts the daemon
```

The login item runs `build/us1800d` from where you cloned the repo, so keep the checkout in place (or edit
`launchd/local.us1800d.plist`, which has an absolute path).

Uninstall:

```sh
launchctl bootout gui/$(id -u)/local.us1800d && rm ~/Library/LaunchAgents/local.us1800d.plist
sudo rm -rf /Library/Audio/Plug-Ins/HAL/US1800.driver && sudo killall coreaudiod
```

## Settings

The daemon takes flags (the login item uses the defaults):

| Flag | Default | Meaning |
|---|---|---|
| `--queue-ms` | 9 | Milliseconds of audio queued on the USB wire. Absorbs macOS stalls of the USB clock |
| `--prefill` | 200 | Frames held between plug-in and daemon. Absorbs delays in coreaudiod |
| `--cushion` | 240 | Frames held on the capture side |
| `--out-channels` | 4 | 4 is the maximum (see Gotchas) |
| `--rate` | 48000 | |

Presets, measured with the loopback test at a 64-frame buffer:

| queue / prefill / cushion | Round trip | Behaviour |
|---|---|---|
| 5 / 100 / 120 | ~12.7 ms | one late transfer about every 75 s - not robust |
| 6 / 120 / 160 | 15.6 ms | clean on an idle machine; plug-in ran dry (clicks) when the Mac was busy |
| **9 / 200 / 240 (default)** | **21.6 ms** | clean during Spotify use with scrolling and app switching |
| 10 / 240 / 480 | 25.1 ms | clean |

The latency the plug-in reports to apps (`US1800_OUT_LATENCY`, `US1800_IN_LATENCY` in `src/us1800_shm.h`) is a constant.
If you change the preset, update it and reinstall the plug-in so apps' latency compensation matches. Environment variables
for debugging: `US1800_DUMP` (write the playback wire data to a file), `US1800_NO_CAPTURE`.

## Cold-boot test

1. Plug the US-1800 directly into the Mac and power it on.
2. Restart the Mac and log in. Wait about 10 seconds.
3. `launchctl list | grep us1800` shows `local.us1800d` with a PID and exit status `0`.
4. `build/us1800_client stat` prints `running=1 ... usb_errors=0`.
5. System Settings > Sound lists **US1800**. Select it for input and output and play something. Both monitor channels
   should work; outputs 1 and 2 are the left and right monitors.
6. Power-cycle the unit while logged in: the daemon should recover (it restarts itself within a few seconds).
7. Check the log: `tail ~/Library/Logs/us1800d.log` should show a status line every 10 s with `usb_errors=0`.

## Testing

`tools/loopback_test.c` needs a patch cable from output 1 to an input (we used input 9 or 14, line mode, +4 for the rear jacks):

```sh
build/loopback_test rtl 64 20      # frame-counted round trip with 20 clicks at a 64-frame buffer
build/loopback_test tone 600       # 1 kHz tone for 10 minutes; flags dropouts and pitch anomalies
```

Plug-in events go to os_log subsystem `local.us1800`:
`/usr/bin/log show --last 10m --predicate 'subsystem == "local.us1800"' --style compact`
(use the full path: `log` is a shell builtin in zsh).

## Gotchas

- **Capture needs the playback stream running.** With no isoc OUT stream the unit sends nothing on the bulk IN pipe.
  Recovery from a read timeout (`0xe00002ed`) needs `ClearPipeStallBothEnds`; do not clear on aborted reads (`0xe00002eb`),
  that causes an abort cascade.
- **A late playback transfer stops capture**, and recovery costs about a second of dropout. The cause is the host USB clock
  stalling for 9-33 ms under load; the 9 ms queue rides out the shorter ones.
- **Do not drive output from USB completion callbacks.** They are delivered 0.3-30 ms late even though the bus is regular.
- **No 8-channel playback.** The endpoint maximum is 156 bytes per microframe; 8 channels needs 168. The unit fails with
  ~10,000 errors per second. It is an 18-in / 4-out device over USB.
- **Linear-interpolation drift correction is audibly bad** (swishy treble). The windowed sinc is required.
- **Sleeping in a capture callback freezes capture.** Never block there.
- The plug-in is ad-hoc signed and coreaudiod runs sandboxed; it can open `/us1800` shared memory by name but little else.
  Installing or updating it needs `killall coreaudiod`, which interrupts all system audio for a moment.
- Run only one daemon. A stray test copy next to the login item will fight it for the device.
- Loud inputs bleed about 100-700 counts into the neighbouring slot; judge input mapping by orders of magnitude.

## What would improve it

- **Find why a late transfer stops capture.** If recovery were clean, the queue could drop back toward 5-6 ms (~15 ms round trip).
- **Find what stalls the realtime timer thread** occasionally (about once per several minutes).
- **Mixer, monitor routing and clock source** (vendor requests; the kext reference has `usbApply*` / `usbInit*`), and meters.
- **MIDI** (probably the bulk OUT endpoint `0x04`).
- **Other sample rates** (44.1 / 88.2 / 96 kHz): the protocol side is known, the rate matching and plug-in are fixed at 48 kHz.
- **Measure converter latency inside the unit** with the loopback and put it in the reported latency.
- **Presets as a setting** (low / safe) without rebuilding, and a proper installer package.
- A **notarised, Developer-ID-signed** plug-in so it does not rely on ad-hoc signing.
- Test on other units and firmware versions.

## Credits and legal

- Protocol knowledge comes from USB captures and probing, from disassembling Tascam's V3.25 kext (x86_64, used for reference
  only and **not** included), and from the community output driver `MarlonFauser/tascam-us1800-driver` (license unstated, not
  included here).
- "TASCAM" and "US-1800" are trademarks of TEAC Corporation. This project is independent and unofficial.
