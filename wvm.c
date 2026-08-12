/* WarmVM 解释器核心
 *
 * 热循环：computed goto，fast-64 跳转表（512B，L1d 常驻）
 * release：无分支检查，所有访问 & 0x7FFF 掩码（1 条 AND）
 * SAFE（-DWVM_SAFE）：栈碰撞 / 地址越界 / 除零检查，halt 带错误码
 */
#include "wvm.h"
#include <string.h>

#ifdef WVM_STATS
uint64_t wvm_steps;
#endif

/* ---- 指令 opcode ---- */
enum {
    OP_HALT = 0x00, OP_NOP, OP_LIT8, OP_LIT16, OP_LIT32,
    OP_DUP, OP_DROP, OP_SWAP, OP_OVER, OP_ROT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_LOAD, OP_STORE, OP_LOAD8, OP_STORE8, OP_LOAD16, OP_STORE16,
    OP_JMP, OP_JZ, OP_JNZ, OP_CALL, OP_RET, OP_JMPX, OP_CALLX
};

int wvm_load(wvm_t *m, const void *file, size_t len) {
    const uint8_t *f = (const uint8_t *)file;
    if (len < 16) return -1;
    if (f[0] != (WVM_MAGIC & 0xFF) || f[1] != (WVM_MAGIC >> 8)) return -1;
    uint16_t entry = (uint16_t)(f[2] | (f[3] << 8));
    uint16_t rp0   = (uint16_t)(f[4] | (f[5] << 8));
    uint16_t sp0   = (uint16_t)(f[6] | (f[7] << 8));
    uint16_t csz   = (uint16_t)(f[8] | (f[9] << 8));
    uint16_t dsz   = (uint16_t)(f[10] | (f[11] << 8));
    /* 宿主侧安全（非 VM 语义校验）：代码+数据必须放得进 28K 宇宙 */
    if ((uint32_t)sp0 + csz + dsz > WVM_UNIVERSE) return -1;
    if (16u + csz + dsz > len) return -1;
    memset(m, 0, sizeof(*m));
    wvm_wr16(m, WVM_CB_MAGIC,  WVM_MAGIC);
    wvm_wr16(m, WVM_CB_ENTRY,  entry);
    wvm_wr16(m, WVM_CB_STATUS, WVM_OK);
    wvm_wr16(m, WVM_CB_IP,     0);
    wvm_wr16(m, WVM_CB_RP,     rp0);
    wvm_wr16(m, WVM_CB_SP,     sp0);
    wvm_wr16(m, WVM_CB_RP0,    rp0);
    wvm_wr16(m, WVM_CB_SP0,    sp0);
    memcpy((uint8_t *)m->mem + sp0, f + 16, csz);
    memcpy((uint8_t *)m->mem + sp0 + csz, f + 16 + csz, dsz);
    return 0;
}

int wvm_run(wvm_t *m) {
    uint32_t ip = wvm_rd16(m, WVM_CB_ENTRY);   /* 入口 */
    uint32_t sp = wvm_rd16(m, WVM_CB_SP);      /* 参数栈指针（指向栈顶条目） */
    uint32_t rp = wvm_rd16(m, WVM_CB_RP);      /* 返回栈指针（指向下一个空闲槽） */
    uint16_t status = 0;
#ifdef WVM_SAFE
    uint32_t sp0 = wvm_rd16(m, WVM_CB_SP0);
    uint32_t rp0 = wvm_rd16(m, WVM_CB_RP0);
#endif
#ifdef WVM_STATS
    uint64_t steps = 0;
#endif

    /* push/pop：sp 向低地址增长（先减后写），掩码保证宿主安全 */
#ifdef WVM_SAFE
#define PUSH(v) do { if (sp - 4 < rp) { status = WVM_ERR_STACK; goto STOP; } \
                     sp -= 4; wvm_wr32(m, sp, (uint32_t)(v)); } while (0)
#define POP()   ({ uint32_t _v; if (sp + 4 > sp0) { status = WVM_ERR_STACK; goto STOP; } \
                   sp += 4; _v = wvm_rd32(m, sp - 4); _v; })
#define PUSH_RET(addr) do { if (rp + 2 > sp) { status = WVM_ERR_STACK; goto STOP; } \
                            wvm_wr16(m, rp, (uint16_t)(addr)); rp += 2; } while (0)
#define POP_RET()      ({ if (rp - 2 < rp0) { status = WVM_ERR_STACK; goto STOP; } \
                          rp -= 2; wvm_rd16(m, rp); })
#define CHECK_ADDR(a)  do { if ((uint32_t)(a) >= WVM_UNIVERSE) { status = WVM_ERR_ADDR; goto STOP; } } while (0)
#else
#define PUSH(v) do { sp -= 4; wvm_wr32(m, sp, (uint32_t)(v)); } while (0)
#define POP()   ({ uint32_t _v = wvm_rd32(m, sp); sp += 4; _v; })
#define PUSH_RET(addr) do { wvm_wr16(m, rp, (uint16_t)(addr)); rp += 2; } while (0)
#define POP_RET()      ({ rp -= 2; wvm_rd16(m, rp); })
#define CHECK_ADDR(a)  ((void)0)
#endif
#define FETCH()  do { _op = wvm_rd8(m, ip); ip++; \
                      if (_op >= 64) goto H_ILLEGAL; \
                      goto *tbl[_op]; } while (0)
#define BINOP(op_) do { uint32_t _r = POP(); uint32_t _l = POP(); PUSH(_l op_ _r); } while (0)
#define CMPOP(op_) do { uint32_t _r = POP(); uint32_t _l = POP(); \
                        PUSH((int32_t)_l op_ (int32_t)_r ? 1 : 0); } while (0)

    static const void *tbl[64] = {
        &&H_HALT, &&H_NOP, &&H_LIT8, &&H_LIT16, &&H_LIT32,
        &&H_DUP,  &&H_DROP, &&H_SWAP, &&H_OVER,  &&H_ROT,
        &&H_ADD,  &&H_SUB,  &&H_MUL,  &&H_DIV,   &&H_MOD,
        &&H_AND,  &&H_OR,   &&H_XOR,  &&H_SHL,   &&H_SHR,
        &&H_EQ,   &&H_NE,   &&H_LT,   &&H_GT,    &&H_LE, &&H_GE,
        &&H_LOAD, &&H_STORE,&&H_LOAD8,&&H_STORE8,&&H_LOAD16, &&H_STORE16,
        &&H_JMP,  &&H_JZ,   &&H_JNZ,  &&H_CALL,  &&H_RET, &&H_JMPX, &&H_CALLX,
        [39 ... 63] = &&H_ILLEGAL
    };

    uint8_t _op;
    wvm_wr16(m, WVM_CB_STATUS, 0);
    goto FETCH_D;

H_HALT:   status = WVM_OK; goto STOP;
H_NOP:    goto FETCH_D;
H_LIT8: { int32_t v = (int8_t)wvm_rd8(m, ip); ip += 1; PUSH(v); goto FETCH_D; }
H_LIT16:{ int32_t v = (int16_t)wvm_rd16(m, ip); ip += 2; PUSH(v); goto FETCH_D; }
H_LIT32:{ uint32_t v = wvm_rd32(m, ip); ip += 4; PUSH(v); goto FETCH_D; }
H_DUP:   { uint32_t v = wvm_rd32(m, sp); PUSH(v); goto FETCH_D; }
H_DROP:  { POP(); goto FETCH_D; }
H_SWAP:  { uint32_t a = POP(); uint32_t b = POP(); PUSH(a); PUSH(b); goto FETCH_D; }
H_OVER:  { uint32_t a = POP(); uint32_t b = POP(); PUSH(b); PUSH(a); PUSH(b); goto FETCH_D; }
H_ROT:   { uint32_t a = POP(); uint32_t b = POP(); uint32_t c = POP();
           PUSH(b); PUSH(c); PUSH(a); goto FETCH_D; }
H_ADD:   BINOP(+);  goto FETCH_D;
H_SUB:   BINOP(-);  goto FETCH_D;
H_MUL:   BINOP(*);  goto FETCH_D;
H_DIV:   { uint32_t _r = POP(); uint32_t _l = POP();
           if (_r == 0) { status = WVM_ERR_DIVZERO; goto STOP; }
           PUSH((int32_t)_l / (int32_t)_r); goto FETCH_D; }
H_MOD:   { uint32_t _r = POP(); uint32_t _l = POP();
           if (_r == 0) { status = WVM_ERR_DIVZERO; goto STOP; }
           PUSH((int32_t)_l % (int32_t)_r); goto FETCH_D; }
H_AND:   BINOP(&);  goto FETCH_D;
H_OR:    BINOP(|);  goto FETCH_D;
H_XOR:   BINOP(^);  goto FETCH_D;
H_SHL:   { uint32_t _r = POP(); uint32_t _l = POP(); PUSH(_l << (_r & 31)); goto FETCH_D; }
H_SHR:   { uint32_t _r = POP(); uint32_t _l = POP(); PUSH(_l >> (_r & 31)); goto FETCH_D; }
H_EQ:    { uint32_t _r = POP(); uint32_t _l = POP(); PUSH(_l == _r ? 1 : 0); goto FETCH_D; }
H_NE:    { uint32_t _r = POP(); uint32_t _l = POP(); PUSH(_l != _r ? 1 : 0); goto FETCH_D; }
H_LT:    CMPOP(<);  goto FETCH_D;
H_GT:    CMPOP(>);  goto FETCH_D;
H_LE:    CMPOP(<=); goto FETCH_D;
H_GE:    CMPOP(>=); goto FETCH_D;
H_LOAD:  { uint32_t a = POP(); CHECK_ADDR(a); PUSH(wvm_rd32(m, a)); goto FETCH_D; }
H_STORE: { uint32_t a = POP(); uint32_t v = POP(); CHECK_ADDR(a); wvm_wr32(m, a, v); goto FETCH_D; }
H_LOAD8: { uint32_t a = POP(); CHECK_ADDR(a); PUSH(wvm_rd8(m, a)); goto FETCH_D; }
H_STORE8:{ uint32_t a = POP(); uint32_t v = POP(); CHECK_ADDR(a); wvm_wr8(m, a, (uint8_t)v); goto FETCH_D; }
H_LOAD16:{ uint32_t a = POP(); CHECK_ADDR(a); PUSH(wvm_rd16(m, a)); goto FETCH_D; }
H_STORE16:{uint32_t a = POP(); uint32_t v = POP(); CHECK_ADDR(a); wvm_wr16(m, a, (uint16_t)v); goto FETCH_D; }
H_JMP:   { ip = wvm_rd16(m, ip); goto FETCH_D; }
H_JZ:    { uint32_t c = POP(); uint32_t t = wvm_rd16(m, ip); ip += 2;
           if (c == 0) ip = t;
           goto FETCH_D; }
H_JNZ:   { uint32_t c = POP(); uint32_t t = wvm_rd16(m, ip); ip += 2;
           if (c != 0) ip = t;
           goto FETCH_D; }
H_CALL:  { uint32_t t = wvm_rd16(m, ip); ip += 2; PUSH_RET(ip); ip = t; goto FETCH_D; }
H_RET:   { ip = POP_RET(); goto FETCH_D; }
H_JMPX:  { ip = POP() & 0xFFFF; CHECK_ADDR(ip); goto FETCH_D; }
H_CALLX: { uint32_t t = POP() & 0xFFFF; CHECK_ADDR(t); PUSH_RET(ip); ip = t; goto FETCH_D; }
H_ILLEGAL: status = WVM_ERR_OPCODE; goto STOP;

FETCH_D:
#ifdef WVM_STATS
    steps++;
#endif
    FETCH();

STOP:
    wvm_wr16(m, WVM_CB_STATUS, status);
    wvm_wr16(m, WVM_CB_IP, (uint16_t)ip);
    wvm_wr16(m, WVM_CB_RP, (uint16_t)rp);
    wvm_wr16(m, WVM_CB_SP, (uint16_t)sp);
#ifdef WVM_STATS
    wvm_steps = steps;
#endif
    return status;
}
