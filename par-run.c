/* par-run —— 4 warmvm 并行验证
 * 阶段 1 (顺序): VM0 export_entry 循环 3 次 (宿主写 DEPTHG: 6,5,4)
 *   → 导出 T2(64 叶)/T3(32 叶)/T4(16 叶), 每次 halt(EXPORT) 后宿主搬运
 * 阶段 2 (并行): VM0 reduce(T1=64 叶) + VM1/2/3 import(T2/T3/T4) 同时归约
 * 正确性: count0+count1+count2+count3 == 64+64+32+16 = 176
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

#define ENTRY_EXPORT  0x155E
#define ENTRY_REDUCE  0x162A
#define ENTRY_IMPORT  0x16F1
#define XBASE         0x6000
#define XREC          0x6108
#define CB_ERR        0x4010
#define CB_COUNT      0x400C
#define CB_DEPTHG     0x401C
#define NVM 4

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

    wvm_t *m[NVM];
    for (int i = 0; i < NVM; i++) {
        m[i] = calloc(1, sizeof(wvm_t));
        wvm_load(m[i], file, flen);
    }

    /* 阶段 1: VM0 依次导出 T2/T3/T4 (depth 6,5,4) */
    static const uint32_t depth[NVM - 1] = { 6, 5, 4 };
    static const uint32_t leaves[NVM - 1] = { 64, 32, 16 };
    size_t blen = 0;
    for (int i = 0; i < NVM - 1; i++) {
        wvm_wr32(m[0], CB_DEPTHG, depth[i]);
        wvm_wr16(m[0], WVM_CB_ENTRY, ENTRY_EXPORT);
        wvm_run(m[0]);
        uint32_t err = wvm_rd32(m[0], CB_ERR);
        uint32_t n = wvm_rd32(m[0], XBASE);
        uint32_t pn = wvm_rd32(m[0], XBASE + 4);
        if (err != 1) { fprintf(stderr, "export %d: EXPECT err=1, got %u\n", i, err); return 1; }
        blen = XREC - XBASE + (size_t)n * 16;
        memcpy((uint8_t *)m[i + 1]->mem + XBASE, (uint8_t *)m[0]->mem + XBASE, blen);
        printf("VM0 export T%d: depth=%u n=%u pairs=%u (%zu B)\n", i + 2, depth[i], n, pn, blen);
    }

    /* 阶段 2: 4 线程并行归约 */
    wvm_wr32(m[0], CB_DEPTHG, 6);   /* T1 = 64 叶 */
    job_t j[NVM];
    j[0] = (job_t){ m[0], file, flen, ENTRY_REDUCE, 0, 0, 0, 0 };
    j[1] = (job_t){ m[1], file, flen, ENTRY_IMPORT, 4, 0, 0, 0 };
    j[2] = (job_t){ m[2], file, flen, ENTRY_IMPORT, 6, 0, 0, 0 };
    j[3] = (job_t){ m[3], file, flen, ENTRY_IMPORT, 8, 0, 0, 0 };
    pthread_t th[NVM];
    double tp0 = now_ms();
    for (int i = 0; i < NVM; i++)
        pthread_create(&th[i], NULL, worker_thread, &j[i]);
    for (int i = 0; i < NVM; i++)
        pthread_join(th[i], NULL);
    double par_ms = now_ms() - tp0;

    static const char *name[NVM] = { "VM0 reduce T1(64)", "VM1 import T2(64)", "VM2 import T3(32)", "VM3 import T4(16)" };
    uint32_t total = 0, expect = 176, maxc = 0;
    for (int i = 0; i < NVM; i++) {
        uint32_t c = wvm_rd32(m[i], CB_COUNT);
        total += c;
        if (j[i].ms > maxc) maxc = j[i].ms;
        printf("%-17s count=%2u  %6.2f ms  L1d miss=%lld L1i miss=%lld\n",
               name[i], c, j[i].ms, j[i].l1d_miss, j[i].l1i_miss);
    }
    printf("parallel phase: %.2f ms (max %.2f)\n", par_ms, maxc / 1.0);
    printf("total: %u (expect %u) %s\n", total, expect, total == expect ? "OK" : "FAIL");
    return total == expect ? 0 : 1;
}
