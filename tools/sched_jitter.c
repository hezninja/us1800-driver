/* sched_jitter.c - how late does a realtime thread wake up? Wakes every 1 ms for N seconds and reports the lateness tail.
 * Usage: sched_jitter <seconds> [rt=1|0]    Build: clang -O2 -o build/sched_jitter tools/sched_jitter.c */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 60;
    int rt = argc > 2 ? atoi(argv[2]) : 1;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ns2abs = (double)tb.denom / tb.numer;
    if (rt) {
        thread_time_constraint_policy_data_t p = { .period = (uint32_t)(1e6 * ns2abs), .computation = (uint32_t)(2e5 * ns2abs),
                                                   .constraint = (uint32_t)(5e5 * ns2abs), .preemptible = 1 };
        if (thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT))
            fprintf(stderr, "realtime policy failed\n");
    }
    uint64_t step = (uint64_t)(1e6 * ns2abs), next = mach_absolute_time() + step, end = next + (uint64_t)(secs * 1e9 * ns2abs);
    unsigned long n = 0, over1 = 0, over2 = 0, over5 = 0, over10 = 0; double worst = 0;
    while (next < end) {
        mach_wait_until(next);
        double late_ms = (double)(mach_absolute_time() - next) / ns2abs / 1e6;
        n++; if (late_ms > worst) worst = late_ms;
        if (late_ms > 1) over1++; if (late_ms > 2) over2++; if (late_ms > 5) over5++; if (late_ms > 10) over10++;
        next += step;
        if (mach_absolute_time() > next) next = mach_absolute_time() + step;      /* skip missed ticks */
    }
    printf("%lu wakeups, rt=%d: >1ms %lu, >2ms %lu, >5ms %lu, >10ms %lu, worst %.2f ms\n", n, rt, over1, over2, over5, over10, worst);
    return 0;
}
