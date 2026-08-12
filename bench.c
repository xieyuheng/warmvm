/* bench —— WarmVM 基准：正确性 + 计时 + L1 缓存驻留测量
 *
 * 用法:
 *   bench [N ...]        计时 fib(N)（默认 20 30 35）
 *   bench counters N     用 perf 硬件计数器测 fib(N) 的 L1 缺失
 */
#define _GNU_SOURCE
#include "wvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>

static const struct { int n; int64_t expect; } CASES[] = {
    {10, 55}, {20, 6765}, {30, 832040}, {35, 9227465}, {40, 102334155},
};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void pin_cpu(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

static int run_fib(wvm_t *m, int n) {
    uint16_t sp0 = wvm_rd16(m, WVM_CB_SP0);
    uint16_t rp0 = wvm_rd16(m, WVM_CB_RP0);
    wvm_wr32(m, sp0 - 4, (uint32_t)n);   /* 宿主压入参数 */
    wvm_wr16(m, WVM_CB_SP, sp0 - 4);
    wvm_wr16(m, WVM_CB_RP, rp0);
    wvm_wr16(m, WVM_CB_IP, 0);
    int st = wvm_run(m);
    if (st != 0) {
        fprintf(stderr, "fib(%d): status=%d\n", n, st);
        exit(1);
    }
    return (int32_t)wvm_rd32(m, wvm_rd16(m, WVM_CB_SP));
}

static long long ev_open(uint32_t type, uint64_t config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = type;
    pe.size = sizeof(pe);
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    return syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
}

#define EV_L1D_LOADS    (PERF_TYPE_HW_CACHE | \
        (PERF_COUNT_HW_CACHE_L1D << 0) | \
        (PERF_COUNT_HW_CACHE_OP_READ << 8) | \
        (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16))
#define EV_L1D_LOAD_MISS (PERF_TYPE_HW_CACHE | \
        (PERF_COUNT_HW_CACHE_L1D << 0) | \
        (PERF_COUNT_HW_CACHE_OP_READ << 8) | \
        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))
#define EV_L1D_STORES   (PERF_TYPE_HW_CACHE | \
        (PERF_COUNT_HW_CACHE_L1D << 0) | \
        (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | \
        (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16))
#define EV_L1D_STORE_MISS (PERF_TYPE_HW_CACHE | \
        (PERF_COUNT_HW_CACHE_L1D << 0) | \
        (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | \
        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))
#define EV_L1I_FETCH   (PERF_TYPE_HW_CACHE | \
        (PERF_COUNT_HW_CACHE_L1I << 0) | \
        (PERF_COUNT_HW_CACHE_OP_READ << 8) | \
        (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16))
#define EV_L1I_FETCH_MISS (PERF_TYPE_HW_CACHE | \
        (PERF_COUNT_HW_CACHE_L1I << 0) | \
        (PERF_COUNT_HW_CACHE_OP_READ << 8) | \
        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))

static void counters(wvm_t *m, int n) {
    int fds[6];
    fds[0] = ev_open(PERF_TYPE_HW_CACHE, EV_L1D_LOADS);
    fds[1] = ev_open(PERF_TYPE_HW_CACHE, EV_L1D_LOAD_MISS);
    fds[2] = ev_open(PERF_TYPE_HW_CACHE, EV_L1D_STORES);
    fds[3] = ev_open(PERF_TYPE_HW_CACHE, EV_L1D_STORE_MISS);
    fds[4] = ev_open(PERF_TYPE_HW_CACHE, EV_L1I_FETCH);
    fds[5] = ev_open(PERF_TYPE_HW_CACHE, EV_L1I_FETCH_MISS);
    for (int i = 0; i < 6; i++)
        if (fds[i] < 0) {
            perror("perf_event_open");
            exit(1);
        }

    /* 预热：把解释器代码与 VM 数组全部带进 L1 */
    run_fib(m, 30);

    for (int i = 0; i < 6; i++) {
        ioctl(fds[i], PERF_EVENT_IOC_RESET, 0);
        ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0);
    }
    double t0 = now_ms();
    int64_t r = run_fib(m, n);
    double dt = now_ms() - t0;
    for (int i = 0; i < 6; i++)
        ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);

    long long v[6];
    for (int i = 0; i < 6; i++) {
        read(fds[i], &v[i], 8);
        close(fds[i]);
    }
    printf("fib(%d) = %lld   %.1f ms\n", n, (long long)r, dt);
    printf("L1d loads        %12lld   misses %12lld  (%.4f%%)\n",
           v[0], v[1], v[1] * 100.0 / (v[0] ? v[0] : 1));
    printf("L1d stores       %12lld   misses %12lld  (%.4f%%)\n",
           v[2], v[3], v[3] * 100.0 / (v[2] ? v[2] : 1));
    printf("L1i fetches      %12lld   misses %12lld  (%.4f%%)\n",
           v[4], v[5], v[5] * 100.0 / (v[4] ? v[4] : 1));
#ifdef WVM_STATS
    printf("VM instructions  %12llu   (%.1f M/s)\n",
           (unsigned long long)wvm_steps, wvm_steps / (dt / 1000.0) / 1e6);
#endif
}

int main(int argc, char **argv) {
    wvm_t *m = calloc(1, sizeof(*m));
    FILE *f = fopen("fib.wvm", "rb");
    if (!f) { perror("fib.wvm"); return 1; }
    static uint8_t buf[1 << 20];
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (wvm_load(m, buf, len) != 0) { fprintf(stderr, "load failed\n"); return 1; }

    pin_cpu();

    int verify = 1;
    int argi = 1;
    if (argc >= 2 && strcmp(argv[1], "--noverify") == 0) {
        verify = 0;
        argi = 2;
    }

    /* 正确性 */
    if (verify) {
        for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            int64_t r = run_fib(m, CASES[i].n);
            if (r != CASES[i].expect) {
                fprintf(stderr, "FAIL fib(%d) = %ld, expect %ld\n",
                        CASES[i].n, (long)r, (long)CASES[i].expect);
                return 1;
            }
        }
        printf("correctness: all %zu cases OK\n", sizeof(CASES) / sizeof(CASES[0]));
    }

    if (argc >= argi + 1 && strcmp(argv[argi], "counters") == 0) {
        counters(m, argc >= argi + 2 ? atoi(argv[argi + 1]) : 35);
        return 0;
    }

    int ns[8], k = 0;
    if (argc > argi)
        for (int i = argi; i < argc && k < 8; i++) ns[k++] = atoi(argv[i]);
    else {
        ns[k++] = 20; ns[k++] = 30; ns[k++] = 35;
    }
    for (int i = 0; i < k; i++) {
        int n = ns[i];
        run_fib(m, 20);   /* 预热 */
        double t0 = now_ms();
        int64_t r = run_fib(m, n);
        double dt = now_ms() - t0;
        printf("fib(%d) = %lld   %9.1f ms\n", n, (long long)r, dt);
#ifdef WVM_STATS
        printf("  steps %llu  (%.1f M/s)\n",
               (unsigned long long)wvm_steps, wvm_steps / (dt / 1000.0) / 1e6);
#endif
    }
    return 0;
}
