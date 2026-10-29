/*
 * us1800_shm.h - shared-memory ring layout between the US-1800 daemon and its clients (CoreAudio plug-in, tools).
 *
 * All samples are int32, 24-bit left-justified (value << 8), interleaved by frame.
 * Rings are single-producer / single-consumer, indexed by free-running 64-bit frame counters.
 */
#pragma once
#include <stdint.h>
#include <stdatomic.h>

#define US1800_SHM_NAME     "/us1800"
#define US1800_SHM_MAGIC    0x55531800u
#define US1800_SHM_VERSION  1
#define US1800_IN_CH        18          /* decoded capture slots */
#define US1800_MAX_OUT      8           /* playback ring columns */
#define US1800_RING_FRAMES  16384       /* power of two, ~341 ms at 48 kHz */

/* Latency the plug-in reports to CoreAudio, in frames: our own buffering at the daemon defaults (queue 6 ms + prefill 120;
 * cushion 160). Converter and USB-packet delay inside the unit is not included because it has not been measured yet. */
#define US1800_OUT_LATENCY  640
#define US1800_IN_LATENCY   256

typedef struct {
    uint32_t magic, version, rate, out_ch;      /* out_ch = channels actually on the wire */
    _Atomic uint64_t cap_w;                     /* frames written by the daemon (capture) */
    _Atomic uint64_t play_r;                    /* frames consumed by the daemon (playback) */
    _Atomic uint64_t play_w;                    /* frames written by the client (playback) */
    _Atomic uint32_t running;                   /* 1 while the USB streams are up */
    _Atomic uint32_t underruns;                 /* playback frames the daemon had to fill with silence */
    _Atomic uint32_t usb_errors;
    uint32_t play_prefill, cap_cushion;         /* plug-in slack in frames, set by the daemon (latency vs stall tolerance) */
    _Atomic uint32_t plug_resyncs;              /* times the CoreAudio plug-in had to resync a ring (audible glitch) */
    int32_t cap[US1800_RING_FRAMES][US1800_IN_CH];
    int32_t play[US1800_RING_FRAMES][US1800_MAX_OUT];
} us1800_shm;
