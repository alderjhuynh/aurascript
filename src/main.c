#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "bytecode.h"
#include "vm.h"

typedef enum {
    MODE_RUN,
    MODE_AST,
    MODE_BYTECODE
} Mode;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "aurascript: could not open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--ast | --bytecode [out.txt]] <script.ausc>\n"
            "\n"
            "  (no flag)            compile the script to bytecode and run it in the VM\n"
            "  --ast                print the parsed AST\n"
            "  --bytecode [f]       compile to bytecode and disassemble to f (default stdout)\n",
            prog);
}

int main(int argc, char **argv) {
    Mode mode = MODE_RUN;
    int idx = 1;
    const char *outfile = NULL;

    if (argc > 1 && argv[1][0] == '-') {
        if (strcmp(argv[1], "--ast") == 0) {
            mode = MODE_AST;
        } else if (strcmp(argv[1], "--bytecode") == 0) {
            mode = MODE_BYTECODE;
        } else {
            fprintf(stderr, "aurascript: unknown option '%s'\n", argv[1]);
            usage(argv[0]);
            return 1;
        }
        idx = 2;
    }

    if (argc <= idx) {
        usage(argv[0]);
        return 1;
    }
    const char *script = argv[idx++];
    if (mode == MODE_BYTECODE && idx < argc) outfile = argv[idx++];

    char *src = read_file(script);

    int token_count;
    Token *tokens = lexer_tokenize(src, &token_count);

    Parser p;
    parser_init(&p, tokens, token_count);
    StmtList *program = parser_parse_program(&p);

    switch (mode) {
        case MODE_AST:
            ast_print_program(program);
            break;
        case MODE_BYTECODE: {
            Bytecode *bc = bytecode_compile(program);
            FILE *out = stdout;
            if (outfile) {
                out = fopen(outfile, "w");
                if (!out) {
                    fprintf(stderr, "aurascript: could not open '%s'\n", outfile);
                    return 1;
                }
            }
            bytecode_disasm(bc, out);
            if (outfile) fclose(out);
            bytecode_free(bc);
            break;
        }
        case MODE_RUN: {
            Bytecode *bc = bytecode_compile(program);
            vm_run(bc);
            bytecode_free(bc);
            break;
        }
    }

    return 0;
}