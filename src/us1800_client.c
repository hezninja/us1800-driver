/*
 * us1800_client.c - test client for us1800d.
 *
 *   us1800_client rec  <seconds> <out.raw>            record the 18 capture slots (raw int32, 24-bit << 8)
 *   us1800_client tone <output 1..8> <amp 0..1> <seconds>   play a 440 Hz sine on one output
 *   us1800_client stat                                 print daemon counters
 *
 * Build: clang -O2 -o build/us1800_client src/us1800_client.c -lm
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "us1800_shm.h"

static us1800_shm *shm_open_existing(void)
{
    int fd = shm_open(US1800_SHM_NAME, O_RDWR, 0);
    if (fd < 0) { fprintf(stderr, "no daemon (shm_open failed) - start us1800d first\n"); return NULL; }
    us1800_shm *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED || s->magic != US1800_SHM_MAGIC) { fprintf(stderr, "bad shm\n"); return NULL; }
    return s;
}

static void stat_print(us1800_shm *s)
{
    printf("running=%u rate=%u out_ch=%u cap_frames=%llu play_r=%llu play_w=%llu underruns=%u usb_errors=%u plug_resyncs=%u\n",
           atomic_load(&s->running), s->rate, s->out_ch, (unsigned long long)atomic_load(&s->cap_w),
           (unsigned long long)atomic_load(&s->play_r), (unsigned long long)atomic_load(&s->play_w),
           atomic_load(&s->underruns), atomic_load(&s->usb_errors), atomic_load(&s->plug_resyncs));
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: see header comment\n"); return 1; }
    us1800_shm *s = shm_open_existing();
    if (!s) return 1;

    if (!strcmp(argv[1], "stat")) { stat_print(s); return 0; }

    if (!strcmp(argv[1], "rec") && argc >= 4) {
        double secs = atof(argv[2]);
        FILE *f = fopen(argv[3], "wb");
        if (!f) { perror("fopen"); return 1; }
        uint64_t total = (uint64_t)(secs * s->rate), got = 0;
        uint64_t r = atomic_load(&s->cap_w);
        while (got < total) {
            uint64_t w = atomic_load_explicit(&s->cap_w, memory_order_acquire);
            if (w - r > US1800_RING_FRAMES) { fprintf(stderr, "overrun: client too slow\n"); r = w; }
            while (r < w && got < total) {
                fwrite(s->cap[r & (US1800_RING_FRAMES - 1)], sizeof(int32_t), US1800_IN_CH, f);
                r++; got++;
            }
            usleep(2000);
        }
        fclose(f);
        printf("recorded %llu frames to %s\n", (unsigned long long)got, argv[3]);
        return 0;
    }

    if (!strcmp(argv[1], "tone") && argc >= 5) {
        int out = atoi(argv[2]) - 1;
        double amp = atof(argv[3]), secs = atof(argv[4]);
        if (out < 0 || out >= US1800_MAX_OUT) { fprintf(stderr, "output must be 1..%d\n", US1800_MAX_OUT); return 1; }
        uint64_t w = atomic_load(&s->play_r), total = (uint64_t)(secs * s->rate), done = 0;
        atomic_store(&s->play_w, w);
        double ph = 0;
        while (done < total) {
            uint64_t r = atomic_load_explicit(&s->play_r, memory_order_acquire);
            /* keep ~60 ms queued */
            while (w - r < (uint64_t)(0.06 * s->rate) && done < total) {
                int32_t *dst = s->play[w & (US1800_RING_FRAMES - 1)];
                memset(dst, 0, sizeof(int32_t) * US1800_MAX_OUT);
                dst[out] = (int32_t)(sin(2 * M_PI * 440.0 * ph / s->rate) * amp * 8388607.0) << 8;
                ph += 1; w++; done++;
                atomic_store_explicit(&s->play_w, w, memory_order_release);
            }
            usleep(2000);
        }
        usleep(150000);     /* let the queue drain before returning */
        stat_print(s);
        return 0;
    }
    fprintf(stderr, "usage: see header comment\n");
    return 1;
}
