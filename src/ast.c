#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

void stmtlist_init(StmtList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void stmtlist_push(StmtList *list, Stmt *stmt) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        list->items = realloc(list->items, list->capacity * sizeof(Stmt *));
    }
    list->items[list->count++] = stmt;
}

void writelist_init(WriteList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void writelist_push(WriteList *list, WriteItem item) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, list->capacity * sizeof(WriteItem));
    }
    list->items[list->count++] = item;
}

void paramlist_init(ParamList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void paramlist_push(ParamList *list, Param param) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, list->capacity * sizeof(Param));
    }
    list->items[list->count++] = param;
}

void exprlist_init(ExprList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void exprlist_push(ExprList *list, Expr *expr) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, list->capacity * sizeof(Expr *));
    }
    list->items[list->count++] = expr;
}

WriteItem ast_new_write_var(const char *name) {
    WriteItem item = {0};
    item.kind = WRITE_VAR;
    item.var_name = strdup(name);
    return item;
}

WriteItem ast_new_write_string(const char *str) {
    WriteItem item = {0};
    item.kind = WRITE_STRING;
    item.str = strdup(str);
    return item;
}

WriteItem ast_new_write_char(int ch) {
    WriteItem item = {0};
    item.kind = WRITE_CHAR;
    item.ch = ch;
    return item;
}

Stmt *ast_new_stmt(StmtKind kind, int line) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    return s;
}

Expr *ast_new_number(long value) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_NUMBER;
    e->number = value;
    return e;
}

Expr *ast_new_double(double value) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_DOUBLE;
    e->dnumber = value;
    return e;
}

Expr *ast_new_pi(void) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_PI;
    return e;
}

Expr *ast_new_string(const char *value) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_STRING;
    e->str = strdup(value);
    return e;
}

Expr *ast_new_char(int ch) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_CHAR;
    e->ch = ch;
    return e;
}

Expr *ast_new_ident(const char *name) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_IDENT;
    e->ident = strdup(name);
    return e;
}

Expr *ast_new_indexed(const char *name, Expr *index) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_INDEXED;
    e->ident = strdup(name);
    e->index = index;
    return e;
}

Expr *ast_new_call(const char *name, ExprList *args) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = EXPR_CALL;
    e->call_name = strdup(name);
    e->call_args = args;
    return e;
}

Condition *ast_new_condition(const char *left_ident, CondOp op, Expr *right) {
    Condition *c = calloc(1, sizeof(Condition));
    c->kind = COND_CMP;
    c->left_ident = strdup(left_ident);
    c->op = op;
    c->right = right;
    return c;
}

Condition *ast_new_logical(CondKind kind, Condition *left, Condition *right) {
    Condition *c = calloc(1, sizeof(Condition));
    c->kind = kind;
    c->left = left;
    c->right_cond = right;
    return c;
}

static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

static void print_char_lit(int ch);

static void print_expr(Expr *e) {
    switch (e->kind) {
        case EXPR_NUMBER: printf("%ld", e->number); break;
        case EXPR_DOUBLE: printf("%g", e->dnumber); break;
        case EXPR_PI: printf("pi"); break;
        case EXPR_STRING: printf("\"%s\"", e->str); break;
        case EXPR_CHAR: print_char_lit(e->ch); break;
        case EXPR_IDENT: printf("%s", e->ident); break;
        case EXPR_INDEXED:
            printf("%s[", e->ident);
            print_expr(e->index);
            printf("]");
            break;
        case EXPR_CALL:
            printf("call the function called %s", e->call_name);
            if (e->call_args && e->call_args->count > 0) {
                printf(" with the value of ");
                print_expr(e->call_args->items[0]);
                for (int i = 1; i < e->call_args->count; i++) {
                    printf(" and the value of ");
                    print_expr(e->call_args->items[i]);
                }
            }
            break;
    }
}

static const char *vtype_name(VarType t) {
    switch (t) {
        case VARTYPE_INTEGER: return "integer";
        case VARTYPE_DOUBLE: return "double";
        case VARTYPE_CHAR: return "char";
        case VARTYPE_STRING: return "string";
    }
    return "?";
}

static void print_char_lit(int ch) {
    switch (ch) {
        case '\n': printf("'\\n'"); break;
        case '\t': printf("'\\t'"); break;
        case '\r': printf("'\\r'"); break;
        case '\\': printf("'\\\\'"); break;
        case '\'': printf("'\\''"); break;
        default: printf("'%c'", (char)ch); break;
    }
}

static const char *cond_op_name(CondOp op) {
    switch (op) {
        case COND_LESS_THAN: return "<";
        case COND_LESS_EQUAL: return "<=";
        case COND_GREATER_THAN: return ">";
        case COND_GREATER_EQUAL: return ">=";
        case COND_EQUAL: return "==";
        case COND_NOT_EQUAL: return "!=";
    }
    return "?";
}

static const char *cond_kind_name(CondKind kind) {
    switch (kind) {
        case COND_CMP: return "";
        case COND_AND: return " and ";
        case COND_OR: return " or ";
    }
    return "?";
}

static const char *arith_op_name(ArithOp op) {
    switch (op) {
        case ARITH_ADD: return "+=";
        case ARITH_SUB: return "-=";
        case ARITH_MUL: return "*=";
        case ARITH_DIV: return "/=";
        case ARITH_MOD: return "%=";
    }
    return "?";
}

static void print_condition(Condition *c) {
    if (c->kind == COND_AND || c->kind == COND_OR) {
        printf("(");
        print_condition(c->left);
        printf(")%s(", cond_kind_name(c->kind));
        print_condition(c->right_cond);
        printf(")");
        return;
    }
    printf("%s", c->left_ident);
    if (c->left_index) {
        printf("[");
        print_expr(c->left_index);
        printf("]");
    }
    printf(" %s ", cond_op_name(c->op));
    print_expr(c->right);
}

static void print_stmt(Stmt *s, int depth) {
    print_indent(depth);
    switch (s->kind) {
        case STMT_DECL:
            printf("DECL %s : ", s->decl_name);
            if (s->decl_is_array) {
                printf("array of %s", vtype_name(s->decl_type));
                if (s->decl_size) {
                    printf(" size ");
                    print_expr(s->decl_size);
                }
            } else {
                printf("%s", vtype_name(s->decl_type));
                if (s->decl_init) {
                    printf(" = ");
                    print_expr(s->decl_init);
                }
            }
            printf("\n");
            break;
        case STMT_ASSIGN:
            printf("ASSIGN %s", s->assign_name);
            if (s->assign_index) {
                printf("[");
                print_expr(s->assign_index);
                printf("]");
            }
            printf(" = ");
            print_expr(s->assign_value);
            printf("\n");
            break;
        case STMT_ARITH:
            printf("ARITH %s", s->arith_dest);
            if (s->arith_dest_index) {
                printf("[");
                print_expr(s->arith_dest_index);
                printf("]");
            }
            printf(" %s %s", arith_op_name(s->arith_op), s->arith_src);
            if (s->arith_src_index) {
                printf("[");
                print_expr(s->arith_src_index);
                printf("]");
            }
            printf("\n");
            break;
        case STMT_WRITE:
            printf("WRITE");
            for (int i = 0; i < s->write_items->count; i++) {
                WriteItem *w = &s->write_items->items[i];
                printf(" %s", i == 0 ? "the" : "and the");
                switch (w->kind) {
                    case WRITE_VAR: {
                        printf(" variable %s", w->var_name);
                        if (w->index) {
                            printf("[");
                            print_expr(w->index);
                            printf("]");
                        }
                        break;
                    }
                    case WRITE_STRING: printf(" string \"%s\"", w->str); break;
                    case WRITE_CHAR:
                        printf(" char ");
                        print_char_lit(w->ch);
                        break;
                }
            }
            printf("\n");
            break;
        case STMT_READ:
            printf("READ_INTO %s", s->read_name);
            if (s->read_index) {
                printf("[");
                print_expr(s->read_index);
                printf("]");
            }
            printf("\n");
            break;
        case STMT_WAIT:
            printf("WAIT ");
            print_expr(s->wait_time);
            printf(" %s\n", s->wait_is_ms ? "milliseconds" : "seconds");
            break;
        case STMT_WHILE:
            printf("WHILE (");
            print_condition(s->while_cond);
            printf(")\n");
            for (int i = 0; i < s->while_body->count; i++) {
                print_stmt(s->while_body->items[i], depth + 1);
            }
            break;
        case STMT_WHEN:
            printf("WHEN (");
            print_condition(s->when_cond);
            printf(")\n");
            for (int i = 0; i < s->when_body->count; i++) {
                print_stmt(s->when_body->items[i], depth + 1);
            }
            if (s->when_else) {
                print_indent(depth);
                printf("ELSE\n");
                for (int i = 0; i < s->when_else->count; i++) {
                    print_stmt(s->when_else->items[i], depth + 1);
                }
            }
            break;
        case STMT_FUNC_DEF:
            printf("FUNC %s(", s->func_name);
            for (int i = 0; i < s->params->count; i++) {
                if (i) printf(", ");
                printf("%s : %s", s->params->items[i].name,
                       vtype_name(s->params->items[i].type));
            }
            printf(")\n");
            for (int i = 0; i < s->func_body->count; i++) {
                print_stmt(s->func_body->items[i], depth + 1);
            }
            break;
        case STMT_RETURN:
            printf("RETURN ");
            if (s->ret_value) print_expr(s->ret_value);
            printf("\n");
            break;
        case STMT_CALL:
            printf("CALL %s(", s->call_name);
            for (int i = 0; i < s->call_args->count; i++) {
                if (i) printf(", ");
                print_expr(s->call_args->items[i]);
            }
            printf(")\n");
            break;
    }
}

void ast_print_program(StmtList *program) {
    printf("PROGRAM\n");
    for (int i = 0; i < program->count; i++) {
        print_stmt(program->items[i], 1);
    }
}
