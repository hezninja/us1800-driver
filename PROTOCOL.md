# TASCAM US-1800 USB protocol (reverse-engineered)

Sources: USB descriptor dump (`tools/dump_descriptors.c`), the community output driver
(`upstream-ref/`, MarlonFauser/tascam-us1800-driver, license unstated), disassembly of Tascam's V3.25 kext
(`re/`, x86_64 only), and live probing on Apple Silicon / macOS 15 (`tools/probe_*.c`).
Device: VID 0x0644 PID 0x8030, firmware 1.49 (reads back as bytes [49, 1] -> "1.49"), self-powered,
high-speed. Needs no kernel driver: IOKit userspace access works as a normal user.

## Descriptors (configuration 1)

| Iface | Alt | EP   | Type          | MaxPkt | Role                                           |
|-------|-----|------|---------------|--------|------------------------------------------------|
| 0     | 1   | 0x02 | isoc OUT      | 156    | playback audio (S24_3LE, packed channels)      |
| 0     | 1   | 0x83 | bulk IN       | 512    | **no data observed yet** (rate is set on it)   |
| 0     | 1   | 0x04 | bulk OUT      | 512    | unknown (control / MIDI out?)                  |
| 1     | 1   | 0x81 | isoc IN       | 64     | probably playback-rate feedback                |
| 1     | 1   | 0x86 | bulk IN       | 512    | **capture audio** (see below)                  |

Original kext class is "Ploytec bulk-in / isoc-out" (`isPloytecBulkInIsocOutDevice`, `rtsIsocOutBulkIn`).

## Init sequence (works)

1. SetConfiguration(1); open interfaces 0 and 1; SetAlternateInterface(1) on both.
2. Vendor IN `0xC0 / 0x56` len 2 -> firmware (minor, major).
3. Class endpoint SET_CUR sample rate: `bmRT 0x22, bReq 0x01, wValue 0x0100, wIndex 0x83` and again with wIndex `0x02`,
   3 bytes little-endian rate (e.g. 48000).
4. Register 0x49: read (`0xC0/0x49`, 1 byte), then write (`0x40/0x49`, wValue = new, no data).
   `new = (old & ~0x20) | 0x10` for >=17 input channels (`PGDevice::setAJDMAInputChannels`);
   for <17 channels bit 0x20 is *set* instead. Selects 16- vs 32-word capture frame layout.
5. Start the isoc OUT stream (silence is fine). Playback: 8 microframes/transfer, 6 frames per microframe at 48 kHz,
   `frReqCount = 6 * nchan * 3`. 4 channels works; 8 does not (see below).

Rate index table in kext (`startStreaming`): 44100->0, 48000->1, 88200->2, 96000->3, 176400->4, 192000->5.
(A 256-byte per-rate block sent with vendor req 0x42 is gated by a flag that is never set for this model: ignore.)

## Playback routing (verified by ear, MONITOR MIX = COMPUTER, monitors on MONITOR OUT balanced L/R)

4-channel isoc stream, one output tone at a time at -34 dBFS: output 1 -> left monitor (faint bleed on the right,
probably room), output 2 -> right monitor only, outputs 3 and 4 -> silent on the monitors (other jacks).
So stream channels 1/2 are the monitor L/R. Re-verified through the daemon (shm ring path) with 10 s tones: output 1 left only, output 2 right only. Other output jacks (3, 4) not yet checked by ear.

## Capture format (bulk IN 0x86) - VERIFIED

Stream is continuous 512-byte packets (~11.7k/s at 48 kHz = 4 frames per packet, 128 bytes per frame).
Words are 16-bit little-endian; only bits 0..8 are meaningful (observed values 0..127, bits 7-8 unused on this unit).
Bit-sliced ("puzzle") layout, from `_dmaDecode32To18At32Bit_C`:

```
frame (128 B = 64 words):
  words  0..23 : 24 bit-planes, MSB first. Bit j of word k -> bit (23-k) of channel 2*j      (j = 0..8)
  words 24..31 : zero
  words 32..55 : 24 bit-planes, MSB first. Bit j of word k -> bit (55-k) of channel 2*j+1    (j = 0..8)
  words 56..63 : zero
sample = signed 24-bit (kext then shifts left 8 into int32)
```

Observed live: channels 0..13 carry data, 14..17 read zero (unit idle, nothing connected; mapping of physical inputs
to slots not yet confirmed). Idle floor ~85 LSB rms (24-bit) plus a large common-mode ~1-3 Hz drift.

Other decoders exist in the kext for other channel counts (`dmaDecode16To..At32Bit`, `dmaDecode32To20..32`); US-1800 uses 32To18.

## Open questions / TODO

- Physical input -> slot mapping. Confirmed by plugging a source into one input at a time: input 1 = slot 0,
  input 2 = slot 1, input 9 = slot 8, input 10 = slot 9 (input n = slot n-1, jack inputs included). Inputs 3-8
  are assumed slots 2-7 (not individually tested). Loud sources bleed ~100-700 into the neighbouring slot
  (crosstalk), so judge by orders of magnitude, not by "above floor".
  Inputs 11 and 14 confirmed as slots 10 and 13 (rear jacks); 12 and 13 follow the pattern. Slot 14 carries a copy of output 1 at ~2x level (internal return, silent for outs 2-3); slots 15-17 are silent.
- ~~Does capture need the isoc OUT stream running?~~ YES: with no isoc OUT stream, bulk IN 0x86 sends nothing.
  Start the OUT stream first (silence is fine), then read 0x86. Recovery after a read timeout: ClearPipeStallBothEnds.
- What is bulk IN 0x83 for (stays silent) and bulk OUT 0x04? Likely control port / MIDI (`AJ::sendCPort`, `midiOut0/1`).
- Playback is 4 channels only: the 8-channel stream (--out-channels 8) needs 7 frames x 8 ch x 3 B = 168 B per microframe, over the 156 B endpoint max, and fails with ~10000 usb_errors/s and dead capture. The US-1800 is 18-in/4-out over USB; digital out and phones are fed by the unit's internal mixer.
- Mixer / monitor / gain / phantom / sample-clock controls: vendor requests, see `usbApply*`, `usbInit*` in kext.
- MIDI (`USBMidiDevice`, `USBMidiPattern`).
- Level meters (`AJ::getLevelInfo`).

## CoreAudio plug-in (plugin/) - WORKS
`build/US1800.driver` (ad-hoc signed AudioServerPlugIn, installed by `plugin/install.sh` with sudo) shows up as
"TASCAM US-1800": 10 in / 4 out, 48 kHz Float32. The sandboxed coreaudiod CAN `shm_open` the daemon's `/us1800` (no Mach
service needed). Verified: capture through CoreAudio shows real levels; playback through CoreAudio is clean on the
correct side (output 1 left only). Test tool: `tools/hal_test.c`. Plug-in timing is the host clock (no USB-clock
feedback yet); rings resync on drift.
Open: `usb_errors` ticks up ~1 per 10 s at startup even when idle (cause unknown, audio unaffected).

## Stall findings
Without realtime scheduling the daemon saw 5-73 ms gaps in both USB streams at once (even idle), causing plug-in ring resyncs
(audible 64 ms holes). With `THREAD_TIME_CONSTRAINT_POLICY` on the main and capture threads: max gap 9 ms, 0 resyncs, 0 USB errors.
Current latency is high (~100 ms round trip: 48 ms wire queue + 64 ms ring prefill/cushion); tune down later.

## Capture must use queued (async) bulk reads - IMPORTANT
One synchronous `ReadPipeTO` at a time loses ~1% of the audio (47.5 kHz instead of 48 kHz): the pipe idles between requests
and the unit's small FIFO overflows. Verified with 50 Hz mains hum on an open input: 50.49 Hz (sync) vs 50.01-50.03 Hz (8 async
512-byte `ReadPipeAsync` in flight, 0 errors, 47981 Hz vs the Mac clock). Larger sync reads (2048 B) did not help.
Do not call `ClearPipeStallBothEnds` on `0xe00002eb` (aborted): it is fallout from a previous clear and cascades.

## Isoc IN 0x81 = playback rate feedback (3-byte packets, 1 per ms)
Each packet holds the unit's last three per-ms sample counts, newest in byte 0 (window slides: `2f 30 30`, `30 2f 30`, `30 30 2f`).
Values are 47/48/49 around 48 (0x30). First packets are `06 00 00`, `30 06 00`, `30 30 06` while the window fills.
Packets arrive with status 0xe00002e7 (overrun) from `ReadIsochPipeAsync` with frReqCount 64 but still carry 3 valid bytes.
Mean over 93 s: ~47.995 samples/ms (about -100 ppm vs the Mac's USB frame clock). Currently unused: the OUT stream always sends exactly 48/ms.
Enabled for study with `us1800d --fb-log`.

## Findings from the latency/stability work (2026-10)

**Playback needs rate matching (isoc IN 0x81).** The unit's clock runs slightly slower than the USB frame clock. Sending a fixed 48
frames/ms overfills its playback FIFO; about every 51 s it overflows and plays HF garbage for ~6 s (found with an output-1 ->
input-9 loopback and a 1 kHz tone). 0x81 packets are `[newest, previous, one-before]` frames-consumed counts per ms (47/48/49;
`06 00 00` style bytes at start are junk). The daemon keeps (frames sent - frames reported consumed) constant: send the smoothed
reported rate plus 0.0002 x deviation, 46..50 frames per ms, remainder spread over the 8 microframes. Empty 1 ms slots are
recovered from the older bytes of the next packet. Do not steer from a model of the unit's clock fitted to capture timestamps: it
wanders by hundreds of ppm under load (and capture stalls lose frames), which made playback worse.

**The unit stops capturing when the playback stream has a hole.** A late playback transfer is followed ~13 ms later by a capture
timeout (0xe00002ed) and the bulk IN pipe stays halted until `ClearPipeStallBothEnds`. Do clear on 0xe00002ed (not on 0xe00002eb,
and never sleep in the callback: a usleep(20000) there froze capture for 20 ms).

**Completions are delivered late; the bus is not.** Kernel completion stamps of the isoc OUT stream are regular (max gap 1.08 ms),
but callbacks reach user space 0.3-30 ms later. So (1) the output stream is submitted by a realtime timer thread (`mach_wait_until`
every 1 ms, `GetBusFrameNumber`, keep `--queue-ms` transfers ahead), not from completion callbacks; (2) capture keeps 64 x 1 KB
`ReadPipeAsync` in flight (~114 ms): with 8 x 512 B a late callback overflowed the unit's FIFO and lost audio.
Worst measured timer-thread hiccup ~3-4 ms (wake-up or the submit / bus-frame kernel calls), so a 6 ms queue holds
(1 late transfer in 9 min), 5 ms does not (one per ~75 s), 3 ms fails.

**Plug-in drift correction** is a 32-tap Kaiser-windowed sinc fractional-delay resampler (`plugin/resample.h`, tested offline with
`tools/test_resample.c`: <= -63 dB error to 18 kHz, flat to 0.06 dB at every fractional position). Linear interpolation was audibly
bad (-28 dB at 5 kHz, -7 dB at 18 kHz, swishy treble once the correction started). The ring fill is held at prefill/cushion by
nudging the ratio at most +-0.1% (5 s time constant).

**Round-trip latency** (output 1 -> input 9 loopback, `tools/loopback_test.c rtl 64`, frames counted from IO cycles, includes the
app's 64-frame buffer): defaults (queue 6 ms, prefill 120, cushion 160) = 15.6 ms; queue 5 / 100 / 120 = 12.7 ms (not robust);
queue 8 / 120 / 360 = 20.4 ms; queue 10 / 240 / 480 = 25.1 ms. 512-frame app buffer adds ~9.7 ms. Each 48 frames of cushion or
prefill is 1 ms.

Dead ends worth remembering: a "device timeline" (plug-in sample time = capture frame counter, fitted to the Mac's clock) broke because
capture stalls lose frames so the counter is not a clock; the NSActivityLatencyCritical hint did not change completion latency;
moving the unit off a hub removed the worst (19 ms) gaps but not the typical ones.
