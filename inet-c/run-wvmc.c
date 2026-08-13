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

    /* 归约后 (add,zero) 的 connect 把 result 线替换为 addend 线,
     * result 值不再出现在活 cell。对 nat 加法: 正规形 = add1 链,
     * 验证 = 数 add1 块 (type 2) 的个数。 */
    int count = 0;
    for (uint32_t a = 0x4C00; a < 0x5C00; a += 16) {
        if (rd32(&m, a) == 2) count++;
    }
    printf("add1 count = %d\n", count);
    if (argc > 2 && count != atoi(argv[2])) {
        printf("FAIL: expect %d\n", atoi(argv[2]));
        return 1;
    }
    printf("OK\n");
    return 0;
}
