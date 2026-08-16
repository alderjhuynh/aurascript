#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include "vm.h"

typedef enum {
    VAL_INT,
    VAL_DOUBLE,
    VAL_CHAR,
    VAL_STR,
    VAL_ARRAY
} ValueType;

typedef struct Value Value;

struct Value {
    ValueType type;
    long i;     /* valid when type == VAL_INT or VAL_CHAR */
    double d;   /* valid when type == VAL_DOUBLE */
    char *s;    /* owned; valid when type == VAL_STR */
    Value *items; /* owned; valid when type == VAL_ARRAY */
    int len;      /* valid when type == VAL_ARRAY */
};

typedef struct {
    Value *vars;     /* this frame's variable slots */
    int var_count;
    size_t ret_ip;   /* address to resume at when this frame returns */
} Frame;

typedef struct {
    Bytecode *bc;
    size_t ip;
    Value *stack;
    int sp;
    int stack_cap;
    Value *vars;       /* current frame's slots (== frames[depth].vars) */
    int var_count;
    Frame *frames;     /* call stack; frames[0] is the top-level frame */
    int frame_depth;
    int frame_cap;
} VM;

static void vm_error(VM *vm, const char *fmt, ...) {
    fprintf(stderr, "aurascript: VM error at pc %zu: ", vm->ip);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

/* Resolves a variable operand (top 2 bits = frame depth: 0 = current frame,
 * 1 = the top-level global frame; low 30 bits = slot index) to its slot. */
static Value *vm_slot(VM *vm, uint32_t op) {
    int depth = (int)((op >> 30) & 0x3);
    int idx = (int)(op & 0x3fffffff);
    if (depth == 1) return &vm->frames[0].vars[idx];
    return &vm->vars[idx];
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

static int32_t read_i32(const uint8_t *p) {
    return (int32_t)read_u32(p);
}

/* Reads one line of stdin into a heap-allocated NUL-terminated string with
 * the trailing newline stripped. Returns NULL on EOF or read error. */
static char *read_line(void) {
    size_t cap = 64, n = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        buf[n++] = (char)c;
    }
    if (n == 0 && c == EOF) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* Parses a whole line as an integer (strtol). Returns 1 on success. */
static int parse_long_line(const char *line, long *out) {
    errno = 0;
    char *end;
    long v = strtol(line, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (end == line || *end != '\0' || errno == ERANGE) return 0;
    *out = v;
    return 1;
}

/* Parses a whole line as a double (strtod). Returns 1 on success. */
static int parse_double_line(const char *line, double *out) {
    errno = 0;
    char *end;
    double v = strtod(line, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (end == line || *end != '\0' || errno == ERANGE) return 0;
    *out = v;
    return 1;
}

/* Reads a line of stdin into the value `dst`, converting it to `dst`'s
 * type: strings take the whole line, integers and doubles parse the whole
 * line, and a char takes the first character. */
static void read_into(VM *vm, Value *dst) {
    char *line = read_line();
    if (!line) vm_error(vm, "unexpected end of input while reading a line");
    if (dst->type == VAL_STR) {
        free(dst->s);
        dst->s = line;
    } else if (dst->type == VAL_INT) {
        long v;
        if (!parse_long_line(line, &v))
            vm_error(vm, "could not read \"%s\" as an integer", line);
        dst->i = v;
        dst->type = VAL_INT;
        free(line);
    } else if (dst->type == VAL_DOUBLE) {
        double v;
        if (!parse_double_line(line, &v))
            vm_error(vm, "could not read \"%s\" as a double", line);
        dst->d = v;
        dst->type = VAL_DOUBLE;
        free(line);
    } else { /* VAL_CHAR */
        dst->i = (unsigned char)line[0];
        dst->type = VAL_CHAR;
        free(line);
    }
}

/* Sleeps for the given number of seconds (or milliseconds, when is_ms) using
 * nanosleep for sub-second precision. Negative delays are a runtime error. */
static void vm_sleep(VM *vm, double delay, int is_ms) {
    double seconds = is_ms ? delay / 1000.0 : delay;
    if (seconds < 0.0)
        vm_error(vm, "cannot wait for a negative amount of time");
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

static double read_f64(const uint8_t *p) {
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | p[i];
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static double value_as_double(const Value *v) {
    switch (v->type) {
        case VAL_INT:
        case VAL_CHAR: return (double)v->i;
        case VAL_DOUBLE: return v->d;
        default: return 0.0;
    }
}

static long value_as_long(const Value *v) {
    switch (v->type) {
        case VAL_INT:
        case VAL_CHAR: return v->i;
        case VAL_DOUBLE: return (long)v->d;
        default: return 0;
    }
}

static const char *value_type_name(const Value *v) {
    switch (v->type) {
        case VAL_INT: return "integer";
        case VAL_DOUBLE: return "double";
        case VAL_CHAR: return "character";
        case VAL_STR: return "string";
        case VAL_ARRAY: return "array";
    }
    return "?";
}

static void value_free(Value *v) {
    if (v->type == VAL_STR) free(v->s);
    else if (v->type == VAL_ARRAY) {
        for (int i = 0; i < v->len; i++) value_free(&v->items[i]);
        free(v->items);
        v->items = NULL;
        v->len = 0;
    }
}

/* Renders any value as a freshly-allocated string (numbers become their
 * decimal text, a char becomes a one-character string, strings are copied). */
static char *value_to_str(const Value *v) {
    char buf[64];
    if (v->type == VAL_STR) return strdup(v->s);
    if (v->type == VAL_INT) {
        snprintf(buf, sizeof buf, "%ld", v->i);
        return strdup(buf);
    }
    if (v->type == VAL_DOUBLE) {
        snprintf(buf, sizeof buf, "%g", v->d);
        return strdup(buf);
    }
    buf[0] = (char)v->i;
    buf[1] = '\0';
    return strdup(buf);
}

/* The initial value for an array element of the given declared type. */
static Value default_value(VarType type) {
    Value v = {0};
    switch (type) {
        case VARTYPE_INTEGER: v.type = VAL_INT; v.i = 0; break;
        case VARTYPE_DOUBLE: v.type = VAL_DOUBLE; v.d = 0.0; break;
        case VARTYPE_CHAR: v.type = VAL_CHAR; v.i = 0; break;
        case VARTYPE_STRING: v.type = VAL_STR; v.s = strdup(""); break;
    }
    return v;
}

/* Stores `v` into `dst`, a slot (or array element) of the declared type
 * `type`, applying the language's conversion rules, then frees `v`. */
static void store_value(VarType type, Value *dst, Value *v, VM *vm) {
    switch (type) {
        case VARTYPE_INTEGER:
            if (v->type != VAL_INT && v->type != VAL_CHAR && v->type != VAL_DOUBLE)
                vm_error(vm, "cannot store a %s as an integer", value_type_name(v));
            dst->i = value_as_long(v);
            dst->type = VAL_INT;
            value_free(v);
            break;
        case VARTYPE_DOUBLE:
            if (v->type == VAL_STR)
                vm_error(vm, "cannot store a string as a double");
            dst->d = value_as_double(v);
            dst->type = VAL_DOUBLE;
            value_free(v);
            break;
        case VARTYPE_CHAR:
            if (v->type == VAL_STR) {
                if (strlen(v->s) != 1)
                    vm_error(vm, "cannot store a multi-character string in a character");
                dst->i = (unsigned char)v->s[0];
                value_free(v);
            } else if (v->type == VAL_DOUBLE) {
                vm_error(vm, "cannot store a double as a character");
            } else {
                dst->i = v->i;
                value_free(v);
            }
            dst->type = VAL_CHAR;
            break;
        case VARTYPE_STRING: {
            char *text = value_to_str(v);
            value_free(v);
            free(dst->s);
            dst->s = text;
            dst->type = VAL_STR;
            break;
        }
    }
}

/* Rejects an index that falls outside the array. */
static void check_index(VM *vm, long idx, int len) {
    if (idx < 0 || idx >= len)
        vm_error(vm, "index %ld out of bounds (array size %d)", idx, len);
}

static void push(VM *vm, Value v) {
    if (vm->sp >= vm->stack_cap) {
        vm->stack_cap = vm->stack_cap == 0 ? 32 : vm->stack_cap * 2;
        vm->stack = realloc(vm->stack, vm->stack_cap * sizeof(Value));
    }
    vm->stack[vm->sp++] = v;
}

static Value pop(VM *vm) {
    if (vm->sp <= 0) vm_error(vm, "stack underflow");
    return vm->stack[--vm->sp];
}

/* Pushes a deep copy of `v` onto the stack. Arrays are never pushed. */
static void push_copy(VM *vm, const Value *v) {
    switch (v->type) {
        case VAL_STR:
            push(vm, (Value){VAL_STR, 0, 0.0, strdup(v->s), NULL, 0});
            break;
        case VAL_DOUBLE:
            push(vm, (Value){VAL_DOUBLE, 0, v->d, NULL, NULL, 0});
            break;
        case VAL_CHAR:
            push(vm, (Value){VAL_CHAR, v->i, 0.0, NULL, NULL, 0});
            break;
        case VAL_ARRAY:
            vm_error(vm, "cannot use an array variable as a value; index it first");
            break;
        default:
            push(vm, (Value){VAL_INT, v->i, 0.0, NULL, NULL, 0});
            break;
    }
}

/* Allocates a new frame with `var_count` slots, initialised to the default
 * value for each declared type, and makes it the current frame. */
static Frame *vm_push_frame(VM *vm, const VarInfo *vtypes, int var_count) {
    if (vm->frame_depth >= vm->frame_cap) {
        vm->frame_cap = vm->frame_cap == 0 ? 8 : vm->frame_cap * 2;
        vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(Frame));
    }
    Frame *f = &vm->frames[vm->frame_depth++];
    f->var_count = var_count;
    f->vars = calloc((size_t)var_count, sizeof(Value));
    for (int i = 0; i < var_count; i++) {
        if (vtypes[i].is_array) {
            f->vars[i].type = VAL_ARRAY;
            f->vars[i].items = NULL;
            f->vars[i].len = 0;
            continue;
        }
        switch (vtypes[i].type) {
            case VARTYPE_INTEGER:
                f->vars[i].type = VAL_INT;
                f->vars[i].i = 0;
                break;
            case VARTYPE_DOUBLE:
                f->vars[i].type = VAL_DOUBLE;
                f->vars[i].d = 0.0;
                break;
            case VARTYPE_CHAR:
                f->vars[i].type = VAL_CHAR;
                f->vars[i].i = 0;
                break;
            case VARTYPE_STRING:
                f->vars[i].type = VAL_STR;
                f->vars[i].s = strdup("");
                break;
        }
    }
    vm->vars = f->vars;
    vm->var_count = var_count;
    return f;
}

/* Pops the current frame, freeing its slots, and resumes the caller's. */
static void vm_pop_frame(VM *vm) {
    if (vm->frame_depth <= 1) vm_error(vm, "return outside of a function");
    Frame *f = &vm->frames[vm->frame_depth - 1];
    for (int i = 0; i < f->var_count; i++) value_free(&f->vars[i]);
    free(f->vars);
    vm->frame_depth--;
    Frame *prev = &vm->frames[vm->frame_depth - 1];
    vm->vars = prev->vars;
    vm->var_count = prev->var_count;
}

/* Stores `v` into the current frame's slot `slot` of type `type`, applying
 * the same conversion rules as the store opcodes, then frees `v`. */
static void store_param(VM *vm, VarType type, int slot, Value *v) {
    Value *dst = &vm->vars[slot];
    switch (type) {
        case VARTYPE_INTEGER:
            if (v->type != VAL_INT && v->type != VAL_CHAR && v->type != VAL_DOUBLE)
                vm_error(vm, "cannot pass a %s as an integer parameter", value_type_name(v));
            dst->i = value_as_long(v);
            dst->type = VAL_INT;
            value_free(v);
            break;
        case VARTYPE_DOUBLE:
            if (v->type == VAL_STR)
                vm_error(vm, "cannot pass a string as a double parameter");
            dst->d = value_as_double(v);
            dst->type = VAL_DOUBLE;
            value_free(v);
            break;
        case VARTYPE_CHAR:
            if (v->type == VAL_STR) {
                if (strlen(v->s) != 1)
                    vm_error(vm, "cannot pass a multi-character string as a char parameter");
                dst->i = (unsigned char)v->s[0];
                value_free(v);
            } else if (v->type == VAL_DOUBLE) {
                vm_error(vm, "cannot pass a double as a char parameter");
            } else {
                dst->i = v->i;
                value_free(v);
            }
            dst->type = VAL_CHAR;
            break;
        case VARTYPE_STRING: {
            char *text = value_to_str(v);
            value_free(v);
            free(dst->s);
            dst->s = text;
            dst->type = VAL_STR;
            break;
        }
    }
}

static void vars_init(VM *vm) {
    Bytecode *bc = vm->bc;
    Frame *f = vm_push_frame(vm, bc->vars, bc->var_count);
    f->ret_ip = 0;
}

/* Performs a relative jump from the current ip (signed offset). */
static void vm_jump(VM *vm, int32_t rel) {
    vm->ip = (size_t)((int64_t)vm->ip + rel);
}

void vm_run(Bytecode *bc) {
    VM vm = {0};
    vm.bc = bc;
    vars_init(&vm);

    const uint8_t *code = bc->code;

    for (;;) {
        OpCode op = (OpCode)code[vm.ip];
        vm.ip++;

        switch (op) {
            case BC_HALT:
                goto done;

            case BC_PUSH_NUM: {
                int64_t v = read_i64(code + vm.ip);
                vm.ip += 8;
                push(&vm, (Value){VAL_INT, (long)v, 0.0, NULL, NULL, 0});
                break;
            }
            case BC_PUSH_DOUBLE: {
                double v = read_f64(code + vm.ip);
                vm.ip += 8;
                push(&vm, (Value){VAL_DOUBLE, 0, v, NULL, NULL, 0});
                break;
            }
            case BC_PUSH_CHAR: {
                int64_t v = read_i64(code + vm.ip);
                vm.ip += 8;
                push(&vm, (Value){VAL_CHAR, (long)v, 0.0, NULL, NULL, 0});
                break;
            }
            case BC_PUSH_STR: {
                uint32_t ci = read_u32(code + vm.ip);
                vm.ip += 4;
                push(&vm, (Value){VAL_STR, 0, 0.0, strdup(bc->strings[ci]), NULL, 0});
                break;
            }
            case BC_LOAD_VAR: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                push_copy(&vm, vm_slot(&vm, vi));
                break;
            }
            case BC_STORE_INT: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                Value v = pop(&vm);
                store_value(VARTYPE_INTEGER, vm_slot(&vm, vi), &v, &vm);
                break;
            }
            case BC_STORE_DOUBLE: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                Value v = pop(&vm);
                store_value(VARTYPE_DOUBLE, vm_slot(&vm, vi), &v, &vm);
                break;
            }
            case BC_STORE_CHAR: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                Value v = pop(&vm);
                store_value(VARTYPE_CHAR, vm_slot(&vm, vi), &v, &vm);
                break;
            }
            case BC_STORE_STR: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                Value v = pop(&vm);
                store_value(VARTYPE_STRING, vm_slot(&vm, vi), &v, &vm);
                break;
            }
            case BC_MAKE_ARRAY: {
                uint32_t vi = read_u32(code + vm.ip);
                uint32_t et = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value sz = pop(&vm);
                long n = value_as_long(&sz);
                value_free(&sz);
                if (n < 0) vm_error(&vm, "cannot create an array with a negative size");
                Value *slot = vm_slot(&vm, vi);
                value_free(slot);
                Value *items = calloc((size_t)n, sizeof(Value));
                for (long i = 0; i < n; i++) items[i] = default_value((VarType)et);
                slot->type = VAL_ARRAY;
                slot->items = items;
                slot->len = (int)n;
                break;
            }
            case BC_LOAD_INDEX: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                Value idx = pop(&vm);
                long i = value_as_long(&idx);
                value_free(&idx);
                Value *slot = vm_slot(&vm, vi);
                if (slot->type != VAL_ARRAY)
                    vm_error(&vm, "variable is not an array");
                check_index(&vm, i, slot->len);
                push_copy(&vm, &slot->items[i]);
                break;
            }
            case BC_STORE_INDEX: {
                uint32_t vi = read_u32(code + vm.ip);
                uint32_t et = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value idx = pop(&vm);
                long i = value_as_long(&idx);
                value_free(&idx);
                Value *slot = vm_slot(&vm, vi);
                if (slot->type != VAL_ARRAY)
                    vm_error(&vm, "variable is not an array");
                check_index(&vm, i, slot->len);
                Value val = pop(&vm);
                store_value((VarType)et, &slot->items[i], &val, &vm);
                break;
            }
            case BC_READ_INDEX: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                Value idx = pop(&vm);
                long i = value_as_long(&idx);
                value_free(&idx);
                Value *slot = vm_slot(&vm, vi);
                if (slot->type != VAL_ARRAY)
                    vm_error(&vm, "variable is not an array");
                check_index(&vm, i, slot->len);
                read_into(&vm, &slot->items[i]);
                break;
            }
            case BC_ADD: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                dst->i = value_as_long(dst) + value_as_long(src);
                break;
            }
            case BC_SUB: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                dst->i = value_as_long(dst) - value_as_long(src);
                break;
            }
            case BC_MUL: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                dst->i = value_as_long(dst) * value_as_long(src);
                break;
            }
            case BC_DIV: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                long divisor = value_as_long(src);
                if (divisor == 0)
                    vm_error(&vm, "division by zero");
                dst->i = value_as_long(dst) / divisor;
                break;
            }
            case BC_MOD: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                long divisor = value_as_long(src);
                if (divisor == 0)
                    vm_error(&vm, "modulo by zero");
                dst->i = value_as_long(dst) % divisor;
                break;
            }
            case BC_ADD_DOUBLE: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                dst->d = value_as_double(dst) + value_as_double(src);
                dst->type = VAL_DOUBLE;
                break;
            }
            case BC_SUB_DOUBLE: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                dst->d = value_as_double(dst) - value_as_double(src);
                dst->type = VAL_DOUBLE;
                break;
            }
            case BC_MUL_DOUBLE: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                dst->d = value_as_double(dst) * value_as_double(src);
                dst->type = VAL_DOUBLE;
                break;
            }
            case BC_DIV_DOUBLE: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                double divisor = value_as_double(src);
                if (divisor == 0.0)
                    vm_error(&vm, "division by zero");
                dst->d = value_as_double(dst) / divisor;
                dst->type = VAL_DOUBLE;
                break;
            }
            case BC_MOD_DOUBLE: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                double divisor = value_as_double(src);
                if (divisor == 0.0)
                    vm_error(&vm, "modulo by zero");
                dst->d = fmod(value_as_double(dst), divisor);
                dst->type = VAL_DOUBLE;
                break;
            }
            case BC_CONCAT: {
                uint32_t d = read_u32(code + vm.ip);
                uint32_t s = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                Value *dst = vm_slot(&vm, d);
                Value *src = vm_slot(&vm, s);
                size_t dl = strlen(dst->s);
                size_t sl = strlen(src->s);
                char *joined = malloc(dl + sl + 1);
                memcpy(joined, dst->s, dl);
                memcpy(joined + dl, src->s, sl + 1);
                free(dst->s);
                dst->s = joined;
                break;
            }
            case BC_PRINT_VAR: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                Value v = *vm_slot(&vm, vi);
                switch (v.type) {
                    case VAL_INT: printf("%ld", v.i); break;
                    case VAL_DOUBLE: printf("%g", v.d); break;
                    case VAL_CHAR: printf("%c", (char)v.i); break;
                    case VAL_STR: printf("%s", v.s); break;
                    case VAL_ARRAY: break;
                }
                break;
            }
            case BC_PRINT_STR: {
                uint32_t ci = read_u32(code + vm.ip);
                vm.ip += 4;
                printf("%s", bc->strings[ci]);
                break;
            }
            case BC_PRINT_CHAR: {
                int64_t ch = read_i64(code + vm.ip);
                vm.ip += 8;
                printf("%c", (char)ch);
                break;
            }
            case BC_PRINT_NL: {
                printf("\n");
                break;
            }
            case BC_READ_LINE: {
                uint32_t vi = read_u32(code + vm.ip);
                vm.ip += 4;
                read_into(&vm, vm_slot(&vm, vi));
                break;
            }
            case BC_SLEEP: {
                uint32_t unit = read_u32(code + vm.ip);
                vm.ip += 4;
                Value v = pop(&vm);
                double delay = value_as_double(&v);
                value_free(&v);
                vm_sleep(&vm, delay, unit != 0);
                break;
            }
            case BC_PRINT_STK: {
                Value v = pop(&vm);
                switch (v.type) {
                    case VAL_INT: printf("%ld", v.i); break;
                    case VAL_DOUBLE: printf("%g", v.d); break;
                    case VAL_CHAR: printf("%c", (char)v.i); break;
                    case VAL_STR: printf("%s", v.s); break;
                    case VAL_ARRAY: break;
                }
                value_free(&v);
                break;
            }
            case BC_ADD_STK:
            case BC_SUB_STK:
            case BC_MUL_STK:
            case BC_DIV_STK:
            case BC_MOD_STK: {
                Value right = pop(&vm);
                Value left = pop(&vm);
                Value res = {0};
                if (op == BC_ADD_STK && left.type == VAL_STR && right.type == VAL_STR) {
                    size_t ll = strlen(left.s);
                    size_t rl = strlen(right.s);
                    char *joined = malloc(ll + rl + 1);
                    memcpy(joined, left.s, ll);
                    memcpy(joined + ll, right.s, rl + 1);
                    res.type = VAL_STR;
                    res.s = joined;
                } else {
                    if (left.type == VAL_STR || right.type == VAL_STR)
                        vm_error(&vm, "cannot use a string with an arithmetic operator");
                    if (left.type == VAL_DOUBLE || right.type == VAL_DOUBLE) {
                        double a = value_as_double(&left);
                        double b = value_as_double(&right);
                        if ((op == BC_DIV_STK || op == BC_MOD_STK) && b == 0.0)
                            vm_error(&vm, "%s by zero", op == BC_MOD_STK ? "modulo" : "division");
                        res.type = VAL_DOUBLE;
                        if (op == BC_ADD_STK) res.d = a + b;
                        else if (op == BC_SUB_STK) res.d = a - b;
                        else if (op == BC_MUL_STK) res.d = a * b;
                        else if (op == BC_DIV_STK) res.d = a / b;
                        else res.d = fmod(a, b);
                    } else {
                        long a = value_as_long(&left);
                        long b = value_as_long(&right);
                        if ((op == BC_DIV_STK || op == BC_MOD_STK) && b == 0)
                            vm_error(&vm, "%s by zero", op == BC_MOD_STK ? "modulo" : "division");
                        res.type = VAL_INT;
                        if (op == BC_ADD_STK) res.i = a + b;
                        else if (op == BC_SUB_STK) res.i = a - b;
                        else if (op == BC_MUL_STK) res.i = a * b;
                        else if (op == BC_DIV_STK) res.i = a / b;
                        else res.i = a % b;
                    }
                }
                value_free(&left);
                value_free(&right);
                push(&vm, res);
                break;
            }
            case BC_MATH_SIN:
            case BC_MATH_COS:
            case BC_MATH_SQRT: {
                Value v = pop(&vm);
                double x = value_as_double(&v);
                value_free(&v);
                double res;
                if (op == BC_MATH_SIN) res = sin(x);
                else if (op == BC_MATH_COS) res = cos(x);
                else {
                    if (x < 0.0) vm_error(&vm, "cannot take the square root of a negative number");
                    res = sqrt(x);
                }
                push(&vm, (Value){VAL_DOUBLE, 0, res, NULL, NULL, 0});
                break;
            }
            case BC_CMP_LT:
            case BC_CMP_GT:
            case BC_CMP_EQ:
            case BC_CMP_LE:
            case BC_CMP_GE:
            case BC_CMP_NE: {
                Value right = pop(&vm);
                Value left = pop(&vm);
                int res = 0;
                if (left.type == VAL_STR && right.type == VAL_STR) {
                    int c = strcmp(left.s, right.s);
                    if (op == BC_CMP_LT) res = c < 0;
                    else if (op == BC_CMP_GT) res = c > 0;
                    else if (op == BC_CMP_LE) res = c <= 0;
                    else if (op == BC_CMP_GE) res = c >= 0;
                    else if (op == BC_CMP_NE) res = c != 0;
                    else res = c == 0;
                } else if (left.type != VAL_STR && right.type != VAL_STR) {
                    double a = value_as_double(&left);
                    double b = value_as_double(&right);
                    if (op == BC_CMP_LT) res = a < b;
                    else if (op == BC_CMP_GT) res = a > b;
                    else if (op == BC_CMP_LE) res = a <= b;
                    else if (op == BC_CMP_GE) res = a >= b;
                    else if (op == BC_CMP_NE) res = a != b;
                    else res = a == b;
                } else {
                    vm_error(&vm, "cannot compare a string with a number");
                }
                value_free(&left);
                value_free(&right);
                push(&vm, (Value){VAL_INT, res, 0.0, NULL, NULL, 0});
                break;
            }
            case BC_AND:
            case BC_OR: {
                Value right = pop(&vm);
                Value left = pop(&vm);
                int a = left.i != 0;
                int b = right.i != 0;
                int res = op == BC_AND ? (a && b) : (a || b);
                value_free(&left);
                value_free(&right);
                push(&vm, (Value){VAL_INT, res, 0.0, NULL, NULL, 0});
                break;
            }
            case BC_JMP: {
                int32_t rel = read_i32(code + vm.ip);
                vm.ip += 4;
                vm_jump(&vm, rel);
                break;
            }
            case BC_JMP_IF_FALSE: {
                int32_t rel = read_i32(code + vm.ip);
                vm.ip += 4;
                Value v = pop(&vm);
                int cond = v.i != 0;
                value_free(&v);
                if (!cond) vm_jump(&vm, rel);
                break;
            }
            case BC_CALL: {
                uint32_t fi = read_u32(code + vm.ip);
                uint32_t argc = read_u32(code + vm.ip + 4);
                vm.ip += 8;
                FuncInfo *f = &bc->funcs[fi];
                if (argc != (uint32_t)f->param_count)
                    vm_error(&vm, "function '%s' expects %d argument(s), got %u",
                             f->name, f->param_count, argc);
                /* Pop the arguments (pushed left to right) into a temp
                 * array, then install a new frame whose parameter slots
                 * receive them in order. */
                Value *args = calloc(argc, sizeof(Value));
                for (uint32_t i = argc; i > 0; i--) args[i - 1] = pop(&vm);
                Frame *fr = vm_push_frame(&vm, f->vars, f->var_count);
                fr->ret_ip = vm.ip;
                for (uint32_t i = 0; i < argc; i++)
                    store_param(&vm, f->vars[i].type, (int)i, &args[i]);
                free(args);
                vm.ip = f->entry;
                break;
            }
            case BC_RET: {
                Value ret = pop(&vm);
                size_t ret_ip = vm.frames[vm.frame_depth - 1].ret_ip;
                vm_pop_frame(&vm);
                vm.ip = ret_ip;
                push(&vm, ret);
                break;
            }
            case BC_POP: {
                Value v = pop(&vm);
                value_free(&v);
                break;
            }
            default:
                vm_error(&vm, "unknown opcode %d", (int)op);
        }
    }

done:
    /* The top-level frame is still on the stack; release its slots. */
    for (int i = 0; i < vm.frame_depth; i++) {
        Frame *f = &vm.frames[i];
        for (int v = 0; v < f->var_count; v++) value_free(&f->vars[v]);
        free(f->vars);
    }
    free(vm.frames);
    for (int i = 0; i < vm.sp; i++) value_free(&vm.stack[i]);
    free(vm.stack);
}