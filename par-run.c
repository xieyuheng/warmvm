/* par-run —— 双 warmvm 并行验证
 * VM0: main(构建 T1 + 导出 T2) → 宿主搬运 bundle → reduce(T1)
 * VM1: import(T2) + reduce
 * 正确性: count0 + count1 == 128 (2 × 64 叶)
 */
#define _GNU_SOURCE
#include "wvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>

#define ENTRY_MAIN    0x155E
#define ENTRY_REDUCE  0x164A
#define ENTRY_IMPORT  0x164D
#define XBASE         0x6000
#define XREC          0x6108
#define CB_ERR        0x4010
#define CB_COUNT      0x400C

typedef struct {
    wvm_t *m;
    const uint8_t *file;
    size_t flen;
    uint16_t entry;
    int cpu;
    double ms;
    long long l1d_miss, l1i_miss;
} job_t;

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

#define EV_L1D_LOAD_MISS (PERF_TYPE_HW_CACHE | (PERF_COUNT_HW_CACHE_L1D) | \
    (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))
#define EV_L1I_FETCH_MISS (PERF_TYPE_HW_CACHE | (PERF_COUNT_HW_CACHE_L1I) | \
    (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))

static void *worker_thread(void *arg) {
    job_t *j = arg;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(j->cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
    wvm_wr16(j->m, WVM_CB_ENTRY, j->entry);
    int f1 = ev_open(PERF_TYPE_HW_CACHE, EV_L1D_LOAD_MISS);
    int f2 = ev_open(PERF_TYPE_HW_CACHE, EV_L1I_FETCH_MISS);
    ioctl(f1, PERF_EVENT_IOC_RESET, 0);
    ioctl(f2, PERF_EVENT_IOC_RESET, 0);
    ioctl(f1, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(f2, PERF_EVENT_IOC_ENABLE, 0);
    double t0 = now_ms();
    wvm_run(j->m);
    j->ms = now_ms() - t0;
    ioctl(f1, PERF_EVENT_IOC_DISABLE, 0);
    ioctl(f2, PERF_EVENT_IOC_DISABLE, 0);
    read(f1, &j->l1d_miss, 8);
    read(f2, &j->l1i_miss, 8);
    close(f1); close(f2);
    return NULL;
}

int main(void) {
    static uint8_t file[1 << 20];
    FILE *f = fopen("inet-par.wvm", "rb");
    if (!f) { perror("inet-par.wvm"); return 1; }
    size_t flen = fread(file, 1, sizeof(file), f);
    fclose(f);

    wvm_t *m0 = calloc(1, sizeof(*m0));
    wvm_t *m1 = calloc(1, sizeof(*m1));

    /* 阶段 1: VM0 构建 + 导出 (顺序) */
    wvm_load(m0, file, flen);
    wvm_wr16(m0, WVM_CB_ENTRY, ENTRY_MAIN);
    double t0 = now_ms();
    wvm_run(m0);
    double build_ms = now_ms() - t0;
    uint32_t err = wvm_rd32(m0, CB_ERR);
    uint32_t n = wvm_rd32(m0, XBASE);
    uint32_t pn = wvm_rd32(m0, XBASE + 4);
    printf("VM0 build+export: err=%u n=%u pairs=%u (%.2f ms)\n", err, n, pn, build_ms);
    if (err != 1) { fprintf(stderr, "EXPECT err=1 (EXPORT), got %u\n", err); return 1; }

    /* 阶段 2: 宿主搬运 bundle 到 VM1 */
    wvm_load(m1, file, flen);   /* 先加载程序 */
    size_t blen = XREC - XBASE + (size_t)n * 16;
    memcpy((uint8_t *)m1->mem + XBASE, (uint8_t *)m0->mem + XBASE, blen);
    printf("bundle copied: %zu bytes\n", blen);

    /* 阶段 3: 并行 (两个线程) */
    job_t j0 = { m0, file, flen, ENTRY_REDUCE, 0, 0, 0, 0 };
    job_t j1 = { m1, file, flen, ENTRY_IMPORT, 4, 0, 0, 0 };
    pthread_t th0, th1;
    double tp0 = now_ms();
    pthread_create(&th0, NULL, worker_thread, &j0);
    pthread_create(&th1, NULL, worker_thread, &j1);
    pthread_join(th0, NULL);
    pthread_join(th1, NULL);
    double par_ms = now_ms() - tp0;

    uint32_t c0 = wvm_rd32(m0, CB_COUNT);
    uint32_t c1 = wvm_rd32(m1, CB_COUNT);
    printf("VM0 (reduce T1): count=%u  %.2f ms  L1d miss=%lld L1i miss=%lld\n",
           c0, j0.ms, j0.l1d_miss, j0.l1i_miss);
    printf("VM1 (import T2): count=%u  %.2f ms  L1d miss=%lld L1i miss=%lld\n",
           c1, j1.ms, j1.l1d_miss, j1.l1i_miss);
    printf("parallel phase: %.2f ms (max %.2f)\n", par_ms,
           j0.ms > j1.ms ? j0.ms : j1.ms);
    printf("total: count0+count1 = %u (expect 128) %s\n", c0 + c1,
           c0 + c1 == 128 ? "OK" : "FAIL");
    return c0 + c1 == 128 ? 0 : 1;
}
