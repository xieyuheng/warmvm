/* 测试: 解析一个 .lisp 文件 */
#include "lang/index.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.lisp\n", argv[0]); return 1; }
    file_t *file = file_open_or_fail(argv[1], "r");
    if (!file) { perror(argv[1]); return 1; }
    char *text = NULL;
    {   // 读整个文件
        fseek(file, 0, SEEK_END);
        long sz = ftell(file);
        fseek(file, 0, SEEK_SET);
        text = malloc(sz + 1);
        fread(text, 1, sz, file);
        text[sz] = 0;
    }
    list_t *sexp_list = sexp_parse_list(text);
    list_t *stmt_list = parse_stmt_list(sexp_list);
    stmt_t *stmt = list_first(stmt_list);
    while (stmt) {
        stmt_print(stmt, stdout);
        stmt = list_next(stmt_list);
    }
    return 0;
}
