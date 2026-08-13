/* compile_wvm.c —— inet-lisp AST → warmvm 汇编文本
 *
 * 语义映射 (warmvm 版 interaction nets):
 *   agent: 16B 块 [type, p0, p1, p2], 端口值 v = (base<<4)|port, cell = base+4+4*port
 *   port 0 = principal (名字以 ! 结尾), aux = 1,2,3 (arity ≤ 4, 类型 ≤ 16)
 *   规则 (F (G p1..pj) a1..ak): TABLE[(F<<4)|G] = TABLE[(G<<4)|F] = handler
 *   handler 入口 [a, B] (a=F 或 B=F, 由类型检查 swap 规范化)
 *   connectx(v1,v2): 全双向 + 双主端口则 enq(v1>>4)
 *   apply(f, args): 输入连接 (principal 输入双向+enq 检查, aux 输入单边/双向),
 *                   输出端口 push fresh portv
 *   函数 = warmvm 子程序 (参数在栈上, 结果 = 最后 exp 的残留值, 留在栈上)
 *   顶层 exprs 在 main 执行, 最后 exp 的值存 RESULT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "lang/index.h"

/* ---------------- 类型表 ---------------- */
#define MAX_TYPES 16
#define MAX_RULES 128
#define MAX_FUNCS 128
#define MAX_VARS  512

typedef struct {
    const char *name;
    int type_id;               /* 1..15 */
    size_t arity;              /* 1..4 */
    const char *port_names[4];
    bool is_principal[4];
    int enc[4];                /* 声明位置 -> 编码端口 (0=principal, aux 按序) */
    bool is_prim;              /* primitive 节点 (iadd/eq?/assert/...) */
    int prim_op;               /* 运算代码 */
    size_t n_in;               /* 输入端口数 */
    size_t n_out;              /* 输出端口数 */
} node_ctor_t;

typedef struct {
    const char *name;
    list_t *arg_name_list;
    list_t *exp_list;
    char label[64];
    size_t result_count;       /* 最后 exp 的残留数 (编译完成后) */
    bool is_value_define;      /* (define x exp) 形式 */
} function_t;

typedef struct {
    exp_t *pattern;            /* (F (G ...) a1..ak) */
    list_t *body;
    int type_f, type_g;
    char label[64];
} rule_t;

static node_ctor_t types[MAX_TYPES];
static size_t n_types = 0;
static function_t funcs[MAX_FUNCS];
static size_t n_funcs = 0;
static rule_t rules[MAX_RULES];
static size_t n_rules = 0;
static size_t label_seq = 0;
static FILE *out;
static const char *cur_fn_name;   /* 递归检测 */

/* ---------------- 变量/绑定 ---------------- */
typedef struct {
    const char *name;
    uint32_t slot;             /* VARS + idx*4 */
    int type_id;               /* -1 未知 */
    int port;                  /* -1 未知 */
} var_t;

static var_t vars[MAX_VARS];
static size_t n_vars = 0;
static const char **bind_names;  /* 当前作用域绑定 (栈式) */
static int *bind_types, *bind_ports;
static size_t n_binds = 0, cap_binds = 0;

static void bind_push(const char *name, int type, int port) {
    if (n_binds == cap_binds) {
        cap_binds = cap_binds ? cap_binds * 2 : 16;
        bind_names = realloc(bind_names, cap_binds * sizeof(char *));
        bind_types = realloc(bind_types, cap_binds * sizeof(int));
        bind_ports = realloc(bind_ports, cap_binds * sizeof(int));
    }
    bind_names[n_binds] = name;
    bind_types[n_binds] = type;
    bind_ports[n_binds] = port;
    n_binds++;
}
static size_t bind_mark(void) { return n_binds; }
static void bind_restore(size_t m) { n_binds = m; }
static int bind_find(const char *name, int *type, int *port) {
    for (size_t i = n_binds; i-- > 0; )
        if (strcmp(bind_names[i], name) == 0) { *type = bind_types[i]; *port = bind_ports[i]; return 1; }
    return 0;
}

static var_t *var_add(const char *name, int type, int port) {
    if (n_vars >= MAX_VARS || 0x4A5C + (uint32_t)n_vars * 4 >= 0x5000) {
        fprintf(stderr, "[wvmc] 变量槽耗尽 (VARS 区 0x4A5C-0x5000)\n");
        exit(1);
    }
    var_t *v = &vars[n_vars++];
    v->name = name;
    v->slot = 0x4A5C + (uint32_t)(n_vars - 1) * 4;
    v->type_id = type;
    v->port = port;
    return v;
}
static var_t *var_find(const char *name) {
    for (size_t i = n_vars; i-- > 0; )
        if (vars[i].name && strcmp(vars[i].name, name) == 0)
            return &vars[i];
    return NULL;
}
static node_ctor_t *prim_find(const char *name);   /* 前向声明 */

static node_ctor_t *ctor_find(const char *name) {
    for (size_t i = 0; i < n_types; i++)
        if (strcmp(types[i].name, name) == 0)
            return &types[i];
    return prim_find(name);
}
static function_t *func_find(const char *name) {
    for (size_t i = 0; i < n_funcs; i++)
        if (strcmp(funcs[i].name, name) == 0)
            return &funcs[i];
    return NULL;
}

static void emit(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
}
static void new_label(char *buf, const char *pfx) {
    snprintf(buf, 64, ".L%zu%s", label_seq++, pfx);
}

/* ================= 表达式编译 =================
 * 返回残留值个数 (栈上留下的值数) */
static size_t compile_exp(exp_t *exp, size_t *rtype, int *rport);
static size_t compile_apply(exp_t *target, list_t *arg_list, size_t *rtype, int *rport);

const char *var_slot_name(const var_t *v) {
    static char buf[2][16];
    static int bi = 0;
    bi ^= 1;                    /* 轮换: 支持同一 emit 内两个槽 */
    snprintf(buf[bi], 16, "0x%04X", v->slot);
    return buf[bi];
}

/* 内建 primitive: 名字, 输入端口数, 运算代码 */
#define PRIM_IADD 1
#define PRIM_ISUB 2
#define PRIM_IMUL 3
#define PRIM_IDIV 4
#define PRIM_IMOD 5
#define PRIM_EQ   6
#define PRIM_ASSERT 7
#define PRIM_NOT  8
#define PRIM_AND  9
#define PRIM_OR   10

static node_ctor_t *prim_find(const char *name) {
    static const struct { const char *n; int op; int nin; } ps[] = {
        { "iadd", PRIM_IADD, 2 }, { "isub", PRIM_ISUB, 2 },
        { "imul", PRIM_IMUL, 2 }, { "idiv", PRIM_IDIV, 2 },
        { "imod", PRIM_IMOD, 2 }, { "eq?", PRIM_EQ, 2 },
        { "assert", PRIM_ASSERT, 1 }, { "not", PRIM_NOT, 1 },
        { "and", PRIM_AND, 2 }, { "or", PRIM_OR, 2 },
        { NULL, 0, 0 }
    };
    for (size_t i = 0; ps[i].n; i++) {
        if (strcmp(ps[i].n, name) == 0) {
            /* 已登记则复用 */
            for (size_t j = 0; j < n_types; j++)
                if (strcmp(types[j].name, name) == 0) return &types[j];
            node_ctor_t *t = &types[n_types++];
            memset(t, 0, sizeof(*t));
            t->name = ps[i].n;
            t->type_id = (int)n_types;
            t->is_prim = true;
            t->prim_op = ps[i].op;
            t->n_in = ps[i].nin;
            t->n_out = (ps[i].op == PRIM_ASSERT) ? 0 : 1;
            /* 端口: 输入 (principal, 名字带 !) + 输出 (aux) */
            t->arity = t->n_in + t->n_out;
            for (size_t j = 0; j < t->arity; j++) {
                t->enc[j] = (int)j;
                t->is_principal[j] = (j < t->n_in);
                t->port_names[j] = ps[i].n;
            }
            return t;
        }
    }
    return NULL;
}

/* primitive 就绪检查: 所有输入 deref 后是 int, 否则跳 L */
static void emit_prim_ready(const node_ctor_t *F, const var_t *base, const char *L) {
    for (size_t i = 0; i < F->n_in; i++) {
        emit("    lit16 %s\n    load\n    lit16 %u\n    add\n    load\n    call drefv\n"
             "    lit32 0x80000000\n    and\n    jz %s\n",
             var_slot_name(base), 4 + 4 * i, L);
    }
}

/* primitive 计算: 读输入 (deref) -> 计算结果压栈 (int 标记值) */
static void emit_prim_calc(const node_ctor_t *F, const var_t *base) {
    char xn[32], yn[32];
    snprintf(xn, sizeof(xn), ".px%zu", label_seq++);
    snprintf(yn, sizeof(yn), ".py%zu", label_seq++);
    var_t *x = var_add(xn, -1, -1), *y = var_add(yn, -1, -1);
    emit("    lit16 %s\n    load\n    lit16 4\n    add\n    load\n    call drefv\n"
         "    lit16 %s\n    store\n", var_slot_name(base), var_slot_name(x));
    if (F->n_in > 1)
        emit("    lit16 %s\n    load\n    lit16 8\n    add\n    load\n    call drefv\n"
             "    lit16 %s\n    store\n", var_slot_name(base), var_slot_name(y));
    if (F->prim_op == PRIM_ASSERT) {
        emit("    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n    jnz .pas_ok%zu\n",
             var_slot_name(x), label_seq);
        emit("    lit8 6\n    lit16 ERR\n    store\n    halt\n");
        emit(".pas_ok%zu:\n    drop\n", label_seq);
        return;
    }
    switch (F->prim_op) {
    case PRIM_IADD: case PRIM_ISUB: case PRIM_IMUL: {
        const char *op = F->prim_op == PRIM_IADD ? "add" :
                         F->prim_op == PRIM_ISUB ? "sub" : "mul";
        emit("    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    %s\n    lit32 0x80000000\n    or\n",
             var_slot_name(x), var_slot_name(y), op);
        break;
    }
    case PRIM_IDIV: case PRIM_IMOD: {
        const char *op = F->prim_op == PRIM_IDIV ? "div" : "mod";
        emit("    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    %s\n    lit32 0x80000000\n    or\n",
             var_slot_name(x), var_slot_name(y), op);
        break;
    }
    case PRIM_EQ: {
        emit("    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    eq\n    lit32 0x80000000\n    or\n",
             var_slot_name(x), var_slot_name(y));
        break;
    }
    case PRIM_NOT: {
        emit("    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    lit8 1\n    xor\n    lit32 0x80000000\n    or\n",
             var_slot_name(x));
        break;
    }
    case PRIM_AND: case PRIM_OR: {
        const char *op = F->prim_op == PRIM_AND ? "and" : "or";
        emit("    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    lit16 %s\n    load\n    lit32 0x7FFFFFFF\n    and\n"
             "    %s\n    lit32 0x80000000\n    or\n",
             var_slot_name(x), var_slot_name(y), op);
        break;
    }
    default:
        fprintf(stderr, "[wvmc] 未实现 primitive op %d\n", F->prim_op);
        exit(1);
    }
}

/* primitive 输出写: [int 结果] -> cell = int; connectx(int, portv) 对端写 */
static void emit_prim_out(const node_ctor_t *F, const var_t *base) {
    emit("    dup\n");            /* [结果, 结果] */
    emit("    lit16 %s\n    load\n    lit16 %u\n    add\n",
         var_slot_name(base), 4 + 4 * F->n_in);
    emit("    store\n");          /* [结果, addr] -> mem[addr] = 结果 ; [结果] */
    emit("    lit16 %s\n    load\n    lit8 %zu\n    call portv\n",
         var_slot_name(base), F->n_in);
    emit("    swap\n");
    emit("    call wropp\n");     /* wropp(portv, 结果): pc(portv) = 结果 (不触发 pcheck) */
}

static size_t compile_apply(exp_t *target, list_t *arg_list, size_t *rtype, int *rport) {
    const char *tname = target->var.name;
    if (rtype) { *rtype = -1; *rport = -1; }
    if (strcmp(tname, "connect") == 0) {
        /* (connect a b): 双向 + 双主 enq */
        exp_t *a = list_first(arg_list);
        exp_t *b = list_next(arg_list);
        assert(a && b && !list_next(arg_list));
        compile_exp(a, NULL, NULL);
        compile_exp(b, NULL, NULL);
        emit("    call connectx\n");
        return 0;
    }
    node_ctor_t *F = ctor_find(tname);
    if (F) {
        size_t k = list_length(arg_list);
        if (k > F->arity) {
            fprintf(stderr, "[wvmc] 节点 %s 参数过多: %zu > %zu\n", tname, k, F->arity);
            exit(1);
        }
        if (F->is_prim) {
            /* primitive: alloc + 输入连接 + 就绪检查 + 计算 */
            var_t tmp[4];
            exp_t *arg = list_first(arg_list);
            for (size_t i = 0; i < k; i++) {
                char tname2[32];
                snprintf(tname2, sizeof(tname2), ".ptmp%zu", label_seq++);
                tmp[i] = *var_add(tname2, -1, -1);
                size_t t; int p;
                size_t n = compile_exp(arg, &t, &p);
                if (n != 1) { fprintf(stderr, "[wvmc] 参数必须产生恰好 1 个值\n"); exit(1); }
                emit("    lit16 %s\n    store\n", var_slot_name(&tmp[i]));
                arg = list_next(arg_list);
            }
            char bname[32];
            snprintf(bname, sizeof(bname), ".pbase%zu", label_seq++);
            var_t *base = var_add(bname, -1, -1);
            emit("    lit8 %zu\n    call newag\n", F->type_id);
            emit("    lit16 %s\n    store\n", var_slot_name(base));
            for (size_t i = 0; i < k; i++) {
                emit("    lit16 %s\n    load\n", var_slot_name(&tmp[i]));
                emit("    lit16 %s\n    load\n    lit8 %d\n    call portv\n",
                     var_slot_name(base), (int)i);
                emit("    call connectx\n");
            }
            /* 就绪检查 + 计算 + 输出 (共享生成) */
            char *L = malloc(64);
            new_label(L, "nof");
            if (F->n_out > 0) {
                char rn[32];
                snprintf(rn, sizeof(rn), ".pr%zu", label_seq++);
                var_t *r = var_add(rn, -1, -1);
                emit_prim_ready(F, base, L);
                emit_prim_calc(F, base);
                emit("    lit16 %s\n    store\n", var_slot_name(r));
                emit("    lit16 %s\n    load\n", var_slot_name(r));
                emit_prim_out(F, base);
                emit("    lit16 %s\n    load\n", var_slot_name(r));
                emit("%s:\n", L);
                free(L);
                if (rtype) *rtype = -1;
                if (rport) *rport = -1;
                return 1;
            } else {
                emit_prim_ready(F, base, L);
                emit_prim_calc(F, base);     /* assert: 失败 halt */
                emit("%s:\n", L);
                free(L);
                return 0;
            }
        }
        /* 参数存临时槽 */
        var_t tmp[4];
        exp_t *arg = list_first(arg_list);
        for (size_t i = 0; i < k; i++) {
            char tname2[32];
            snprintf(tname2, sizeof(tname2), ".tmp%zu", label_seq++);
            tmp[i] = *var_add(tname2, -1, -1);
            size_t t; int p;
            size_t n = compile_exp(arg, &t, &p);
            if (n != 1) { fprintf(stderr, "[wvmc] 参数必须产生恰好 1 个值\n"); exit(1); }
            emit("    lit16 %s\n    store\n", var_slot_name(&tmp[i]));
            tmp[i].type_id = (int)t; tmp[i].port = p;
            arg = list_next(arg_list);
        }
        /* alloc + 存 base 槽 */
        char bname[32];
        snprintf(bname, sizeof(bname), ".base%zu", label_seq++);
        var_t *base = var_add(bname, -1, -1);
        emit("    lit8 %zu\n    call newag\n", F->type_id);
        emit("    lit16 %s\n    store\n", var_slot_name(base));
        /* 输入连接:
         *   principal 输入: connectx (首写值 cell + 互指 + 双主 enq)
         *   aux 输入: 只写 cell + (arg 是端口值时) 互指 — 不首写值 cell
         *             (principal 线由 principal 消费者首写, 避免污染) */
        for (size_t i = 0; i < k; i++) {
            int pe = F->enc[i];            /* 声明位置 i 的编码端口 */
            if (F->is_principal[i]) {
                emit("    lit16 %s\n    load\n", var_slot_name(&tmp[i]));
                emit("    lit16 %s\n    load\n    lit8 %d\n    call portv\n",
                     var_slot_name(base), pe);
                emit("    call connectx\n");
            } else {
                char *L = malloc(64);
                new_label(L, "ax");
                emit("    lit16 %s\n    load\n", var_slot_name(&tmp[i]));
                emit("    lit16 %s\n    load\n    lit16 %u\n    add\n",
                     var_slot_name(base), 4 + 4 * pe);
                emit("    store\n");              /* cell = arg */
                emit("    lit16 %s\n    load\n", var_slot_name(&tmp[i]));
                emit("    dup\n    lit32 0xC0000000\n    and\n    jnz %s\n", L);
                emit("    dup\n    call pc_addr\n");
                emit("    lit16 %s\n    load\n    lit8 %d\n    call portv\n",
                     var_slot_name(base), pe);
                emit("    swap\n    store\n");   /* pc(arg) = portv(f, pe) */
                emit("%s:\n", L);
                free(L);
            }
        }
        /* 输出端口 k..arity-1: fresh wire 引用 (经临时槽), 输出端口 cell = W,
         * wire cell = 持有者端口值; 返回值 W 留在栈上 */
        size_t nout = F->arity - k;
        for (size_t i = k; i < F->arity; i++) {
            char oname[32];
            snprintf(oname, sizeof(oname), ".out%zu", label_seq++);
            var_t *wo = var_add(oname, -1, -1);
            emit("    lit8 %d\n    call new_wire\n", F->enc[i] == 0 ? 1 : 0);
            emit("    lit16 %s\n    store\n", var_slot_name(wo));
            emit("    lit16 %s\n    load\n", var_slot_name(wo));
            emit("    lit16 %s\n    load\n    lit16 %u\n    add\n",
                 var_slot_name(base), 4 + 4 * F->enc[i]);
            emit("    store\n");                  /* mem[base+off] = W ([W, addr]) */
            /* 持有者 cell = portv(base, enc) (驱动自环时推导对端) */
            emit("    lit16 %s\n    load\n    lit32 0x1FFFFFFF\n    and\n    lit16 4\n    add\n",
                 var_slot_name(wo));
            emit("    lit16 %s\n    load\n    lit8 %d\n    call portv\n",
                 var_slot_name(base), F->enc[i]);
            emit("    swap\n    store\n");
            emit("    lit16 %s\n    load\n", var_slot_name(wo));
        }
        if (rtype) *rtype = -1;
        if (rport) *rport = -1;
        return nout;
    }
    function_t *fn = func_find(tname);
    if (fn) {
        if (fn->result_count == (size_t)-1) {
            fprintf(stderr, "[wvmc] 递归函数暂不支持: %s\n", tname);
            exit(1);
        }
        exp_t *arg = list_first(arg_list);
        while (arg) {
            compile_exp(arg, NULL, NULL);
            arg = list_next(arg_list);
        }
        emit("    call %s\n", fn->label);
        if (rtype) *rtype = -1;
        if (rport) *rport = -1;
        return fn->result_count;
    }
    fprintf(stderr, "[wvmc] 不能应用: %s (apply-wire/primitive 暂不支持 v1)\n", tname);
    exit(1);
}

static size_t compile_exp(exp_t *exp, size_t *rtype, int *rport) {
    switch (exp->kind) {
    case EXP_VAR: {
        if (strcmp(exp->var.name, "true") == 0) {
            emit("    lit32 0x80000001\n");
            if (rtype) *rtype = -1;
            if (rport) *rport = -1;
            return 1;
        }
        if (strcmp(exp->var.name, "false") == 0) {
            emit("    lit32 0x80000000\n");
            if (rtype) *rtype = -1;
            if (rport) *rport = -1;
            return 1;
        }
        int t, p;
        if (bind_find(exp->var.name, &t, &p)) {
            var_t *v = var_find(exp->var.name);
            assert(v);
            emit("    lit16 %s\n    load\n", var_slot_name(v));
            if (rtype) *rtype = t;
            if (rport) *rport = p;
            return 1;
        }
        node_ctor_t *F = ctor_find(exp->var.name);
        if (F) {
            list_t *empty = exp_make_list();
            size_t n = compile_apply(exp, empty, rtype, rport);
            list_destroy(&empty);
            return n;
        }
        function_t *fn = func_find(exp->var.name);
        if (fn && fn->result_count == 0) {
            emit("    call %s\n", fn->label);
            if (rtype) *rtype = -1;
            if (rport) *rport = -1;
            return 0;
        }
        fprintf(stderr, "[wvmc] 未定义名字: %s\n", exp->var.name);
        exit(1);
    }
    case EXP_AP:
        if (strcmp(exp->ap.target->var.name, "link") == 0) {
            emit("    lit8 0\n    call new_wire\n");
            emit("    lit8 0\n    call new_wire\n");
            if (rtype) *rtype = -1;
            if (rport) *rport = -1;
            return 2;
        }
        return compile_apply(exp->ap.target, exp->ap.arg_list, rtype, rport);
    case EXP_ASSIGN: {
        /* (= x1..xk (f ...)): exp 产生 k 个值, 依次存槽
         * name_list[i] <-> 第 i 个值 (栈底=第 0 个) */
        size_t k = list_length(exp->assign.name_list);
        size_t n = compile_exp(exp->assign.exp, NULL, NULL);
        if (n != k) {
            fprintf(stderr, "[wvmc] (= ...) 值数量 %zu != 名字数量 %zu\n", n, k);
            exit(1);
        }
        /* 输出端口信息: 若 exp 是 ctor apply, name i <-> port k_in+i */
        int *ports = calloc(k, sizeof(int));
        int *tys = calloc(k, sizeof(int));
        if (exp->assign.exp->kind == EXP_AP) {
            node_ctor_t *F = ctor_find(exp->assign.exp->ap.target->var.name);
            if (F) {
                size_t kin = list_length(exp->assign.exp->ap.arg_list);
                for (size_t i = 0; i < k; i++) {
                    ports[i] = F->enc[kin + i];
                    tys[i] = (int)F->type_id;
                }
            } else {
                for (size_t i = 0; i < k; i++) { ports[i] = -1; tys[i] = -1; }
            }
        }
        char *name = list_last(exp->assign.name_list);
        size_t i = k;
        while (name) {
            i--;
            var_t *v = var_add(name, tys[i], ports[i]);
            emit("    lit16 %s\n    store\n", var_slot_name(v));
            bind_push(name, tys[i], ports[i]);
            name = list_prev(exp->assign.name_list);
        }
        free(ports); free(tys);
        return 0;
    }
    case EXP_INT: {
        emit("    lit32 0x%08X\n", 0x80000000u | ((uint32_t)exp->i.target & 0x7FFFFFFFu));
        if (rtype) *rtype = -1;
        if (rport) *rport = -1;
        return 1;
    }
    case EXP_FLOAT:
        fprintf(stderr, "[wvmc] 浮点字面量暂不支持 (v1)\n");
        exit(1);
    }
    return 0;
}

/* ================= 函数编译 ================= */
static void compile_function(function_t *fn) {
    size_t mark = bind_mark();
    emit("%s:                  ; [args...]\n", fn->label);
    /* 参数存槽: 栈顶 = 最后一个参数 */
    char *arg = list_last(fn->arg_name_list);
    while (arg) {
        var_t *v = var_add(arg, -1, -1);
        emit("    lit16 %s\n    store\n", var_slot_name(v));
        bind_push(arg, -1, -1);
        arg = list_prev(fn->arg_name_list);
    }
    /* body: 非最后 exp 求值后 drop 残留 */
    size_t n = list_length(fn->exp_list);
    exp_t *e = list_first(fn->exp_list);
    size_t idx = 0;
    while (e) {
        size_t r = compile_exp(e, NULL, NULL);
        if (idx < n - 1) {
            for (size_t i = 0; i < r; i++) emit("    drop\n");
        } else {
            fn->result_count = r;   /* 最后 exp 的残留 = 返回值 */
        }
        e = list_next(fn->exp_list);
        idx++;
    }
    emit("    ret\n");
    bind_restore(mark);
}

/* ================= 规则编译 ================= */
static void compile_rule(rule_t *r) {
    /* 模式: (F (G p1..pj) a1..ak) */
    exp_t *pat = r->pattern;
    const char *fname = pat->ap.target->var.name;
    node_ctor_t *F = ctor_find(fname);
    if (!F) { fprintf(stderr, "[wvmc] 规则中未定义节点: %s\n", fname); exit(1); }
    /* 找节点模式 (G 模式): 位置 i 必须是 F 的 principal 端口 (enc[i]==0) */
    exp_t *g = NULL;
    size_t gi = 0;
    {
        size_t i = 0;
        exp_t *e = list_first(pat->ap.arg_list);
        while (e) {
            if (e->kind == EXP_AP) {
                if (g) { fprintf(stderr, "[wvmc] 规则模式只能有一个节点模式: %s\n", fname); exit(1); }
                if (F->enc[i] != 0) {
                    fprintf(stderr, "[wvmc] 节点模式位置 %zu 不是 %s 的 principal 端口\n", i, fname);
                    exit(1);
                }
                g = e; gi = i;
            }
            e = list_next(pat->ap.arg_list);
            i++;
        }
        if (!g) { fprintf(stderr, "[wvmc] 规则模式缺少节点模式: %s\n", fname); exit(1); }
    }
    node_ctor_t *G = ctor_find(g->ap.target->var.name);
    if (!G) { fprintf(stderr, "[wvmc] 规则中未定义节点: %s\n", g->ap.target->var.name); exit(1); }
    r->type_f = (int)F->type_id;
    r->type_g = (int)G->type_id;

    size_t mark = bind_mark();
    size_t nv_mark = n_vars;
    size_t rseq = label_seq++;
    emit("%s:              ; [a B] key=(%d,%d)\n", r->label, r->type_f, r->type_g);
    emit("    dup\n    load\n    lit8 %d\n    eq\n    jnz .rok%zu\n", r->type_f, rseq);
    emit("    swap\n");
    emit(".rok%zu:\n", rseq);
    emit("    lit16 SCR\n    store\n");
    emit("    lit16 SCR+4\n    store\n");
    /* 绑定 F 的端口变量: 跳过节点模式位置和 principal 端口 */
    {
        exp_t *e = list_first(pat->ap.arg_list);
        for (size_t pi = 0; pi < F->arity && e; pi++, e = list_next(pat->ap.arg_list)) {
            if (pi == gi) continue;              /* 节点模式位置 */
            if (e->kind != EXP_VAR) { fprintf(stderr, "[wvmc] F 端口模式必须为变量\n"); exit(1); }
            int pe = F->enc[pi];
            /* port 未知: 绑定值是线的对端, 端口位运行时才可知 */
            var_t *v = var_add(e->var.name, F->type_id, -1);
            emit("    lit16 SCR\n    load\n    lit16 %u\n    add\n    load\n", 4 + 4 * pe);
            emit("    lit16 %s\n    store\n", var_slot_name(v));
            bind_push(e->var.name, F->type_id, -1);
        }
    }
    /* 绑定 G 的 aux 端口变量 */
    exp_t *e = list_first(g->ap.arg_list);
    for (size_t pi = 0; pi < G->arity && e; pi++) {
        if (G->is_principal[pi]) continue;
        if (e->kind != EXP_VAR) { fprintf(stderr, "[wvmc] G 端口模式必须为变量\n"); exit(1); }
        int pe = G->enc[pi];
        var_t *v = var_add(e->var.name, G->type_id, -1);
        emit("    lit16 SCR+4\n    load\n    lit16 %u\n    add\n    load\n", 4 + 4 * pe);
        emit("    lit16 %s\n    store\n", var_slot_name(v));
        bind_push(e->var.name, G->type_id, -1);
        e = list_next(g->ap.arg_list);
    }
    /* body */
    e = list_first(r->body);
    while (e) {
        size_t rv = compile_exp(e, NULL, NULL);
        for (size_t q = 0; q < rv; q++) emit("    drop\n");
        e = list_next(r->body);
    }
    /* 释放 */
    emit("    lit16 SCR\n    load\n    call freeag\n");
    emit("    lit16 SCR+4\n    load\n    call freeag\n");
    emit("    ret\n");
    bind_restore(mark);
}

/* ================= 主程序 ================= */
static const char *engine_asm =
"; ============ 引擎 (wvmc 生成) ============\n"
".eq FREE   0x4000\n"
".eq QHEAD  0x4004\n"
".eq QTAIL  0x4008\n"
".eq ERR    0x400C\n"
".eq RESULT 0x4010\n"
".eq QUEUE  0x4024\n"
".eq TABLE  0x4824\n"
".eq SCR    0x4A24\n"
".eq CSLOT  0x4A4C\n"
".eq AGENTS 0x5000\n"
".eq VARS   0x4A5C\n"
".eq WIRE   0x6000\n"
".eq WNEXT  0x4018\n"
".sp0 0x1000\n"
"\n"
"; 值编码: 端口值 = (base<<4)|port; int = 0x80000000|v; wire = 0x40000000|cell\n"
"; portv: [base port] -> [v]\n"
"portv:\n"
"    swap\n    lit8 4\n    shl\n    swap\n    or\n    ret\n"
"\n"
"; new_wire: [is_principal] -> [wire 引用]\n"
"; wire = 2 cell: [0]=值 (0=未连), [4]=持有者端口值\n"
"; 编码 = 0x40000000 | (p<<29) | cell_addr (cell_addr = 0x6000 + idx*8)\n"
"new_wire:\n"
"    lit16 WNEXT\n    load\n"
"    dup\n    lit16 0x6FF8\n    le\n    jnz .nwok2\n"
"    drop\n    lit8 4\n    lit16 ERR\n    store\n    halt\n"
".nwok2:\n"
"    lit8 1\n    sub\n"
"    dup\n    lit8 3\n    shl\n    lit16 WIRE\n    add\n"
"    lit16 0\n    swap\n    store\n"
"    lit16 WNEXT\n    load\n    lit8 1\n    add\n    lit16 WNEXT\n    store\n"
"    lit8 3\n    shl\n    lit16 WIRE\n    add\n"
"    lit32 0x40000000\n    or\n"
"    swap\n    lit8 29\n    shl\n    or\n    ret\n"
"\n"
"; deref: [v] -> [确定值] (wire 引用 -> wire cell 内容; 其他原样)\n"
"deref:\n"
"    dup\n    lit32 0xC0000000\n    and\n"
"    lit32 0x40000000\n    eq\n"
"    jz .dr_done\n"
"    lit32 0x1FFFFFFF\n    and\n    load\n"
".dr_done:\n    ret\n"
"\n"
"; drefv: [v] -> [确定值] (深度: wire->cell, 端口值->pc 读, 最多 3 层)\n"
"drefv:\n"
"    lit16 SCR+28\n    lit8 0\n    swap\n    store\n"
".dv_loop:\n"
"    lit16 SCR+28\n    load\n    lit8 3\n    lt\n    jz .dv_done\n"
"    dup\n    lit32 0x40000000\n    and\n    jnz .dv_wire\n"
"    dup\n    lit32 0xC0000000\n    and\n    jnz .dv_done\n"
"    dup\n    dup\n    lit8 4\n    shr\n    lit8 4\n    shl\n"
"    swap\n    lit8 15\n    and\n    lit8 2\n    shl\n    add\n"
"    lit8 4\n    add\n    load\n"
"    swap\n    drop\n"
"    jmp .dv_cnt\n"
".dv_wire:\n"
"    lit32 0x1FFFFFFF\n    and\n    load\n"
".dv_cnt:\n"
"    lit16 SCR+28\n    load\n    lit8 1\n    add\n    lit16 SCR+28\n    store\n"
"    jmp .dv_loop\n"
".dv_done:\n    ret\n"
"\n"
"; wropp: [v, val] -> 写 v 的对端, wire cell 无条件覆盖 (connectx/规则转移)\n"
"wropp:\n"
"    swap\n    lit16 CSLOT\n    store\n"
"    lit16 CSLOT+4\n    store\n"
"    jmp .wp_common\n"
"; wropp1: [v, val] -> 同 wropp, 但 wire cell 仅当为 0 时写 (apply 输入首写)\n"
"wropp1:\n"
"    swap\n    lit16 CSLOT\n    store\n"
"    lit16 CSLOT+4\n    store\n"
"    lit16 CSLOT\n    load\n"
"    lit32 0x40000000\n    and\n    jz .wp_common\n"
"    lit16 CSLOT\n    load\n    lit32 0x1FFFFFFF\n    and\n"
"    load\n    dup\n    jnz .wp_done\n"
"    drop\n"
"    jmp .wp_common\n"
".wp_common:\n"
"    lit16 CSLOT\n    load\n"
"    lit32 0xC0000000\n    and\n"
"    lit32 0x40000000\n    eq\n    jnz .wp_wire\n"
"    lit16 CSLOT\n    load\n"
"    lit32 0x80000000\n    and\n    jnz .wp_done\n"
"    lit16 CSLOT\n    load\n    dup\n    call pc_addr\n"
"    dup\n    load\n"
"    lit16 CSLOT+4\n    load\n"
"    rot\n"
"    store\n"
"    swap\n    drop\n"
"    dup\n    lit32 0xC0000000\n    and\n    jnz .wp_done\n"
"    lit8 4\n    shr\n    call pcheck\n"
"    jmp .wp_done\n"
".wp_wire:\n"
"    lit16 CSLOT\n    load\n    lit32 0x1FFFFFFF\n    and\n"
"    dup\n    load\n"
"    lit16 CSLOT+4\n    load\n"
"    rot\n"
"    store\n"
"    dup\n    lit32 0xC0000000\n    and\n    jnz .wp_done\n"
"    lit8 4\n    shr\n    call pcheck\n"
".wp_done:\n    ret\n"
"\n"
"; pc_addr: [v] -> [cell 地址]\n"
"pc_addr:\n"
"    dup\n    lit8 4\n    shr\n"
"    swap\n    lit8 15\n    and\n    lit8 2\n    shl\n    add\n"
"    lit8 4\n    add\n    ret\n"
"\n"
"; newag: [type] -> [addr]  (bump 分配, 不回收 —— 端口值永久有效)\n"
".eq NEXT   0x4014\n"
"newag:\n"
"    lit16 SCR+32\n    store\n"
"    lit16 NEXT\n    load\n"
"    dup\n    lit16 0x5C00\n    lt\n    jnz .nw_ok\n"
"    drop\n    lit8 2\n    lit16 ERR\n    store\n    halt\n"
".nw_ok:\n"
"    lit16 SCR+36\n    store\n"
"    lit16 SCR+32\n    load\n    lit16 SCR+36\n    load\n    store\n"
"    lit16 NEXT\n    load\n    lit16 16\n    add\n    lit16 NEXT\n    store\n"
"    lit16 SCR+36\n    load\n    ret\n"
"\n"
"; freeag: no-op (bump 分配不回收)\n"
"freeag:\n    ret\n"
"\n"
"; enq: [agent_addr]\n"
"enq:\n"
"    lit16 QTAIL\n    load\n"
"    dup\n    lit16 511\n    and\n    lit8 2\n    shl\n    lit16 QUEUE\n    add\n"
"    rot\n    swap\n    store\n"
"    lit8 1\n    add\n    lit16 QTAIL\n    store\n    ret\n"
"\n"
"; connectx: [v1 v2] -> 连接两线端 (fuze 语义)\n"
";   wire 参与: cell 为 0 -> 首写; 否则 connect(旧值, v2) 重连\n"
";   端口值-端口值: 双向互指 + 双主 enq\n"
";   int 参与: 单边写\n"
"connectx:\n"
"    swap\n    lit16 CSLOT\n    store\n"
"    lit16 CSLOT+4\n    store\n"
".cx_common:\n"
"    lit16 CSLOT\n    load\n    lit32 0x40000000\n    and\n    jnz .cx_w1\n"
"    lit16 CSLOT\n    load\n    lit32 0x80000000\n    and\n    jnz .cx_int1\n"
"    lit16 CSLOT+4\n    load\n    lit32 0x40000000\n    and\n    jnz .cx_w2\n"
"    lit16 CSLOT+4\n    load\n    lit32 0x80000000\n    and\n    jnz .cx_int2\n"
"; 端口值-端口值: 双向 + 双主 enq\n"
"    lit16 CSLOT\n    load\n    dup\n    call pc_addr\n    lit16 CSLOT+8\n    store\n"
"    lit16 CSLOT+4\n    load\n    dup\n    call pc_addr\n    lit16 CSLOT+12\n    store\n"
"    lit16 CSLOT+4\n    load\n    lit16 CSLOT+8\n    load\n    store\n"
"    lit16 CSLOT\n    load\n    lit16 CSLOT+12\n    load\n    store\n"
"    lit16 CSLOT\n    load\n    lit8 15\n    and\n    jnz .cx_done\n"
"    lit16 CSLOT+4\n    load\n    lit8 15\n    and\n    jnz .cx_done\n"
"    lit16 CSLOT\n    load\n    lit8 4\n    shr\n    call enq\n"
"    jmp .cx_done\n"
".cx_w2:\n"
"    lit16 CSLOT+4\n    load\n    lit16 CSLOT\n    load\n    call wropp\n"
"    lit16 CSLOT\n    load\n    lit32 0xC0000000\n    and\n    jnz .cx_done\n"
"    lit16 CSLOT\n    load\n    dup\n    call pc_addr\n"
"    lit16 CSLOT+4\n    load\n    swap\n    store\n"
"    jmp .cx_done\n"
".cx_int2:\n"
"    lit16 CSLOT\n    load\n    dup\n    call pc_addr\n"
"    lit16 CSLOT+4\n    load\n    swap\n    store\n"
"    lit16 CSLOT\n    load\n    lit8 4\n    shr\n    call pcheck\n"
"    jmp .cx_done\n"
".cx_w1:\n"
"    lit16 CSLOT\n    load\n    lit32 0x1FFFFFFF\n    and\n"
"    dup\n    load\n    dup\n    jnz .cx_w1_old\n"
"    drop\n"
"    lit16 CSLOT+4\n    load\n    swap\n    store\n"
"    lit16 CSLOT+4\n    load\n    lit32 0xC0000000\n    and\n    jnz .cx_done\n"
"    lit16 CSLOT+4\n    load\n    dup\n    call pc_addr\n"
"    lit16 CSLOT\n    load\n    swap\n    store\n"
"    lit16 CSLOT\n    load\n    lit32 0x60000000\n    and\n    jz .cx_done\n"
"    lit16 CSLOT+4\n    load\n    lit8 15\n    and\n    jnz .cx_done\n"
"    lit16 CSLOT\n    load\n    lit32 0x1FFFFFFF\n    and\n    lit16 4\n    add\n    load\n"
"    lit8 4\n    shr\n    call enq\n"
"    jmp .cx_done\n"
".cx_w1_old:\n"
"    drop\n"
"    lit16 CSLOT\n    load\n    lit32 0x1FFFFFFF\n    and\n    load\n"
"    lit16 CSLOT+4\n    load\n"
"    swap\n    lit16 CSLOT\n    store\n"
"    lit16 CSLOT+4\n    store\n"
"    drop\n"
"    jmp .cx_common\n"
".cx_int1:\n"
"    lit16 CSLOT+4\n    load\n    dup\n    call pc_addr\n"
"    lit16 CSLOT\n    load\n    swap\n    store\n"
"    lit16 CSLOT+4\n    load\n    lit8 4\n    shr\n    call pcheck\n"
".cx_done:\n    ret\n"
"\n"
"; 驱动循环\n"
"driver:\n"
"    lit16 QHEAD\n    load\n    lit16 QTAIL\n    load\n"
"    dup\n    rot\n    eq\n    jnz .done\n"
"    drop\n"
"    lit16 QHEAD\n    load\n    dup\n    lit16 511\n    and\n    lit8 2\n    shl\n"
"    lit16 QUEUE\n    add\n    load\n    swap\n    lit8 1\n    add\n    lit16 QHEAD\n    store\n"
"    dup\n    load\n    dup\n    jnz .ta_ok\n    drop\n    jmp .drop\n"
".ta_ok:\n    lit8 16\n    lt\n    jz .drop\n"
"    dup\n    lit16 4\n    add\n    load\n    call deref\n    lit8 4\n    shr\n    swap\n"
"    lit16 SCR\n    store\n    lit16 SCR+4\n    store\n"
"    lit16 SCR\n    load\n    lit8 4\n    shl\n"
"    lit16 SCR+4\n    load\n    lit16 4\n    add\n    load\n    call deref\n"
"    dup\n    lit16 SCR+4\n    load\n    lit8 4\n    shl\n    eq\n"
"    jnz .bk_ok\n"
"    lit16 SCR\n    load\n    lit8 4\n    shl\n    eq\n"
"    jnz .bk_ok\n"
"    drop\n    jmp .drop\n"
".bk_ok:\n"
"    drop\n"
"    lit16 SCR\n    load\n    lit16 SCR+4\n    load\n    eq\n    jnz .drop\n"
"    lit16 SCR+4\n    load\n    load\n    dup\n    jnz .tb_ok\n    drop\n    jmp .drop\n"
".tb_ok:\n    lit8 16\n    lt\n    jz .drop\n"
"    lit16 SCR\n    load\n    load\n    lit8 4\n    shl\n"
"    lit16 SCR+4\n    load\n    load\n    or\n"
"    dup\n    lit8 1\n    shl\n    lit16 TABLE\n    add\n    load16\n"
"    dup\n    jnz .has_rule\n    drop\n"
"    lit8 3\n    lit16 ERR\n    store\n    halt\n"
".has_rule:\n"
"    swap\n"
"    dup\n    lit8 2\n    shl\n    lit16 0x7FE0\n    add\n"
"    dup\n    load\n    lit8 1\n    add\n    swap\n    store\n"
"    drop\n"
"    lit16 SCR\n    load\n    lit16 SCR+4\n    load\n    rot\n    callx\n"
"    jmp driver\n"
".drop:\n    jmp driver\n"
".done:\n    halt\n";

/* ---------------- 收集 ---------------- */
static void collect_stmt(stmt_t *stmt) {
    switch (stmt->kind) {
    case STMT_DEFINE_NODE: {
        if (n_types >= MAX_TYPES) {
            fprintf(stderr, "[wvmc] 类型数超限: 已有 %zu 个 (重复 define-node?)\n", n_types);
            for (size_t i = 0; i < n_types; i++) fprintf(stderr, "  %s\n", types[i].name);
            exit(1);
        }
        node_ctor_t *t = &types[n_types++];
        memset(t, 0, sizeof(*t));
        t->name = stmt->define_node.name;
        t->type_id = (int)n_types;
        char *pn = list_first(stmt->define_node.port_name_list);
        while (pn) {
            assert(t->arity < 4);
            bool prin = (pn[strlen(pn) - 1] == '!');
            t->port_names[t->arity] = pn;
            t->is_principal[t->arity] = prin;
            t->arity++;
            pn = list_next(stmt->define_node.port_name_list);
        }
        assert(t->arity >= 1);
        /* 编码端口: principal -> 0, aux 按声明顺序 -> 1,2,3 */
        {
            int aux = 1;
            for (size_t i = 0; i < t->arity; i++) {
                if (t->is_principal[i]) t->enc[i] = 0;
                else t->enc[i] = aux++;
            }
        }
        break;
    }
    case STMT_DEFINE:
    case STMT_DEFINE_FUNCTION: {
        assert(n_funcs < MAX_FUNCS);
        function_t *fn = &funcs[n_funcs++];
        memset(fn, 0, sizeof(*fn));
        fn->name = stmt->define.name;
        fn->result_count = (size_t)-1;
        if (stmt->kind == STMT_DEFINE) {
            /* (define x exp): 值定义 = 0 参函数 */
            fn->is_value_define = true;
            fn->arg_name_list = string_make_list();
            fn->exp_list = exp_make_list();
            list_push(fn->exp_list, stmt->define.exp);
        } else {
            fn->arg_name_list = stmt->define_function.arg_name_list;
            fn->exp_list = stmt->define_function.exp_list;
        }
        snprintf(fn->label, sizeof(fn->label), ".fn%zu", n_funcs - 1);
        break;
    }
    case STMT_DEFINE_RULE: {
        assert(n_rules < MAX_RULES);
        rule_t *r = &rules[n_rules];
        r->pattern = stmt->define_rule.pattern_exp;
        r->body = stmt->define_rule.exp_list;
        snprintf(r->label, sizeof(r->label), ".rule%zu", n_rules);
        n_rules++;
        break;
    }
    case STMT_RUN_EXP:
        /* 顶层 exprs 单独收集 */
        break;
    case STMT_IMPORT:
        fprintf(stderr, "[wvmc] import 暂不支持 (v1): 请合并到单文件\n");
        exit(1);
    case STMT_DEFINE_RULE_STAR:
        fprintf(stderr, "[wvmc] define-rule* 暂不支持 (v1)\n");
        exit(1);
    }
}

int compile_wvm_main(list_t *stmt_list, list_t *top_exps, FILE *asm_out) {
    out = asm_out;
    /* pass 1: 收集 */
    stmt_t *st = list_first(stmt_list);
    while (st) { collect_stmt(st); st = list_next(stmt_list); }

    fputs(engine_asm, out);
    emit("\n; ============ 函数 ============\n");
    for (size_t i = 0; i < n_funcs; i++) {
        cur_fn_name = funcs[i].name;
        compile_function(&funcs[i]);
        cur_fn_name = NULL;
    }
    emit("\n; ============ 规则 ============\n");
    for (size_t i = 0; i < n_rules; i++) compile_rule(&rules[i]);

    /* ============ pcheck: 动态 primitive 触发 ============ */
    emit("\n; ============ pcheck ============\n");
    {
        bool any = false;
        for (size_t i = 0; i < n_types; i++)
            if (types[i].is_prim) any = true;
        if (any) {
            emit("pcheck:                    ; [agent_base]\n");
            size_t pc = 0;
            for (size_t i = 0; i < n_types; i++) {
                if (!types[i].is_prim) continue;
                emit("    dup\n    load\n    lit8 %d\n    eq\n    jnz .pc%zu\n",
                     types[i].type_id, pc);
                pc++;
            }
            emit("    drop\n    ret\n");
            pc = 0;
            for (size_t i = 0; i < n_types; i++) {
                if (!types[i].is_prim) continue;
                emit(".pc%zu:\n", pc);
                {
                    char bn[32];
                    snprintf(bn, sizeof(bn), ".pcb%zu", pc);
                    var_t *base = var_add(bn, -1, -1);
                    emit("    lit16 %s\n    store\n", var_slot_name(base));
                    char *L = malloc(64);
                    new_label(L, "pcd");
                    if (types[i].n_out > 0) {
                        emit_prim_ready(&types[i], base, L);
                        /* out 已 int (已计算) -> 跳过, 防触发循环 */
                        emit("    lit16 %s\n    load\n    lit16 %u\n    add\n    load\n"
                             "    lit32 0x80000000\n    and\n    jnz %s\n",
                             var_slot_name(base), 4 + 4 * types[i].n_in, L);
                        emit_prim_calc(&types[i], base);
                        emit_prim_out(&types[i], base);
                    } else {
                        emit_prim_ready(&types[i], base, L);
                        emit_prim_calc(&types[i], base);
                    }
                    emit("    jmp .pc_done\n");
                    emit("%s:\n", L);
                    free(L);
                }
                pc++;
            }
            emit(".pc_done:\n    ret\n");
        } else {
            emit("pcheck:                    ; [agent_base] 无 primitive\n");
            emit("    drop\n    ret\n");
        }
    }

    /* ============ main ============ */
    emit("\n; ============ main ============\n");
    emit(".entry entry\nentry:\n");
    /* init globals */
    emit("    lit16 0\n    lit16 QHEAD\n    store\n");
    emit("    lit16 0\n    lit16 QTAIL\n    store\n");
    emit("    lit16 0\n    lit16 ERR\n    store\n");
    /* bump 分配器: NEXT = AGENTS; WNEXT = 1 (new_wire 用 WNEXT-1 作 idx) */
    emit("    lit16 AGENTS\n    lit16 NEXT\n    store\n");
    emit("    lit16 1\n    lit16 WNEXT\n    store\n");
    /* TABLE */
    for (size_t i = 0; i < n_rules; i++) {
        int k1 = (rules[i].type_f << 4) | rules[i].type_g;
        int k2 = (rules[i].type_g << 4) | rules[i].type_f;
        emit("    lit16 %s\n    lit16 TABLE+%d\n    store16\n", rules[i].label, k1 * 2);
        emit("    lit16 %s\n    lit16 TABLE+%d\n    store16\n", rules[i].label, k2 * 2);
    }
    /* 顶层 exprs */
    size_t ntop = list_length(top_exps);
    size_t idx = 0;
    exp_t *e = list_first(top_exps);
    while (e) {
        size_t r = compile_exp(e, NULL, NULL);
        if (idx < ntop - 1) {
            for (size_t q = 0; q < r; q++) emit("    drop\n");
        } else {
            if (r != 1) {
                fprintf(stderr, "[wvmc] 顶层最后表达式必须产生恰好 1 个值 (得到 %zu)\n", r);
                exit(1);
            }
            emit("    lit16 RESULT\n    store\n");
        }
        e = list_next(top_exps);
        idx++;
    }
    emit("    jmp driver\n");
    return 0;
}
