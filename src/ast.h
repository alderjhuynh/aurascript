#ifndef AST_H
#define AST_H

typedef enum {
    VARTYPE_INTEGER,
    VARTYPE_DOUBLE,
    VARTYPE_CHAR,
    VARTYPE_STRING
} VarType;

typedef enum {
    EXPR_NUMBER,   /* integer literal */
    EXPR_DOUBLE,   /* floating-point literal */
    EXPR_STRING,   /* string literal */
    EXPR_CHAR,     /* character literal */
    EXPR_PI,       /* the constant pi */
    EXPR_IDENT,
    EXPR_INDEXED,  /* the variable NAME at the index EXPR */
    EXPR_CALL      /* call the function called NAME with the value of ... */
} ExprKind;

typedef struct ExprList ExprList;
typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    long number;      /* valid when kind == EXPR_NUMBER */
    double dnumber;   /* valid when kind == EXPR_DOUBLE */
    char *str;        /* valid when kind == EXPR_STRING */
    int ch;           /* valid when kind == EXPR_CHAR */
    char *ident;      /* valid when kind == EXPR_IDENT or EXPR_INDEXED */
    Expr *index;      /* valid when kind == EXPR_INDEXED */
    char *call_name;  /* valid when kind == EXPR_CALL */
    ExprList *call_args; /* valid when kind == EXPR_CALL */
};

typedef enum {
    COND_LESS_THAN,
    COND_LESS_EQUAL,
    COND_GREATER_THAN,
    COND_GREATER_EQUAL,
    COND_EQUAL,
    COND_NOT_EQUAL
} CondOp;

typedef enum {
    COND_CMP,   /* a single comparison: <left_ident> <op> <right> */
    COND_AND,   /* logical and of two conditions */
    COND_OR     /* logical or of two conditions */
} CondKind;

typedef struct Condition {
    CondKind kind;
    CondOp op;            /* valid when kind == COND_CMP */
    char *left_ident;     /* valid when kind == COND_CMP */
    Expr *left_index;     /* valid when kind == COND_CMP; index expr when the left side is an array element, else NULL */
    Expr *right;          /* valid when kind == COND_CMP */
    struct Condition *left;   /* valid when kind == COND_AND or COND_OR */
    struct Condition *right_cond; /* valid when kind == COND_AND or COND_OR */
} Condition;

typedef enum {
    ARITH_ADD,
    ARITH_SUB,
    ARITH_MUL,
    ARITH_DIV,
    ARITH_MOD
} ArithOp;

typedef enum {
    STMT_DECL,
    STMT_ASSIGN,
    STMT_ARITH,
    STMT_WRITE,
    STMT_READ,
    STMT_WAIT,
    STMT_WHILE,
    STMT_WHEN,
    STMT_FUNC_DEF,
    STMT_RETURN,
    STMT_CALL
} StmtKind;

typedef enum {
    WRITE_VAR,      /* write out the variable <var_name> */
    WRITE_STRING,   /* write out the string "<str>" */
    WRITE_CHAR      /* write out the char '<ch>' */
} WriteKind;

typedef struct {
    WriteKind kind;
    char *var_name;   /* valid when kind == WRITE_VAR */
    Expr *index;      /* valid when kind == WRITE_VAR; index expr when the item is an array element, else NULL */
    char *str;        /* valid when kind == WRITE_STRING */
    int ch;           /* valid when kind == WRITE_CHAR */
} WriteItem;

typedef struct {
    WriteItem *items;
    int count;
    int capacity;
} WriteList;

typedef struct Stmt Stmt;

typedef struct {
    Stmt **items;
    int count;
    int capacity;
} StmtList;

/* Function parameters: one named, typed slot per declaration. */
typedef struct {
    char *name;
    VarType type;
} Param;

typedef struct {
    Param *items;
    int count;
    int capacity;
} ParamList;

/* A list of argument expressions, as in a call statement or expression. */
struct ExprList {
    Expr **items;
    int count;
    int capacity;
};

struct Stmt {
    StmtKind kind;
    int line;

    /* STMT_DECL: create a variable called <decl_name> as a/an <decl_type>
     * [with a value of <decl_init>], or as an array of <decl_type>
     * with a size of <decl_size> (decl_is_array set). */
    char *decl_name;
    VarType decl_type;
    Expr *decl_init;        /* may be NULL */
    int decl_is_array;      /* true for array declarations */
    Expr *decl_size;        /* valid when decl_is_array */

    /* STMT_ASSIGN: assign the variable called <assign_name> [at the index
     * <assign_index>] the value of <assign_value>. */
    char *assign_name;
    Expr *assign_index;     /* may be NULL */
    Expr *assign_value;

    /* STMT_ARITH: <op> the variable <arith_src> [at the index
     * <arith_src_index>] to/from/by the variable <arith_dest> [at the index
     * <arith_dest_index>]. (arith_dest <op>= arith_src; ADD also allows
     * strings, which means concatenation.) */
    ArithOp arith_op;
    char *arith_dest;
    Expr *arith_dest_index; /* may be NULL */
    char *arith_src;
    Expr *arith_src_index;  /* may be NULL */

    /* STMT_WRITE: write out the <write_items[0]> and the <write_items[1]> ...
     * The items print adjacent on one line, ending with a newline. A lone
     * item is the familiar `write out the variable <x>`. */
    WriteList *write_items;

    /* STMT_READ: read the input into the variable called <read_name> [at the
     * index <read_index>]. A line is read from stdin and converted to the
     * variable's (or element's) type. */
    char *read_name;
    Expr *read_index;       /* may be NULL */

    /* STMT_WAIT: wait for <wait_time> <wait_is_ms ? "milliseconds" : "seconds">
     * The value may be a literal or a variable. */
    Expr *wait_time;
    int wait_is_ms;

    /* STMT_WHILE: repeat while <while_cond>: <while_body> stop repeating. */
    Condition *while_cond;
    StmtList *while_body;

    /* STMT_WHEN: when <when_cond> <when_body> [otherwise <when_else>]
     * Bodies are statement lists; the single-statement form is a list of
     * one, and the block form (`when <cond>: ... stop when.`) can hold many. */
    Condition *when_cond;
    StmtList *when_body;
    StmtList *when_else;    /* may be NULL */

    /* STMT_FUNC_DEF: create a function called <func_name> [with a parameter
     * called NAME as a/an TYPE (and a parameter ...)*]: <func_body>
     * stop the function. */
    char *func_name;
    ParamList *params;
    StmtList *func_body;

    /* STMT_RETURN: return [the value of <ret_value>]. Leaves the function,
     * pushing <ret_value> as the result (0 when omitted). */
    Expr *ret_value;        /* may be NULL */

    /* STMT_CALL: call the function called <call_name> [with the value of
     * <call_args[0]> (and the value of ...)*]. Invokes the function and
     * discards its result. */
    char *call_name;
    ExprList *call_args;
};

void stmtlist_init(StmtList *list);
void stmtlist_push(StmtList *list, Stmt *stmt);

void writelist_init(WriteList *list);
void writelist_push(WriteList *list, WriteItem item);

void paramlist_init(ParamList *list);
void paramlist_push(ParamList *list, Param param);

void exprlist_init(ExprList *list);
void exprlist_push(ExprList *list, Expr *expr);

Stmt *ast_new_stmt(StmtKind kind, int line);
WriteItem ast_new_write_var(const char *name);
WriteItem ast_new_write_string(const char *str);
WriteItem ast_new_write_char(int ch);
Expr *ast_new_number(long value);
Expr *ast_new_double(double value);
Expr *ast_new_pi(void);
Expr *ast_new_string(const char *value);
Expr *ast_new_char(int ch);
Expr *ast_new_ident(const char *name);
Expr *ast_new_indexed(const char *name, Expr *index);
Expr *ast_new_call(const char *name, ExprList *args);
Condition *ast_new_condition(const char *left_ident, CondOp op, Expr *right);
Condition *ast_new_logical(CondKind kind, Condition *left, Condition *right);

void ast_print_program(StmtList *program);

#endif
