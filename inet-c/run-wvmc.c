/* run-wvmc.c —— 宿主验证: 跑编译出的 .wvm, 检查 RESULT 处的最终网络
 * 用法: run-wvmc <file.wvm> [期望: 从 RESULT 沿 add1 链数到 zero 的个数]
 * 对 nat.lisp 的 (add (one) (two)): 期望 3
 */
#include "wvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CB_RESULT 0x4010
#define CB_ERR    0x400C

static uint32_t rd32(const wvm_t *m, uint32_t a) { return wvm_rd32(m, a); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.wvm [expect]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static uint8_t file[1 << 20];
    size_t flen = fread(file, 1, sizeof(file), f);
    fclose(f);

    wvm_t m;
    wvm_load(&m, file, flen);
    wvm_run(&m);

    uint32_t err = rd32(&m, CB_ERR);
    uint32_t result = rd32(&m, CB_RESULT);
    uint32_t ip = wvm_rd16(&m, WVM_CB_IP);
    printf("status=%u err=%u ip=0x%04x result=0x%08x\n",
           wvm_rd16(&m, WVM_CB_STATUS), err, ip, result);
    if (wvm_rd16(&m, WVM_CB_STATUS) != 0) {
        printf("VM 未正常结束 (err=%u)\n", err);
        return 1;
    }

    /* 线跟踪: 值 -> wire deref / 端口值 pc 读, 直到 agent 端口值或 int */
    uint32_t v = result;
    for (int i = 0; i < 16; i++) {
        if ((v & 0xC0000000) == 0x40000000) { v = rd32(&m, v & 0x1FFFFFFF); continue; }
        if (v & 0x80000000) break;                 /* int 值 */
        uint32_t base = v >> 4, type = rd32(&m, base);
        if (type == 0 || type > 15) break;
        uint32_t nv = rd32(&m, base + ((v & 15) << 2) + 4);
        if (nv == v) break;                        /* 自环 */
        v = nv;
    }
    int count = 0, guard = 0;
    while (guard++ < 100000) {
        if (v & 0x80000000) { printf("int result: %d\n", (int)(v & 0x7FFFFFFF)); break; }
        uint32_t base = v >> 4;
        uint32_t type = rd32(&m, base);
        if (type == 1) break;                      /* zero */
        if (type == 2) {                           /* add1: prev */
            count++;
            v = rd32(&m, base + 8);
            for (int i = 0; i < 16 && (v & 0xC0000000) == 0x40000000; i++)
                v = rd32(&m, v & 0x1FFFFFFF);
            continue;
        }
        printf("意外节点类型 %u at base 0x%x\n", type, base);
        return 1;
    }
    printf("add1 count = %d\n", count);
    if (argc > 2 && count != atoi(argv[2])) {
        printf("FAIL: expect %d\n", atoi(argv[2]));
        return 1;
    }
    printf("OK\n");
    return 0;
}
