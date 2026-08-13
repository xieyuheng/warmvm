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

    /* 3. 展开 import: 递归加载被导入文件, 合并所有 stmt (全量, 按路径去重) */
    {
        list_t *merged = exp_make_list();
        list_t *loaded = string_make_list();   /* 已加载文件路径 */
        /* 递归展开: 用显式栈 */
        size_t cap = 16, n = 1;
        const char **stack = malloc(cap * sizeof(char *));
        const char **dirs = malloc(cap * sizeof(char *));
        char *slash = strrchr(argv[1], '/');
        stack[0] = slash ? strdup(slash + 1) : strdup(argv[1]);   /* basename */
        dirs[0] = slash ? strndup(argv[1], slash - argv[1] + 1) : strdup("./");
        char *abs0 = malloc(strlen(dirs[0]) + strlen(stack[0]) + 1);
        sprintf(abs0, "%s%s", dirs[0], stack[0]);
        list_push(loaded, abs0);
        while (n > 0) {
            const char *path = stack[--n];
            const char *dir = dirs[n];
            char full[1024];
            snprintf(full, sizeof(full), "%s%s", dir, path);
            FILE *f2 = fopen(full, "rb");
            if (!f2) { fprintf(stderr, "[wvmc] 无法打开: %s\n", full); return 1; }
            fseek(f2, 0, SEEK_END);
            long sz2 = ftell(f2);
            fseek(f2, 0, SEEK_SET);
            char *text2 = malloc(sz2 + 1);
            fread(text2, 1, sz2, f2);
            text2[sz2] = 0;
            fclose(f2);
            list_t *sl2 = parse_stmt_list(sexp_parse_list(text2));
            stmt_t *st2 = list_first(sl2);
            while (st2) {
                if (st2->kind == STMT_IMPORT) {
                    /* 被导入文件: dir + 相对路径; stack 存 basename, dirs 存目录 */
                    const char *imp = path_string(st2->import.path);
                    char *impdir = malloc(strlen(dir) + strlen(imp) + 2);
                    sprintf(impdir, "%s%s", dir, imp);
                    char *norm = strdup(impdir);
                    char *bs = strrchr(impdir, '/');
                    char *base = bs ? strdup(bs + 1) : strdup(impdir);
                    bool dup = false;
                    {
                        char *lp = list_first(loaded);
                        while (lp) { if (string_equal(lp, norm)) { dup = true; break; }
                                     lp = list_next(loaded); }
                    }
                    if (!dup) {
                        if (n == cap) { cap *= 2; stack = realloc(stack, cap * 8); dirs = realloc(dirs, cap * 8); }
                        char *d = bs ? strndup(impdir, bs - impdir + 1) : strdup("");
                        stack[n] = base; dirs[n] = d; n++;
                        list_push(loaded, norm);
                    }
                } else {
                    list_push(merged, st2);
                }
                st2 = list_next(sl2);
            }
            free((void *)path);
        }
        free(stack); free(dirs);
        stmt_list = merged;
    }

    /* 4. 提取顶层 exprs */
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

    /* 5. 编译 → .asm */
    char tmp[] = "/tmp/wvmc_XXXXXX";
    int fd = mkstemp(tmp);
    FILE *ao = fdopen(fd, "w");
    compile_wvm_main(stmt_list, top_exps, ao);
    fclose(ao);

    /* 6. 调 warmasm.py 汇编 */
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
