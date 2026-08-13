/* wvmc_main.c —— inet-lisp 前端: .lisp → warmvm 汇编 → .wvm
 * 用法: wvmc <in.lisp> <out.wvm>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lang/index.h"

int compile_wvm_main(list_t *stmt_list, list_t *top_exps, FILE *asm_out);

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: wvmc <in.lisp> <out.wvm>\n");
        return 1;
    }
    /* 1. 读文件 */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(sz + 1);
    fread(text, 1, sz, f);
    text[sz] = 0;
    fclose(f);

    /* 2. 解析 */
    list_t *sexp_list = sexp_parse_list(text);
    list_t *stmt_list = parse_stmt_list(sexp_list);

    /* 3. 提取顶层 exprs */
    list_t *top_exps = exp_make_list();
    stmt_t *st = list_first(stmt_list);
    while (st) {
        if (st->kind == STMT_RUN_EXP)
            list_push(top_exps, st->run_exp.exp);
        st = list_next(stmt_list);
    }
    if (list_is_empty(top_exps)) {
        fprintf(stderr, "[wvmc] 没有顶层表达式 (程序为空)\n");
        return 1;
    }

    /* 4. 编译 → .asm */
    char tmp[] = "/tmp/wvmc_XXXXXX";
    int fd = mkstemp(tmp);
    FILE *ao = fdopen(fd, "w");
    compile_wvm_main(stmt_list, top_exps, ao);
    fclose(ao);

    /* 5. 调 warmasm.py 汇编 */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "python3 %s/warmasm.py -o %s %s 2>&1",
             getenv("WARMVM_DIR") ? getenv("WARMVM_DIR") : "..",
             argv[2], tmp);
    int rc = system(cmd);
    if (!getenv("WVMCDUMP")) unlink(tmp);
    else fprintf(stderr, "[wvmc] asm 保留: %s\n", tmp);
    if (rc != 0) {
        fprintf(stderr, "[wvmc] 汇编失败\n");
        return 1;
    }
    printf("[wvmc] %s -> %s OK\n", argv[1], argv[2]);
    return 0;
}
