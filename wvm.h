/* WarmVM —— 完全驻留 L1 缓存的虚拟机
 *
 * 设计要点：
 *   - 28K 宇宙（0x0000–0x6FFF）+ 4K 影子区（0x7000–0x7FFF），宿主数组共 32K
 *   - 控制块 = 0x0000 起的固定 ABI 区
 *   - 参数栈向低地址增长（push: sp -= 4），锚在 sp0（高地址侧）
 *   - 返回栈向高地址增长（push: rp += 2），锚在 rp0 = 0x0080（低地址侧）
 *   - 所有地址访问 & 0x7FFF 掩码：宿主永不越界（release 无分支检查）
 */
#ifndef WVM_H
#define WVM_H

#include <stdint.h>
#include <stddef.h>

/* ---- 控制块（VM 内存偏移，字节） ---- */
#define WVM_CB_MAGIC   0x00
#define WVM_CB_ENTRY   0x02
#define WVM_CB_STATUS  0x04
#define WVM_CB_IP      0x06
#define WVM_CB_RP      0x08
#define WVM_CB_SP      0x0A
#define WVM_CB_RP0     0x0C
#define WVM_CB_SP0     0x0E

#define WVM_MAGIC      0x4D57   /* 'W' 'M' */

/* 停机状态 */
#define WVM_OK          0
#define WVM_ERR_DIVZERO 2
#define WVM_ERR_STACK   3   /* 仅 SAFE 模式 */
#define WVM_ERR_ADDR    4   /* 仅 SAFE 模式 */
#define WVM_ERR_OPCODE  5

/* 内存布局 */
#define WVM_MEM_WORDS   16384  /* 宿主数组 = 32K 字节 */
#define WVM_UNIVERSE    0x7000 /* 28K 宇宙终点（影子区起点） */
#define WVM_RP0_DEFAULT 0x0080

typedef struct wvm {
    uint16_t mem[WVM_MEM_WORDS];
} wvm_t;

/* ---- 访存助手（掩码 & 0x7FFF，宿主永不越界；x86 未对齐访问无惩罚） ---- */
static inline uint8_t  wvm_rd8 (const wvm_t *m, uint32_t a) {
    return ((const uint8_t *)m->mem)[a & 0x7FFF];
}
static inline uint16_t wvm_rd16(const wvm_t *m, uint32_t a) {
    uint16_t v; __builtin_memcpy(&v, (const uint8_t *)m->mem + (a & 0x7FFF), 2); return v;
}
static inline uint32_t wvm_rd32(const wvm_t *m, uint32_t a) {
    uint32_t v; __builtin_memcpy(&v, (const uint8_t *)m->mem + (a & 0x7FFF), 4); return v;
}
static inline void wvm_wr8 (wvm_t *m, uint32_t a, uint8_t  v) {
    ((uint8_t *)m->mem)[a & 0x7FFF] = v;
}
static inline void wvm_wr16(wvm_t *m, uint32_t a, uint16_t v) {
    __builtin_memcpy((uint8_t *)m->mem + (a & 0x7FFF), &v, 2);
}
static inline void wvm_wr32(wvm_t *m, uint32_t a, uint32_t v) {
    __builtin_memcpy((uint8_t *)m->mem + (a & 0x7FFF), &v, 4);
}

/* ---- .wvm 文件格式（16 字节头） ----
 * 0  magic u16 | 2 entry u16 | 4 rp0 u16 | 6 sp0 u16
 * 8 code_size u16 | 10 data_size u16 | 12..15 保留
 * 之后：代码段（载入 sp0 起），数据段（载入 sp0+code_size 起）
 */
int wvm_load(wvm_t *m, const void *file, size_t len);
int wvm_run(wvm_t *m);   /* 返回停机状态 */

#ifdef WVM_STATS
extern uint64_t wvm_steps;  /* 本次 run 执行的 VM 指令数 */
#endif

#endif
