#ifndef BYTECODE_H
#define BYTECODE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "ast.h"

/* Opcode set for the Aurascript VM. Instructions are a flat byte stream,
 * little-endian, with the operand layout described in each comment:
 *
 *   u32 = 4-byte unsigned little-endian index
 *   i64 = 8-byte signed little-endian immediate
 *   i32 = 4-byte signed little-endian relative jump offset
 *
 * A `u32 var` operand is a variable reference: its top 2 bits are the frame
 * depth (0 = the current frame, 1 = the top-level global frame) and the low
 * 30 bits are the slot index within that frame. Functions fall back to the
 * global frame for names that are not their own params or locals, so they
 * can read and write outer-scope variables and arrays. */
typedef enum {
    BC_HALT = 0,        /* stop execution */
    BC_PUSH_NUM,        /* i64 imm        push an integer literal */
    BC_PUSH_DOUBLE,     /* f64 imm        push a floating-point literal */
    BC_PUSH_STR,        /* u32 const      push a string literal */
    BC_PUSH_CHAR,       /* i64 imm        push a character literal */
    BC_LOAD_VAR,        /* u32 var        push a copy of a variable's value */
    BC_STORE_INT,       /* u32 var        pop a number, store as that variable's int value */
    BC_STORE_DOUBLE,    /* u32 var        pop a number, store as that variable's double value */
    BC_STORE_CHAR,      /* u32 var        pop a number-or-single-char, store as that variable's char */
    BC_STORE_STR,       /* u32 var        pop a value, store as that variable's string */
    BC_ADD,             /* u32 dst u32 src  dst += src (integer variables) */
    BC_ADD_DOUBLE,      /* u32 dst u32 src  dst += src (double variables) */
    BC_CONCAT,          /* u32 dst u32 src  dst = dst + src (string concatenation) */
    BC_SUB,             /* u32 dst u32 src  dst -= src (integer variables) */
    BC_SUB_DOUBLE,      /* u32 dst u32 src  dst -= src (double variables) */
    BC_MUL,             /* u32 dst u32 src  dst *= src (integer variables) */
    BC_MUL_DOUBLE,      /* u32 dst u32 src  dst *= src (double variables) */
    BC_DIV,             /* u32 dst u32 src  dst /= src (integer variables) */
    BC_DIV_DOUBLE,      /* u32 dst u32 src  dst /= src (double variables) */
    BC_MOD,             /* u32 dst u32 src  dst %= src (integer variables) */
    BC_MOD_DOUBLE,      /* u32 dst u32 src  dst %= src (double variables) */
    BC_PRINT_VAR,       /* u32 var        print a variable (no newline) */
    BC_PRINT_STR,       /* u32 const      print a string literal (no newline) */
    BC_PRINT_CHAR,      /* i64 imm        print a character literal (no newline) */
    BC_PRINT_NL,        /* —              print a newline */
    BC_READ_LINE,       /* u32 var        read a line of stdin into a variable */
    BC_SLEEP,           /* u32 unit       pop a number; sleep that long (0 = seconds, 1 = milliseconds) */
    BC_CMP_LT,          /* pop right, pop left, push (left < right) as int */
    BC_CMP_GT,          /* pop right, pop left, push (left > right) as int */
    BC_CMP_EQ,          /* pop right, pop left, push (left == right) as int */
    BC_CMP_LE,          /* pop right, pop left, push (left <= right) as int */
    BC_CMP_GE,          /* pop right, pop left, push (left >= right) as int */
    BC_CMP_NE,          /* pop right, pop left, push (left != right) as int */
    BC_AND,             /* pop right, pop left, push (left && right) as int */
    BC_OR,              /* pop right, pop left, push (left || right) as int */
    BC_JMP,             /* i32 rel        unconditional relative jump */
    BC_JMP_IF_FALSE,    /* i32 rel        pop int; jump when it is false (0) */
    BC_CALL,            /* u32 func u32 argc  call a function; args are on the stack, pushes one result */
    BC_RET,             /* pop a value; return it to the caller, leaving it on the stack */
    BC_POP,             /* pop and discard a value */
    /* arrays */
    BC_MAKE_ARRAY,      /* u32 var u32 elem_type  pop a size; allocate that many element slots for the variable */
    BC_LOAD_INDEX,      /* u32 var                pop an index; push a copy of the element at that index */
    BC_STORE_INDEX,     /* u32 var u32 elem_type  pop an index, pop a value; store the value as that element */
    BC_READ_INDEX,      /* u32 var                pop an index; read a line of stdin into that element */
    BC_PRINT_STK,       /* pop a value; print it (no newline) */
    BC_ADD_STK,         /* pop right, pop left, push (left + right): numeric, or concatenation when both strings */
    BC_SUB_STK,         /* pop right, pop left, push (left - right) */
    BC_MUL_STK,         /* pop right, pop left, push (left * right) */
    BC_DIV_STK,         /* pop right, pop left, push (left / right) */
    BC_MOD_STK,         /* pop right, pop left, push (left % right) */
    /* math */
    BC_MATH_SIN,        /* pop a number; push sin(number) as a double */
    BC_MATH_COS,        /* pop a number; push cos(number) as a double */
    BC_MATH_SQRT,       /* pop a number; push sqrt(number) as a double (error when negative) */
} OpCode;

typedef struct {
    char *name;
    VarType type;       /* element type when is_array */
    int is_array;
} VarInfo;

/* A compiled function. Its code lives in the shared byte stream after the
 * main program; `vars` is its own table of parameters (first `param_count`
 * slots) followed by any locals declared in its body. Variable references
 * in the body fall back to the top-level table for outer-scope names. */
typedef struct {
    char *name;
    size_t entry;       /* offset of the first byte of the function body */
    size_t end;         /* offset one past the function's last byte */
    VarInfo *vars;      /* frame-relative variable table */
    int var_count;
    int var_cap;
    int param_count;    /* number of leading slots that are parameters */
    StmtList *body;     /* AST body, used only while compiling */
} FuncInfo;

typedef struct {
    uint8_t *code;      /* instruction stream */
    size_t len;
    size_t cap;
    char **strings;     /* string constant pool */
    int str_count;
    int str_cap;
    VarInfo *vars;      /* declared top-level variables, indexed by operand */
    int var_count;
    int var_cap;
    FuncInfo *funcs;    /* compiled functions, in definition order */
    int func_count;
    int func_cap;
} Bytecode;

/* Compiles the program AST to bytecode. Variables are resolved to indices
 * (redeclaration reuses the first slot); the constant pool collects string
 * literals. Type and undefined-variable errors are reported here, before the
 * VM ever runs, with a diagnostic to stderr and exit(1). */
Bytecode *bytecode_compile(StmtList *program);

/* Prints a human-readable disassembly of the compiled program to `out`. */
void bytecode_disasm(Bytecode *bc, FILE *out);

void bytecode_free(Bytecode *bc);

#endif