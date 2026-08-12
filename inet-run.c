/* inet-run —— interaction nets 验证宿主
 * 用法: inet-run [runs]        运行 K+K 归约 N 次，验证 counter == 2K
 *       inet-run counters runs  perf 计数 L1 缺失（加载+运行整循环）
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

#define CB_COUNT 0x400C   /* 结果计数 */
#define CB_ERR   0x4010   /* 错误码: 0 ok, 2 OOM, 3 无规则 */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static long long ev_open(uint32_t type, uint64_t config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = type; pe.size = sizeof(pe); pe.config = config;
    pe.disabled = 1; pe.exclude_kernel = 1; pe.exclude_hv = 1;
    return syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
}

#define EV_L1D_LOADS (PERF_TYPE_HW_CACHE | (PERF_COUNT_HW_CACHE_L1D) | \
    (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16))
#define EV_L1D_LOAD_MISS (PERF_TYPE_HW_CACHE | (PERF_COUNT_HW_CACHE_L1D) | \
    (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))
#define EV_L1I_FETCH (PERF_TYPE_HW_CACHE | (PERF_COUNT_HW_CACHE_L1I) | \
    (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16))
#define EV_L1I_FETCH_MISS (PERF_TYPE_HW_CACHE | (PERF_COUNT_HW_CACHE_L1I) | \
    (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))

int main(int argc, char **argv) {
    int use_counters = argc >= 2 && strcmp(argv[1], "counters") == 0;
    int runs = use_counters ? (argc >= 3 ? atoi(argv[2]) : 50)
                            : (argc >= 2 ? atoi(argv[1]) : 10);

    static uint8_t file[1 << 20];
    FILE *f = fopen("inet.wvm", "rb");
    if (!f) { perror("inet.wvm"); return 1; }
    size_t flen = fread(file, 1, sizeof(file), f);
    fclose(f);

    cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);

    wvm_t *m = calloc(1, sizeof(*m));
    if (wvm_load(m, file, flen) != 0) { fprintf(stderr, "load failed\n"); return 1; }

    /* 正确性: 跑一次, 检查 counter == 2K (K=100 -> 200) */
    wvm_run(m);
    uint32_t err = wvm_rd32(m, CB_ERR);
    uint32_t count = wvm_rd32(m, CB_COUNT);
    printf("first run: err=%u counter=%u (expect 500)\n", err, count);
    if (err != 0 || count != 500) { fprintf(stderr, "VERIFY FAIL\n"); return 1; }

    int fds[4];
    if (use_counters) {
        fds[0] = ev_open(PERF_TYPE_HW_CACHE, EV_L1D_LOADS);
        fds[1] = ev_open(PERF_TYPE_HW_CACHE, EV_L1D_LOAD_MISS);
        fds[2] = ev_open(PERF_TYPE_HW_CACHE, EV_L1I_FETCH);
        fds[3] = ev_open(PERF_TYPE_HW_CACHE, EV_L1I_FETCH_MISS);
        for (int i = 0; i < 4; i++)
            if (fds[i] < 0) { perror("perf_event_open"); return 1; }
        /* 预热 */
        for (int i = 0; i < 3; i++) { wvm_load(m, file, flen); wvm_run(m); }
        for (int i = 0; i < 4; i++) {
            ioctl(fds[i], PERF_EVENT_IOC_RESET, 0);
            ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0);
        }
    }

    double t0 = now_ms();
    for (int i = 0; i < runs; i++) {
        wvm_load(m, file, flen);   /* 重置 VM（含 32K 归零） */
        wvm_run(m);
    }
    double dt = now_ms() - t0;

    if (use_counters) {
        long long v[4];
        for (int i = 0; i < 4; i++) {
            ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);
            read(fds[i], &v[i], 8);
            close(fds[i]);
        }
        printf("%d runs: %.1f ms (%.2f ms/run)\n", runs, dt, dt / runs);
#ifdef WVM_STATS
        printf("steps/run %12llu  (%.1f M/s)\n",
               (unsigned long long)wvm_steps,
               (double)wvm_steps * runs / (dt / 1000.0) / 1e6);
#endif
        printf("L1d loads %12lld  misses %8lld (%.4f%%)\n",
               v[0], v[1], v[1] * 100.0 / (v[0] ? v[0] : 1));
        printf("L1i fetch %12lld  misses %8lld (%.4f%%)\n",
               v[2], v[3], v[3] * 100.0 / (v[2] ? v[2] : 1));
    } else {
        printf("%d runs: %.1f ms (%.2f ms/run)\n", runs, dt, dt / runs);
    }
    return 0;
}
