#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "bytecode.h"

/* The pi constant, as a double (avoid depending on M_PI being defined). */
#define AURA_PI 3.14159265358979323846

static void bc_error(int line, const char *fmt, ...) {
    fprintf(stderr, "aurascript: compile error at line %d: ", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

/* --- compilation state ---------------------------------------------------- */

/* The compiler lowers the program into a single shared code buffer. Each
 * lexical scope (the top level, or one function) owns its own variable
 * table; `Compiler` points at whichever table is currently in scope. */
typedef struct {
    Bytecode *bc;
    VarInfo *vars;      /* current scope's variable table */
    int var_count;
    int var_cap;
    int in_func;        /* true while compiling a function body */
} Compiler;

/* A resolved variable reference. `depth` selects the frame at run time:
 * 0 = the current (function/top-level) frame, 1 = the top-level global
 * frame; `index` is the slot within it. Functions fall back to the global
 * frame for names that are not their own params or locals, so they can
 * touch outer-scope variables and arrays. */
typedef struct {
    int depth;
    int index;
} VarRef;

/* --- byte stream helpers -------------------------------------------------- */

static void emit_byte(Compiler *c, uint8_t b) {
    Bytecode *bc = c->bc;
    if (bc->len >= bc->cap) {
        bc->cap = bc->cap == 0 ? 64 : bc->cap * 2;
        bc->code = realloc(bc->code, bc->cap);
    }
    bc->code[bc->len++] = b;
}

static void emit_u32(Compiler *c, uint32_t v) {
    for (int i = 0; i < 4; i++) emit_byte(c, (uint8_t)(v >> (i * 8)));
}

static void emit_i32(Compiler *c, int32_t v) {
    emit_u32(c, (uint32_t)v);
}

static void emit_i64(Compiler *c, int64_t v) {
    for (int i = 0; i < 8; i++) emit_byte(c, (uint8_t)((uint64_t)v >> (i * 8)));
}

static void emit_f64(Compiler *c, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    for (int i = 0; i < 8; i++) emit_byte(c, (uint8_t)(bits >> (i * 8)));
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t read_i64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return (int64_t)v;
}

static double read_f64(const uint8_t *p) {
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | p[i];
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static int32_t read_i32(const uint8_t *p) {
    return (int32_t)read_u32(p);
}

static void write_i32(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u & 0xff);
    p[1] = (uint8_t)((u >> 8) & 0xff);
    p[2] = (uint8_t)((u >> 16) & 0xff);
    p[3] = (uint8_t)((u >> 24) & 0xff);
}

/* --- symbol tables and constant pool -------------------------------------- */

static int scope_lookup(VarInfo *vars, int var_count, const char *name) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) return i;
    }
    return -1;
}

static int scope_declare(VarInfo **vars, int *var_count, int *var_cap,
                         const char *name, VarType type, int is_array) {
    int idx = scope_lookup(*vars, *var_count, name);
    if (idx >= 0) return idx; /* redeclaration reuses the first slot */

    if (*var_count >= *var_cap) {
        *var_cap = *var_cap == 0 ? 8 : *var_cap * 2;
        *vars = realloc(*vars, *var_cap * sizeof(VarInfo));
    }
    (*vars)[*var_count].name = strdup(name);
    (*vars)[*var_count].type = type;
    (*vars)[*var_count].is_array = is_array;
    return (*var_count)++;
}

static int var_declare(Compiler *c, const char *name, VarType type, int is_array) {
    return scope_declare(&c->vars, &c->var_count, &c->var_cap, name, type, is_array);
}

/* The variable table entry a resolved reference points at. */
static VarInfo *var_info(Compiler *c, VarRef ref) {
    return (ref.depth ? c->bc->vars : c->vars) + ref.index;
}

/* Resolves a name to a variable, checking the current scope first and then
 * falling back to the top-level (global) scope, so a function can read and
 * write outer-scope variables and arrays. Returns index -1 when undefined. */
static VarRef var_ref_lookup(Compiler *c, const char *name) {
    int idx = scope_lookup(c->vars, c->var_count, name);
    if (idx >= 0) return (VarRef){0, idx};
    idx = scope_lookup(c->bc->vars, c->bc->var_count, name);
    if (idx >= 0) return (VarRef){1, idx};
    return (VarRef){0, -1};
}

static int func_lookup(Compiler *c, const char *name) {
    for (int i = 0; i < c->bc->func_count; i++) {
        if (strcmp(c->bc->funcs[i].name, name) == 0) return i;
    }
    return -1;
}

static int string_lit(Compiler *c, const char *text) {
    Bytecode *bc = c->bc;
    if (bc->str_count >= bc->str_cap) {
        bc->str_cap = bc->str_cap == 0 ? 8 : bc->str_cap * 2;
        bc->strings = realloc(bc->strings, bc->str_cap * sizeof(char *));
    }
    bc->strings[bc->str_count] = strdup(text);
    return bc->str_count++;
}

/* --- instruction emission ------------------------------------------------- */

static void emit_op(Compiler *c, OpCode op) {
    emit_byte(c, (uint8_t)op);
}

static void emit_push_num(Compiler *c, long n) {
    emit_op(c, BC_PUSH_NUM);
    emit_i64(c, n);
}

static void emit_push_double(Compiler *c, double d) {
    emit_op(c, BC_PUSH_DOUBLE);
    emit_f64(c, d);
}

/* Pushes the pi constant. */
static void emit_push_pi(Compiler *c) {
    emit_push_double(c, AURA_PI);
}

static void emit_push_char(Compiler *c, int ch) {
    emit_op(c, BC_PUSH_CHAR);
    emit_i64(c, ch);
}

static void emit_push_str(Compiler *c, int str) {
    emit_op(c, BC_PUSH_STR);
    emit_u32(c, (uint32_t)str);
}

/* The variable operand: frame depth in the top 2 bits, slot index below. */
static void emit_var(Compiler *c, VarRef ref) {
    emit_u32(c, ((uint32_t)(ref.depth & 3) << 30) | (uint32_t)ref.index);
}

static void emit_load_var(Compiler *c, VarRef ref) {
    emit_op(c, BC_LOAD_VAR);
    emit_var(c, ref);
}

static void emit_store_int(Compiler *c, VarRef ref) {
    emit_op(c, BC_STORE_INT);
    emit_var(c, ref);
}

static void emit_store_double(Compiler *c, VarRef ref) {
    emit_op(c, BC_STORE_DOUBLE);
    emit_var(c, ref);
}

static void emit_store_char(Compiler *c, VarRef ref) {
    emit_op(c, BC_STORE_CHAR);
    emit_var(c, ref);
}

static void emit_store_str(Compiler *c, VarRef ref) {
    emit_op(c, BC_STORE_STR);
    emit_var(c, ref);
}

static void emit_print_var(Compiler *c, VarRef ref) {
    emit_op(c, BC_PRINT_VAR);
    emit_var(c, ref);
}

static void emit_print_str(Compiler *c, int str) {
    emit_op(c, BC_PRINT_STR);
    emit_u32(c, (uint32_t)str);
}

static void emit_print_char(Compiler *c, int ch) {
    emit_op(c, BC_PRINT_CHAR);
    emit_i64(c, ch);
}

static void emit_print_nl(Compiler *c) {
    emit_op(c, BC_PRINT_NL);
}

static void emit_read_line(Compiler *c, VarRef ref) {
    emit_op(c, BC_READ_LINE);
    emit_var(c, ref);
}

static void emit_make_array(Compiler *c, VarRef ref, VarType elem) {
    emit_op(c, BC_MAKE_ARRAY);
    emit_var(c, ref);
    emit_u32(c, (uint32_t)elem);
}

static void emit_load_index(Compiler *c, VarRef ref) {
    emit_op(c, BC_LOAD_INDEX);
    emit_var(c, ref);
}

static void emit_store_index(Compiler *c, VarRef ref, VarType elem) {
    emit_op(c, BC_STORE_INDEX);
    emit_var(c, ref);
    emit_u32(c, (uint32_t)elem);
}

static void emit_read_index(Compiler *c, VarRef ref) {
    emit_op(c, BC_READ_INDEX);
    emit_var(c, ref);
}

static void emit_sleep(Compiler *c, int is_ms) {
    emit_op(c, BC_SLEEP);
    emit_u32(c, (uint32_t)is_ms);
}

static void emit_add(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_ADD);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_add_double(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_ADD_DOUBLE);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_concat(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_CONCAT);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_sub(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_SUB);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_sub_double(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_SUB_DOUBLE);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_mul(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_MUL);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_mul_double(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_MUL_DOUBLE);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_div(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_DIV);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_div_double(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_DIV_DOUBLE);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_mod(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_MOD);
    emit_var(c, dst);
    emit_var(c, src);
}

static void emit_mod_double(Compiler *c, VarRef dst, VarRef src) {
    emit_op(c, BC_MOD_DOUBLE);
    emit_var(c, dst);
    emit_var(c, src);
}

/* Emits a jump instruction with a placeholder operand and returns the offset
 * of the operand so the caller can patch it once the target is known. */
static size_t emit_jump(Compiler *c, OpCode op) {
    emit_op(c, op);
    size_t pos = c->bc->len;
    emit_i32(c, 0);
    return pos;
}

static void patch_jump_to(Compiler *c, size_t operand_pos, size_t target) {
    int32_t rel = (int32_t)(target - (operand_pos + 4));
    write_i32(c->bc->code + operand_pos, rel);
}

/* --- expression and condition lowering ----------------------------------- */

static void emit_expr_call(Compiler *c, int line, const char *name, ExprList *args, int discard);
static void emit_expr_int(Compiler *c, int line, Expr *e);

/* Pushes any value (number, double, string, char, variable, array element
 * or function call) onto the stack. Used when storing into a string
 * variable, which accepts every value type. */
static void emit_expr_any(Compiler *c, int line, Expr *e) {
    switch (e->kind) {
        case EXPR_NUMBER: emit_push_num(c, e->number); break;
        case EXPR_DOUBLE: emit_push_double(c, e->dnumber); break;
        case EXPR_PI: emit_push_pi(c); break;
        case EXPR_STRING: emit_push_str(c, string_lit(c, e->str)); break;
        case EXPR_CHAR: emit_push_char(c, e->ch); break;
        case EXPR_IDENT: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (var_info(c, ref)->is_array)
                bc_error(line, "array variable '%s' must be indexed with 'at the index'", e->ident);
            emit_load_var(c, ref);
            break;
        }
        case EXPR_INDEXED: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (!var_info(c, ref)->is_array)
                bc_error(line, "variable '%s' is not an array", e->ident);
            emit_expr_int(c, line, e->index);
            emit_load_index(c, ref);
            break;
        }
        case EXPR_CALL:
            emit_expr_call(c, line, e->call_name, e->call_args, 0);
            break;
    }
}

/* Pushes an integer value: an integer or double literal, a character
 * literal, or an integer/char/double variable. Doubles are truncated when
 * stored, and strings are rejected. */
static void emit_expr_int(Compiler *c, int line, Expr *e) {
    switch (e->kind) {
        case EXPR_NUMBER: emit_push_num(c, e->number); break;
        case EXPR_CHAR: emit_push_char(c, e->ch); break;
        case EXPR_DOUBLE: emit_push_double(c, e->dnumber); break;
        case EXPR_PI: emit_push_pi(c); break;
        case EXPR_STRING:
            bc_error(line, "cannot use a string where an integer is expected");
            break;
        case EXPR_IDENT: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (var_info(c, ref)->is_array)
                bc_error(line, "array variable '%s' must be indexed with 'at the index'", e->ident);
            if (var_info(c, ref)->type == VARTYPE_STRING)
                bc_error(line, "cannot use variable '%s' as an integer", e->ident);
            emit_load_var(c, ref);
            break;
        }
        case EXPR_INDEXED: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (!var_info(c, ref)->is_array)
                bc_error(line, "variable '%s' is not an array", e->ident);
            if (var_info(c, ref)->type == VARTYPE_STRING)
                bc_error(line, "cannot use variable '%s' as an integer", e->ident);
            emit_expr_int(c, line, e->index);
            emit_load_index(c, ref);
            break;
        }
        case EXPR_CALL:
            emit_expr_call(c, line, e->call_name, e->call_args, 0);
            break;
    }
}

/* Pushes any numeric value (integer or double literal, char literal, or a
 * numeric variable). Strings are rejected. */
static void emit_expr_numeric(Compiler *c, int line, Expr *e) {
    switch (e->kind) {
        case EXPR_NUMBER: emit_push_num(c, e->number); break;
        case EXPR_DOUBLE: emit_push_double(c, e->dnumber); break;
        case EXPR_PI: emit_push_pi(c); break;
        case EXPR_CHAR: emit_push_char(c, e->ch); break;
        case EXPR_STRING:
            bc_error(line, "cannot use a string as a number");
            break;
        case EXPR_IDENT: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (var_info(c, ref)->is_array)
                bc_error(line, "array variable '%s' must be indexed with 'at the index'", e->ident);
            if (var_info(c, ref)->type == VARTYPE_STRING)
                bc_error(line, "cannot use string variable '%s' as a number", e->ident);
            emit_load_var(c, ref);
            break;
        }
        case EXPR_INDEXED: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (!var_info(c, ref)->is_array)
                bc_error(line, "variable '%s' is not an array", e->ident);
            if (var_info(c, ref)->type == VARTYPE_STRING)
                bc_error(line, "cannot use string variable '%s' as a number", e->ident);
            emit_expr_int(c, line, e->index);
            emit_load_index(c, ref);
            break;
        }
        case EXPR_CALL:
            emit_expr_call(c, line, e->call_name, e->call_args, 0);
            break;
    }
}

/* Pushes a character value: a character literal, an integer literal, a
 * single-character string, or an integer/char variable. */
static void emit_expr_char(Compiler *c, int line, Expr *e) {
    switch (e->kind) {
        case EXPR_NUMBER: emit_push_num(c, e->number); break;
        case EXPR_CHAR: emit_push_char(c, e->ch); break;
        case EXPR_STRING:
            if (strlen(e->str) != 1)
                bc_error(line, "expected a single character, got \"%s\"", e->str);
            emit_push_char(c, (unsigned char)e->str[0]);
            break;
        case EXPR_DOUBLE:
        case EXPR_PI:
            bc_error(line, "cannot store a floating-point value in a character");
            break;
        case EXPR_IDENT: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (var_info(c, ref)->is_array)
                bc_error(line, "array variable '%s' must be indexed with 'at the index'", e->ident);
            if (var_info(c, ref)->type != VARTYPE_INTEGER &&
                var_info(c, ref)->type != VARTYPE_CHAR)
                bc_error(line, "cannot use variable '%s' as a character", e->ident);
            emit_load_var(c, ref);
            break;
        }
        case EXPR_INDEXED: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (!var_info(c, ref)->is_array)
                bc_error(line, "variable '%s' is not an array", e->ident);
            if (var_info(c, ref)->type != VARTYPE_INTEGER &&
                var_info(c, ref)->type != VARTYPE_CHAR)
                bc_error(line, "cannot use variable '%s' as a character", e->ident);
            emit_expr_int(c, line, e->index);
            emit_load_index(c, ref);
            break;
        }
        case EXPR_CALL:
            emit_expr_call(c, line, e->call_name, e->call_args, 0);
            break;
    }
}

/* Emits code that leaves a string value on the stack: a string literal or a
 * string variable. */
static void emit_expr_string(Compiler *c, int line, Expr *e) {
    switch (e->kind) {
        case EXPR_STRING: emit_push_str(c, string_lit(c, e->str)); break;
        case EXPR_IDENT: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (var_info(c, ref)->is_array)
                bc_error(line, "array variable '%s' must be indexed with 'at the index'", e->ident);
            if (var_info(c, ref)->type != VARTYPE_STRING)
                bc_error(line, "cannot use variable '%s' where a string is expected", e->ident);
            emit_load_var(c, ref);
            break;
        }
        case EXPR_INDEXED: {
            VarRef ref = var_ref_lookup(c, e->ident);
            if (ref.index < 0) bc_error(line, "undefined variable '%s'", e->ident);
            if (!var_info(c, ref)->is_array)
                bc_error(line, "variable '%s' is not an array", e->ident);
            if (var_info(c, ref)->type != VARTYPE_STRING)
                bc_error(line, "cannot use variable '%s' where a string is expected", e->ident);
            emit_expr_int(c, line, e->index);
            emit_load_index(c, ref);
            break;
        }
        case EXPR_CALL:
            emit_expr_call(c, line, e->call_name, e->call_args, 0);
            break;
        default:
            bc_error(line, "expected a string literal or string variable");
            break;
    }
}

/* Pushes an argument in the shape the parameter's type wants, so obvious
 * mismatches are caught at compile time. */
static void emit_expr_for_type(Compiler *c, int line, VarType type, Expr *e) {
    switch (type) {
        case VARTYPE_INTEGER: emit_expr_int(c, line, e); break;
        case VARTYPE_DOUBLE: emit_expr_numeric(c, line, e); break;
        case VARTYPE_CHAR: emit_expr_char(c, line, e); break;
        case VARTYPE_STRING: emit_expr_any(c, line, e); break;
    }
}

/* The opcode for a built-in math function, or -1 when `name` isn't one.
 * These behave like single-argument user functions but lower to a single
 * VM instruction; a user-defined function with the same name wins, because
 * it is registered in pass 1 before any call is compiled. */
static int builtin_math_op(const char *name) {
    if (strcmp(name, "sin") == 0) return BC_MATH_SIN;
    if (strcmp(name, "cos") == 0) return BC_MATH_COS;
    if (strcmp(name, "sqrt") == 0) return BC_MATH_SQRT;
    return -1;
}

/* Emits `call the function called NAME with the value of args...` -- the
 * arguments (converted to each parameter's type) are pushed left to right,
 * then BC_CALL leaves exactly one result on the stack. When `discard` is
 * set, that result is popped right away (a call statement). */
static void emit_expr_call(Compiler *c, int line, const char *name, ExprList *args, int discard) {
    int fi = func_lookup(c, name);
    if (fi < 0) {
        int bop = builtin_math_op(name);
        if (bop < 0) bc_error(line, "undefined function '%s'", name);
        if (args->count != 1)
            bc_error(line, "function '%s' expects 1 argument, got %d", name, args->count);
        emit_expr_numeric(c, line, args->items[0]);
        emit_op(c, (OpCode)bop);
        if (discard) emit_op(c, BC_POP);
        return;
    }
    FuncInfo *f = &c->bc->funcs[fi];
    int argc = args->count;
    if (argc != f->param_count)
        bc_error(line, "function '%s' expects %d argument%s, got %d",
                 f->name, f->param_count, f->param_count == 1 ? "" : "s", argc);
    for (int i = 0; i < argc; i++)
        emit_expr_for_type(c, line, f->vars[i].type, args->items[i]);
    emit_op(c, BC_CALL);
    emit_u32(c, (uint32_t)fi);
    emit_u32(c, (uint32_t)argc);
    if (discard) emit_op(c, BC_POP);
}

/* Emits code that evaluates a condition to an int (0/1) on the stack:
 *   load left, push right, compare
 * Logical and/or combine the two sub-conditions with a pop/push opcode.
 * Strings compare lexicographically; everything else compares numerically. */
static void emit_condition(Compiler *c, int line, Condition *cond) {
    if (cond->kind == COND_AND || cond->kind == COND_OR) {
        emit_condition(c, line, cond->left);
        emit_condition(c, line, cond->right_cond);
        emit_op(c, cond->kind == COND_AND ? BC_AND : BC_OR);
        return;
    }
    VarRef left = var_ref_lookup(c, cond->left_ident);
    if (left.index < 0) bc_error(line, "undefined variable '%s'", cond->left_ident);
    if (cond->left_index) {
        if (!var_info(c, left)->is_array)
            bc_error(line, "variable '%s' is not an array", cond->left_ident);
        emit_expr_int(c, line, cond->left_index);
        emit_load_index(c, left);
    } else {
        if (var_info(c, left)->is_array)
            bc_error(line, "array variable '%s' must be indexed with 'at the index'", cond->left_ident);
        emit_load_var(c, left);
    }
    if (var_info(c, left)->type == VARTYPE_STRING) {
        emit_expr_string(c, line, cond->right);
    } else {
        emit_expr_numeric(c, line, cond->right);
    }
    switch (cond->op) {
        case COND_LESS_THAN: emit_op(c, BC_CMP_LT); break;
        case COND_GREATER_THAN: emit_op(c, BC_CMP_GT); break;
        case COND_EQUAL: emit_op(c, BC_CMP_EQ); break;
        case COND_LESS_EQUAL: emit_op(c, BC_CMP_LE); break;
        case COND_GREATER_EQUAL: emit_op(c, BC_CMP_GE); break;
        case COND_NOT_EQUAL: emit_op(c, BC_CMP_NE); break;
    }
}

/* --- statement lowering --------------------------------------------------- */

static void emit_stmt(Compiler *c, Stmt *s);

static void emit_body(Compiler *c, StmtList *body) {
    for (int i = 0; i < body->count; i++) emit_stmt(c, body->items[i]);
}

/* Emits code that evaluates `value` and stores it into the variable `var`
 * of the declared type `type`, applying the language's conversion rules:
 *   integer <- integer, char
 *   double  <- any number (int, double, char)
 *   char    <- number, char, single-character string
 *   string  <- anything (numbers become their decimal text)
 * When `index` is non-NULL the store targets `var[index]` instead of the
 * variable itself. */
static void emit_store_into(Compiler *c, int line, VarType type, VarRef var, Expr *value, Expr *index) {
    if (index) {
        switch (type) {
            case VARTYPE_INTEGER: emit_expr_int(c, line, value); break;
            case VARTYPE_DOUBLE: emit_expr_numeric(c, line, value); break;
            case VARTYPE_CHAR: emit_expr_char(c, line, value); break;
            case VARTYPE_STRING: emit_expr_any(c, line, value); break;
        }
        emit_expr_int(c, line, index);
        emit_store_index(c, var, type);
        return;
    }
    switch (type) {
        case VARTYPE_INTEGER:
            emit_expr_int(c, line, value);
            emit_store_int(c, var);
            break;
        case VARTYPE_DOUBLE:
            emit_expr_numeric(c, line, value);
            emit_store_double(c, var);
            break;
        case VARTYPE_CHAR:
            emit_expr_char(c, line, value);
            emit_store_char(c, var);
            break;
        case VARTYPE_STRING:
            emit_expr_any(c, line, value);
            emit_store_str(c, var);
            break;
    }
}

/* Emits code that pushes the value of an arithmetic operand (a variable or
 * an array element), for the stack-based arithmetic path. */
static void emit_arith_operand(Compiler *c, int line, VarRef var, Expr *index) {
    if (index) {
        emit_expr_int(c, line, index);
        emit_load_index(c, var);
    } else {
        if (var_info(c, var)->is_array)
            bc_error(line, "array variable '%s' must be indexed with 'at the index'", var_info(c, var)->name);
        emit_load_var(c, var);
    }
}

/* Emits code that stores the top of the stack back into an arithmetic
 * destination (a variable or an array element) of type `type`. */
static void emit_arith_store(Compiler *c, int line, VarRef var, Expr *index, VarType type) {
    if (index) {
        emit_expr_int(c, line, index);
        emit_store_index(c, var, type);
        return;
    }
    switch (type) {
        case VARTYPE_INTEGER: emit_store_int(c, var); break;
        case VARTYPE_DOUBLE: emit_store_double(c, var); break;
        case VARTYPE_CHAR: emit_store_char(c, var); break;
        case VARTYPE_STRING: emit_store_str(c, var); break;
    }
}

static void emit_stmt(Compiler *c, Stmt *s) {
    switch (s->kind) {
        case STMT_DECL: {
            if (s->decl_is_array) {
                int var = var_declare(c, s->decl_name, s->decl_type, 1);
                emit_expr_int(c, s->line, s->decl_size);
                emit_make_array(c, (VarRef){0, var}, s->decl_type);
                break;
            }
            int var = var_declare(c, s->decl_name, s->decl_type, 0);
            if (s->decl_init) {
                emit_store_into(c, s->line, s->decl_type, (VarRef){0, var}, s->decl_init, NULL);
            }
            break;
        }
        case STMT_ASSIGN: {
            VarRef var = var_ref_lookup(c, s->assign_name);
            if (var.index < 0) bc_error(s->line, "undefined variable '%s'", s->assign_name);
            if (var_info(c, var)->is_array && !s->assign_index)
                bc_error(s->line, "array variable '%s' must be indexed with 'at the index'", s->assign_name);
            if (s->assign_index && !var_info(c, var)->is_array)
                bc_error(s->line, "variable '%s' is not an array", s->assign_name);
            emit_store_into(c, s->line, var_info(c, var)->type, var, s->assign_value, s->assign_index);
            break;
        }
        case STMT_ARITH: {
            VarRef src = var_ref_lookup(c, s->arith_src);
            VarRef dst = var_ref_lookup(c, s->arith_dest);
            if (src.index < 0) bc_error(s->line, "undefined variable '%s'", s->arith_src);
            if (dst.index < 0) bc_error(s->line, "undefined variable '%s'", s->arith_dest);
            if (s->arith_src_index && !var_info(c, src)->is_array)
                bc_error(s->line, "variable '%s' is not an array", s->arith_src);
            if (s->arith_dest_index && !var_info(c, dst)->is_array)
                bc_error(s->line, "variable '%s' is not an array", s->arith_dest);
            VarType st = var_info(c, src)->type;
            VarType dt = var_info(c, dst)->type;
            int indexed = s->arith_src_index != NULL || s->arith_dest_index != NULL;

            /* String concatenation is only defined for 'add'. */
            if (s->arith_op != ARITH_ADD && (st == VARTYPE_STRING || dt == VARTYPE_STRING))
                bc_error(s->line, "cannot use a string with an arithmetic operator");
            if (s->arith_op == ARITH_ADD && st == VARTYPE_STRING && dt == VARTYPE_STRING) {
                if (indexed) {
                    emit_arith_operand(c, s->line, dst, s->arith_dest_index);
                    emit_arith_operand(c, s->line, src, s->arith_src_index);
                    emit_op(c, BC_ADD_STK);
                    emit_arith_store(c, s->line, dst, s->arith_dest_index, dt);
                } else {
                    emit_concat(c, dst, src);
                }
                break;
            }
            if (st == VARTYPE_STRING || dt == VARTYPE_STRING)
                bc_error(s->line, "cannot add a string and a number");

            if (indexed) {
                emit_arith_operand(c, s->line, dst, s->arith_dest_index);
                emit_arith_operand(c, s->line, src, s->arith_src_index);
                switch (s->arith_op) {
                    case ARITH_ADD: emit_op(c, BC_ADD_STK); break;
                    case ARITH_SUB: emit_op(c, BC_SUB_STK); break;
                    case ARITH_MUL: emit_op(c, BC_MUL_STK); break;
                    case ARITH_DIV: emit_op(c, BC_DIV_STK); break;
                    case ARITH_MOD: emit_op(c, BC_MOD_STK); break;
                }
                emit_arith_store(c, s->line, dst, s->arith_dest_index, dt);
                break;
            }

            if (st == VARTYPE_DOUBLE || dt == VARTYPE_DOUBLE) {
                switch (s->arith_op) {
                    case ARITH_ADD: emit_add_double(c, dst, src); break;
                    case ARITH_SUB: emit_sub_double(c, dst, src); break;
                    case ARITH_MUL: emit_mul_double(c, dst, src); break;
                    case ARITH_DIV: emit_div_double(c, dst, src); break;
                    case ARITH_MOD: emit_mod_double(c, dst, src); break;
                }
            } else if ((st == VARTYPE_INTEGER || st == VARTYPE_CHAR) &&
                       (dt == VARTYPE_INTEGER || dt == VARTYPE_CHAR)) {
                switch (s->arith_op) {
                    case ARITH_ADD: emit_add(c, dst, src); break;
                    case ARITH_SUB: emit_sub(c, dst, src); break;
                    case ARITH_MUL: emit_mul(c, dst, src); break;
                    case ARITH_DIV: emit_div(c, dst, src); break;
                    case ARITH_MOD: emit_mod(c, dst, src); break;
                }
            } else {
                bc_error(s->line, "cannot apply an arithmetic operator to variables of these types");
            }
            break;
        }
        case STMT_WRITE: {
            for (int i = 0; i < s->write_items->count; i++) {
                WriteItem *w = &s->write_items->items[i];
                switch (w->kind) {
                    case WRITE_VAR: {
                        VarRef var = var_ref_lookup(c, w->var_name);
                        if (var.index < 0) bc_error(s->line, "undefined variable '%s'", w->var_name);
                        if (w->index) {
                            if (!var_info(c, var)->is_array)
                                bc_error(s->line, "variable '%s' is not an array", w->var_name);
                            emit_expr_int(c, s->line, w->index);
                            emit_load_index(c, var);
                            emit_op(c, BC_PRINT_STK);
                        } else {
                            if (var_info(c, var)->is_array)
                                bc_error(s->line, "array variable '%s' must be indexed with 'at the index'", w->var_name);
                            emit_print_var(c, var);
                        }
                        break;
                    }
                    case WRITE_STRING: {
                        int str = string_lit(c, w->str);
                        emit_print_str(c, str);
                        break;
                    }
                    case WRITE_CHAR:
                        emit_print_char(c, w->ch);
                        break;
                }
            }
            emit_print_nl(c);
            break;
        }
        case STMT_READ: {
            VarRef var = var_ref_lookup(c, s->read_name);
            if (var.index < 0) bc_error(s->line, "undefined variable '%s'", s->read_name);
            if (s->read_index) {
                if (!var_info(c, var)->is_array)
                    bc_error(s->line, "variable '%s' is not an array", s->read_name);
                emit_expr_int(c, s->line, s->read_index);
                emit_read_index(c, var);
            } else {
                if (var_info(c, var)->is_array)
                    bc_error(s->line, "array variable '%s' must be indexed with 'at the index'", s->read_name);
                emit_read_line(c, var);
            }
            break;
        }
        case STMT_WAIT: {
            emit_expr_numeric(c, s->line, s->wait_time);
            emit_sleep(c, s->wait_is_ms);
            break;
        }
        case STMT_WHILE: {
            size_t loop_start = c->bc->len;
            emit_condition(c, s->line, s->while_cond);
            size_t exit_patch = emit_jump(c, BC_JMP_IF_FALSE);
            emit_body(c, s->while_body);
            size_t back_patch = emit_jump(c, BC_JMP);
            patch_jump_to(c, back_patch, loop_start);
            patch_jump_to(c, exit_patch, c->bc->len);
            break;
        }
        case STMT_WHEN: {
            emit_condition(c, s->line, s->when_cond);
            size_t else_patch = emit_jump(c, BC_JMP_IF_FALSE);
            emit_body(c, s->when_body);
            if (s->when_else) {
                size_t end_patch = emit_jump(c, BC_JMP);
                patch_jump_to(c, else_patch, c->bc->len);
                emit_body(c, s->when_else);
                patch_jump_to(c, end_patch, c->bc->len);
            } else {
                patch_jump_to(c, else_patch, c->bc->len);
            }
            break;
        }
        case STMT_FUNC_DEF:
            /* Function bodies are compiled in a separate pass (see
             * bytecode_compile); nothing is emitted inline here. */
            break;
        case STMT_RETURN: {
            if (!c->in_func) bc_error(s->line, "return can only be used inside a function");
            if (s->ret_value) emit_expr_any(c, s->line, s->ret_value);
            else { emit_op(c, BC_PUSH_NUM); emit_i64(c, 0); }
            emit_op(c, BC_RET);
            break;
        }
        case STMT_CALL:
            emit_expr_call(c, s->line, s->call_name, s->call_args, 1);
            break;
    }
}

/* --- function registration ------------------------------------------------ */

/* Records every function definition reachable from `list`, in order, so
 * that calls (including forward references) resolve to stable indices. */
static void register_func(Compiler *c, Stmt *s);
static void register_funcs(Compiler *c, StmtList *list) {
    for (int i = 0; i < list->count; i++) {
        Stmt *s = list->items[i];
        switch (s->kind) {
            case STMT_FUNC_DEF: register_func(c, s); break;
            case STMT_WHILE: register_funcs(c, s->while_body); break;
            case STMT_WHEN:
                register_funcs(c, s->when_body);
                if (s->when_else) register_funcs(c, s->when_else);
                break;
            default: break;
        }
    }
}

static void register_func(Compiler *c, Stmt *s) {
    Bytecode *bc = c->bc;
    if (bc->func_count >= bc->func_cap) {
        bc->func_cap = bc->func_cap == 0 ? 8 : bc->func_cap * 2;
        bc->funcs = realloc(bc->funcs, bc->func_cap * sizeof(FuncInfo));
    }
    FuncInfo *f = &bc->funcs[bc->func_count];
    memset(f, 0, sizeof(*f));
    f->name = strdup(s->func_name);
    f->param_count = s->params->count;
    f->body = s->func_body;
    for (int i = 0; i < f->param_count; i++) {
        Param *prm = &s->params->items[i];
        scope_declare(&f->vars, &f->var_count, &f->var_cap, prm->name, prm->type, 0);
    }
    bc->func_count++;
}

Bytecode *bytecode_compile(StmtList *program) {
    Bytecode *bc = calloc(1, sizeof(Bytecode));
    Compiler top = {bc, NULL, 0, 0, 0};

    /* Pass 1: register every function before any code is emitted. */
    register_funcs(&top, program);

    /* Pass 2: compile the top-level program. Function definitions emit
     * nothing; they are compiled into the stream by pass 3. */
    emit_body(&top, program);
    emit_op(&top, BC_HALT);

    bc->vars = top.vars;
    bc->var_count = top.var_count;
    bc->var_cap = top.var_cap;

    /* Pass 3: compile each function body after the main program. A function
     * that runs off the end returns 0. */
    for (int i = 0; i < bc->func_count; i++) {
        FuncInfo *f = &bc->funcs[i];
        f->entry = bc->len;
        Compiler fc = {bc, f->vars, f->var_count, f->var_cap, 1};
        emit_body(&fc, f->body);
        f->var_count = fc.var_count;
        f->var_cap = fc.var_cap;
        f->vars = fc.vars;
        emit_op(&fc, BC_PUSH_NUM);
        emit_i64(&fc, 0);
        emit_op(&fc, BC_RET);
        f->end = bc->len;
    }

    return bc;
}

void bytecode_free(Bytecode *bc) {
    if (!bc) return;
    free(bc->code);
    for (int i = 0; i < bc->str_count; i++) free(bc->strings[i]);
    free(bc->strings);
    for (int i = 0; i < bc->var_count; i++) free(bc->vars[i].name);
    free(bc->vars);
    for (int i = 0; i < bc->func_count; i++) {
        free(bc->funcs[i].name);
        for (int v = 0; v < bc->funcs[i].var_count; v++) free(bc->funcs[i].vars[v].name);
        free(bc->funcs[i].vars);
    }
    free(bc->funcs);
    free(bc);
}

/* --- disassembly ---------------------------------------------------------- */

static const char *opcode_name(OpCode op) {
    switch (op) {
        case BC_HALT: return "halt";
        case BC_PUSH_NUM: return "push_num";
        case BC_PUSH_DOUBLE: return "push_double";
        case BC_PUSH_STR: return "push_str";
        case BC_PUSH_CHAR: return "push_char";
        case BC_LOAD_VAR: return "load_var";
        case BC_STORE_INT: return "store_int";
        case BC_STORE_DOUBLE: return "store_double";
        case BC_STORE_CHAR: return "store_char";
        case BC_STORE_STR: return "store_str";
        case BC_ADD: return "add";
        case BC_ADD_DOUBLE: return "add_double";
        case BC_CONCAT: return "concat";
        case BC_SUB: return "sub";
        case BC_SUB_DOUBLE: return "sub_double";
        case BC_MUL: return "mul";
        case BC_MUL_DOUBLE: return "mul_double";
        case BC_DIV: return "div";
        case BC_DIV_DOUBLE: return "div_double";
        case BC_MOD: return "mod";
        case BC_MOD_DOUBLE: return "mod_double";
        case BC_PRINT_VAR: return "print_var";
        case BC_PRINT_STR: return "print_str";
        case BC_PRINT_CHAR: return "print_char";
        case BC_PRINT_NL: return "print_nl";
        case BC_READ_LINE: return "read_line";
        case BC_SLEEP: return "sleep";
        case BC_CMP_LT: return "cmp_lt";
        case BC_CMP_GT: return "cmp_gt";
        case BC_CMP_EQ: return "cmp_eq";
        case BC_CMP_LE: return "cmp_le";
        case BC_CMP_GE: return "cmp_ge";
        case BC_CMP_NE: return "cmp_ne";
        case BC_AND: return "and";
        case BC_OR: return "or";
        case BC_JMP: return "jmp";
        case BC_JMP_IF_FALSE: return "jmp_if_false";
        case BC_CALL: return "call";
        case BC_RET: return "ret";
        case BC_POP: return "pop";
        case BC_MAKE_ARRAY: return "make_array";
        case BC_LOAD_INDEX: return "load_index";
        case BC_STORE_INDEX: return "store_index";
        case BC_READ_INDEX: return "read_index";
        case BC_PRINT_STK: return "print_stk";
        case BC_ADD_STK: return "add_stk";
        case BC_SUB_STK: return "sub_stk";
        case BC_MUL_STK: return "mul_stk";
        case BC_DIV_STK: return "div_stk";
        case BC_MOD_STK: return "mod_stk";
        case BC_MATH_SIN: return "sin";
        case BC_MATH_COS: return "cos";
        case BC_MATH_SQRT: return "sqrt";
    }
    return "?";
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

static void print_var_type(FILE *out, const VarInfo *v) {
    if (v->is_array) fprintf(out, "array of %s", vtype_name(v->type));
    else fprintf(out, "%s", vtype_name(v->type));
}

/* Decodes a variable operand (frame depth in the top 2 bits) and prints it
 * as `[idx] name`, or `[g:idx] name` when it refers to the global frame.
 * `cur` is the table of the region the instruction sits in. */
static void disasm_var(FILE *out, Bytecode *bc, const VarInfo *cur, uint32_t op) {
    int depth = (int)((op >> 30) & 0x3);
    int idx = (int)(op & 0x3fffffff);
    const VarInfo *vt = depth ? bc->vars : cur;
    fprintf(out, "[%s%d] %s", depth ? "g:" : "", idx, vt[idx].name);
}

static void print_char_lit(FILE *out, int ch) {
    switch (ch) {
        case '\n': fputs("'\\n'", out); break;
        case '\t': fputs("'\\t'", out); break;
        case '\r': fputs("'\\r'", out); break;
        case '\\': fputs("'\\\\'", out); break;
        case '\'': fputs("'\\''", out); break;
        default: fprintf(out, "'%c'", (char)ch); break;
    }
}

void bytecode_disasm(Bytecode *bc, FILE *out) {
    fprintf(out, "; variables\n");
    for (int i = 0; i < bc->var_count; i++) {
        fprintf(out, ";   [%d] %s : ", i, bc->vars[i].name);
        print_var_type(out, &bc->vars[i]);
        fprintf(out, "\n");
    }
    fprintf(out, "; strings\n");
    for (int i = 0; i < bc->str_count; i++) {
        fprintf(out, ";   [%d] \"%s\"\n", i, bc->strings[i]);
    }
    fprintf(out, "; functions\n");
    for (int i = 0; i < bc->func_count; i++) {
        FuncInfo *f = &bc->funcs[i];
        fprintf(out, ";   [%d] %s(%d) @ %04zu..%04zu\n", i, f->name, f->param_count, f->entry, f->end);
        for (int v = 0; v < f->var_count; v++) {
            fprintf(out, ";     [%d] %s : ", v, f->vars[v].name);
            print_var_type(out, &f->vars[v]);
            fprintf(out, "%s\n", v < f->param_count ? " (param)" : "");
        }
    }
    fprintf(out, "; code\n");

    size_t ip = 0;
    int fi = -1; /* function region the current ip falls in, -1 = top level */
    while (ip < bc->len) {
        while (fi + 1 < bc->func_count && ip >= bc->funcs[fi + 1].entry) fi++;
        VarInfo *vt = bc->vars;
        if (fi >= 0) {
            vt = bc->funcs[fi].vars;
        }

        OpCode op = (OpCode)bc->code[ip];
        fprintf(out, "%04zu: %-12s", ip, opcode_name(op));
        switch (op) {
            case BC_PUSH_NUM:
                fprintf(out, "%ld", (long)read_i64(bc->code + ip + 1));
                ip += 9;
                break;
            case BC_PUSH_DOUBLE:
                fprintf(out, "%g", read_f64(bc->code + ip + 1));
                ip += 9;
                break;
            case BC_PUSH_CHAR:
                print_char_lit(out, (int)read_i64(bc->code + ip + 1));
                ip += 9;
                break;
            case BC_PRINT_CHAR:
                print_char_lit(out, (int)read_i64(bc->code + ip + 1));
                ip += 9;
                break;
            case BC_PUSH_STR: {
                uint32_t ci = read_u32(bc->code + ip + 1);
                fprintf(out, "[%u] \"%s\"", ci, bc->strings[ci]);
                ip += 5;
                break;
            }
            case BC_LOAD_VAR:
            case BC_STORE_INT:
            case BC_STORE_DOUBLE:
            case BC_STORE_CHAR:
            case BC_STORE_STR:
            case BC_PRINT_VAR: {
                uint32_t vi = read_u32(bc->code + ip + 1);
                disasm_var(out, bc, vt, vi);
                ip += 5;
                break;
            }
            case BC_LOAD_INDEX:
            case BC_READ_INDEX: {
                uint32_t vi = read_u32(bc->code + ip + 1);
                disasm_var(out, bc, vt, vi);
                fputs("[", out);
                ip += 5;
                break;
            }
            case BC_MAKE_ARRAY:
            case BC_STORE_INDEX: {
                uint32_t vi = read_u32(bc->code + ip + 1);
                uint32_t et = read_u32(bc->code + ip + 5);
                disasm_var(out, bc, vt, vi);
                fprintf(out, " : %s", vtype_name((VarType)et));
                ip += 9;
                break;
            }
            case BC_ADD:
            case BC_ADD_DOUBLE:
            case BC_CONCAT:
            case BC_SUB:
            case BC_SUB_DOUBLE:
            case BC_MUL:
            case BC_MUL_DOUBLE:
            case BC_DIV:
            case BC_DIV_DOUBLE:
            case BC_MOD:
            case BC_MOD_DOUBLE: {
                uint32_t d = read_u32(bc->code + ip + 1);
                uint32_t s = read_u32(bc->code + ip + 5);
                const char *sym = "+=";
                if (op == BC_SUB || op == BC_SUB_DOUBLE) sym = "-=";
                else if (op == BC_MUL || op == BC_MUL_DOUBLE) sym = "*=";
                else if (op == BC_DIV || op == BC_DIV_DOUBLE) sym = "/=";
                else if (op == BC_MOD || op == BC_MOD_DOUBLE) sym = "%=";
                disasm_var(out, bc, vt, d);
                fprintf(out, " %s ", sym);
                disasm_var(out, bc, vt, s);
                ip += 9;
                break;
            }
            case BC_PRINT_STR: {
                uint32_t ci = read_u32(bc->code + ip + 1);
                fprintf(out, "[%u] \"%s\"", ci, bc->strings[ci]);
                ip += 5;
                break;
            }
            case BC_READ_LINE: {
                uint32_t vi = read_u32(bc->code + ip + 1);
                disasm_var(out, bc, vt, vi);
                ip += 5;
                break;
            }
            case BC_SLEEP: {
                uint32_t unit = read_u32(bc->code + ip + 1);
                fprintf(out, "%s", unit ? "milliseconds" : "seconds");
                ip += 5;
                break;
            }
            case BC_CALL: {
                uint32_t fi_op = read_u32(bc->code + ip + 1);
                uint32_t argc = read_u32(bc->code + ip + 5);
                fprintf(out, "[%u] %s(%u)", fi_op, bc->funcs[fi_op].name, argc);
                ip += 9;
                break;
            }
            case BC_JMP:
            case BC_JMP_IF_FALSE: {
                int32_t rel = read_i32(bc->code + ip + 1);
                fprintf(out, "-> %ld", (long)ip + 5 + rel);
                ip += 5;
                break;
            }
            case BC_CMP_LT:
            case BC_CMP_GT:
            case BC_CMP_EQ:
            case BC_CMP_LE:
            case BC_CMP_GE:
            case BC_CMP_NE:
            case BC_AND:
            case BC_OR:
            case BC_PRINT_NL:
            case BC_RET:
            case BC_POP:
            case BC_PRINT_STK:
            case BC_ADD_STK:
            case BC_SUB_STK:
            case BC_MUL_STK:
            case BC_DIV_STK:
            case BC_MOD_STK:
            case BC_MATH_SIN:
            case BC_MATH_COS:
            case BC_MATH_SQRT:
            case BC_HALT:
                ip += 1;
                break;
        }
        fprintf(out, "\n");
    }
}