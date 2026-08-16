#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

void parser_init(Parser *p, Token *tokens, int count) {
    p->tokens = tokens;
    p->count = count;
    p->pos = 0;
}

static Token *cur(Parser *p) {
    /* tokens[] always has a trailing TOK_EOF sentinel at index `count`,
     * so it's safe to read one past the last "real" token. */
    if (p->pos >= p->count) return &p->tokens[p->count];
    return &p->tokens[p->pos];
}

static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (p->pos < p->count) p->pos++;
    return t;
}

static int check(Parser *p, TokenType type) {
    return cur(p)->type == type;
}

static void parser_error(Parser *p, const char *msg) {
    Token *t = cur(p);
    fprintf(stderr, "aurascript: parse error at line %d: %s (got %s%s%s)\n",
            t->line, msg, token_type_name(t->type),
            t->text ? " '" : "", t->text ? t->text : "");
    if (t->text) fprintf(stderr, "'\n");
    exit(1);
}

static Token *expect(Parser *p, TokenType type, const char *msg) {
    if (!check(p, type)) parser_error(p, msg);
    return advance(p);
}

/* An article ("a"/"an") -- we don't care which, English grammar isn't
 * something we're in the business of validating. */
static void expect_article(Parser *p) {
    if (check(p, TOK_A) || check(p, TOK_AN)) {
        advance(p);
    } else {
        parser_error(p, "expected 'a' or 'an'");
    }
}

/* Statement terminators are inconsistent in practice (see examples/basic.ausc,
 * where some lines end in '.' and some don't), so we treat the trailing dot
 * as optional rather than mandatory. */
static void skip_optional_dot(Parser *p) {
    if (check(p, TOK_DOT)) advance(p);
}

static int is_double_text(const char *s) {
    return strchr(s, '.') != NULL || strchr(s, 'e') != NULL || strchr(s, 'E') != NULL;
}

static ExprList *parse_call_args(Parser *p);
static Stmt *parse_stmt(Parser *p);
static Expr *parse_optional_index(Parser *p);

static Expr *parse_expr(Parser *p) {
    if (check(p, TOK_NUMBER)) {
        Token *t = advance(p);
        if (is_double_text(t->text)) return ast_new_double(strtod(t->text, NULL));
        return ast_new_number(strtol(t->text, NULL, 10));
    }
    if (check(p, TOK_STRING)) {
        Token *t = advance(p);
        return ast_new_string(t->text);
    }
    if (check(p, TOK_CHAR_LIT)) {
        Token *t = advance(p);
        return ast_new_char(t->ch);
    }
    if (check(p, TOK_PI)) {
        advance(p);
        return ast_new_pi();
    }
    if (check(p, TOK_IDENT)) {
        Token *t = advance(p);
        return ast_new_ident(t->text);
    }
    if (check(p, TOK_THE)) {
        advance(p);
        expect(p, TOK_VARIABLE, "expected 'variable' after 'the'");
        Token *name = expect(p, TOK_IDENT, "expected variable name");
        Expr *idx = parse_optional_index(p);
        if (idx) return ast_new_indexed(name->text, idx);
        return ast_new_ident(name->text);
    }
    if (check(p, TOK_CALL)) {
        advance(p);
        expect(p, TOK_THE, "expected 'the'");
        expect(p, TOK_FUNCTION, "expected 'function'");
        expect(p, TOK_CALLED, "expected 'called'");
        Token *name = expect(p, TOK_IDENT, "expected function name");
        return ast_new_call(name->text, parse_call_args(p));
    }
    parser_error(p, "expected a number, string, character or identifier");
    return NULL; /* unreachable */
}

/* OptionalIndex := [ AT THE INDEX Expr ]
 * Attaches an element index to a variable reference, turning e.g.
 * `the variable scores at the index 2` into an indexed access. */
static Expr *parse_optional_index(Parser *p) {
    if (!check(p, TOK_AT)) return NULL;
    advance(p);
    expect(p, TOK_THE, "expected 'the' after 'at'");
    expect(p, TOK_INDEX, "expected 'index' after 'the'");
    return parse_expr(p);
}

/* CallArgs := [ WITH THE VALUE OF Expr ( AND THE VALUE OF Expr )* ]
 * Shared by the call statement and the call expression. */
static ExprList *parse_call_args(Parser *p) {
    ExprList *args = malloc(sizeof(ExprList));
    exprlist_init(args);
    if (check(p, TOK_WITH)) {
        advance(p);
        for (;;) {
            expect(p, TOK_THE, "expected 'the'");
            expect(p, TOK_VALUE, "expected 'value'");
            expect(p, TOK_OF, "expected 'of'");
            exprlist_push(args, parse_expr(p));
            if (!check(p, TOK_AND)) break;
            advance(p);
        }
    }
    return args;
}

/* Condition := OrExpr
 * OrExpr := AndExpr ( OR AndExpr )*
 * AndExpr := Comparison ( AND Comparison )*
 * Comparison := THE VARIABLE IDENT IS CmpOp Expr
 * CmpOp := LESS THAN [ OR EQUAL TO ]
 *        | GREATER THAN [ OR EQUAL TO ]
 *        | EQUAL TO
 *        | NOT EQUAL TO
 * "and" binds tighter than "or", so "a or b and c" means "a or (b and c)". */
static Condition *parse_or_expr(Parser *p);
static Condition *parse_and_expr(Parser *p);
static Condition *parse_comparison(Parser *p);

static Condition *parse_condition(Parser *p) {
    return parse_or_expr(p);
}

static Condition *parse_or_expr(Parser *p) {
    Condition *left = parse_and_expr(p);
    while (check(p, TOK_OR)) {
        advance(p);
        Condition *right = parse_and_expr(p);
        left = ast_new_logical(COND_OR, left, right);
    }
    return left;
}

static Condition *parse_and_expr(Parser *p) {
    Condition *left = parse_comparison(p);
    while (check(p, TOK_AND)) {
        advance(p);
        Condition *right = parse_comparison(p);
        left = ast_new_logical(COND_AND, left, right);
    }
    return left;
}

static Condition *parse_comparison(Parser *p) {
    expect(p, TOK_THE, "expected 'the' to start a condition");
    expect(p, TOK_VARIABLE, "expected 'variable' in condition");
    Token *name = expect(p, TOK_IDENT, "expected variable name in condition");
    Expr *idx = parse_optional_index(p);
    expect(p, TOK_IS, "expected 'is' in condition");

    CondOp op;
    if (check(p, TOK_LESS)) {
        advance(p);
        expect(p, TOK_THAN, "expected 'than' after 'less'");
        if (check(p, TOK_OR)) {
            advance(p);
            expect(p, TOK_EQUAL, "expected 'equal' after 'or'");
            expect(p, TOK_TO, "expected 'to' after 'equal'");
            op = COND_LESS_EQUAL;
        } else {
            op = COND_LESS_THAN;
        }
    } else if (check(p, TOK_GREATER)) {
        advance(p);
        expect(p, TOK_THAN, "expected 'than' after 'greater'");
        if (check(p, TOK_OR)) {
            advance(p);
            expect(p, TOK_EQUAL, "expected 'equal' after 'or'");
            expect(p, TOK_TO, "expected 'to' after 'equal'");
            op = COND_GREATER_EQUAL;
        } else {
            op = COND_GREATER_THAN;
        }
    } else if (check(p, TOK_NOT)) {
        advance(p);
        expect(p, TOK_EQUAL, "expected 'equal' after 'not'");
        expect(p, TOK_TO, "expected 'to' after 'equal'");
        op = COND_NOT_EQUAL;
    } else if (check(p, TOK_EQUAL)) {
        advance(p);
        expect(p, TOK_TO, "expected 'to' after 'equal'");
        op = COND_EQUAL;
    } else {
        parser_error(p, "expected 'less', 'greater', 'equal' or 'not equal' in condition");
        op = COND_EQUAL; /* unreachable */
    }

    Expr *right = parse_expr(p);
    Condition *cond = ast_new_condition(name->text, op, right);
    cond->left_index = idx;
    return cond;
}

/* Type := INTEGER | DOUBLE | CHAR | STRING */
static VarType parse_type(Parser *p) {
    if (check(p, TOK_INTEGER)) {
        advance(p);
        return VARTYPE_INTEGER;
    }
    if (check(p, TOK_DOUBLE)) {
        advance(p);
        return VARTYPE_DOUBLE;
    }
    if (check(p, TOK_CHAR)) {
        advance(p);
        return VARTYPE_CHAR;
    }
    if (check(p, TOK_STRING_TYPE)) {
        advance(p);
        return VARTYPE_STRING;
    }
    parser_error(p, "expected 'integer', 'double', 'char' or 'string'");
    return VARTYPE_INTEGER; /* unreachable */
}

/* FuncDefStmt := CREATE article FUNCTION CALLED IDENT
 *                [ WITH PARAMETER CALLED IDENT AS article Type
 *                  ( AND PARAMETER CALLED IDENT AS article Type )* ]
 *                COLON Stmt* STOP FUNCTION [DOT]
 * `create article` is consumed by the caller (parse_decl). */
static Stmt *parse_func_def(Parser *p, int line) {
    expect(p, TOK_FUNCTION, "expected 'function'");
    expect(p, TOK_CALLED, "expected 'called'");
    Token *name = expect(p, TOK_IDENT, "expected function name");

    Stmt *s = ast_new_stmt(STMT_FUNC_DEF, line);
    s->func_name = strdup(name->text);
    s->params = malloc(sizeof(ParamList));
    paramlist_init(s->params);

    if (check(p, TOK_WITH)) {
        advance(p);
        for (;;) {
            expect_article(p);
            expect(p, TOK_PARAMETER, "expected 'parameter'");
            expect(p, TOK_CALLED, "expected 'called'");
            Token *pname = expect(p, TOK_IDENT, "expected parameter name");
            expect(p, TOK_AS, "expected 'as'");
            expect_article(p);
            Param prm = {strdup(pname->text), parse_type(p)};
            paramlist_push(s->params, prm);
            if (!check(p, TOK_AND)) break;
            advance(p);
        }
    }

    expect(p, TOK_COLON, "expected ':' before the function body");

    StmtList *body = malloc(sizeof(StmtList));
    stmtlist_init(body);
    while (!check(p, TOK_STOP) && !check(p, TOK_EOF)) {
        stmtlist_push(body, parse_stmt(p));
    }
    expect(p, TOK_STOP, "expected 'stop' to close the function");
    if (check(p, TOK_THE)) advance(p); /* "stop the function." */
    expect(p, TOK_FUNCTION, "expected 'function' after 'stop'");
    skip_optional_dot(p);

    s->func_body = body;
    return s;
}

/* DeclStmt := CREATE article VARIABLE CALLED IDENT AS article Type
 *             [ WITH article VALUE OF Expr ] [DOT]
 *           | CREATE article VARIABLE CALLED IDENT AS article ARRAY OF Type
 *             WITH article SIZE OF Expr [DOT]
 * A `create a function ...` statement is routed to parse_func_def. */
static Stmt *parse_decl(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_CREATE, "expected 'create'");
    expect_article(p);
    if (check(p, TOK_FUNCTION)) return parse_func_def(p, line);
    expect(p, TOK_VARIABLE, "expected 'variable'");
    expect(p, TOK_CALLED, "expected 'called'");
    Token *name = expect(p, TOK_IDENT, "expected variable name");
    expect(p, TOK_AS, "expected 'as'");
    expect_article(p);

    Stmt *s = ast_new_stmt(STMT_DECL, line);
    s->decl_name = strdup(name->text);
    s->decl_init = NULL;
    s->decl_is_array = 0;
    s->decl_size = NULL;

    if (check(p, TOK_ARRAY)) {
        /* as an array of TYPE with a size of Expr */
        advance(p);
        expect(p, TOK_OF, "expected 'of' after 'array'");
        s->decl_is_array = 1;
        s->decl_type = parse_type(p);
        if (check(p, TOK_WITH)) {
            advance(p);
            expect_article(p);
            expect(p, TOK_SIZE, "expected 'size'");
            expect(p, TOK_OF, "expected 'of' after 'size'");
            s->decl_size = parse_expr(p);
        } else {
            parser_error(p, "expected 'with a size of' for an array declaration");
        }
        skip_optional_dot(p);
        return s;
    }

    s->decl_type = parse_type(p);

    if (check(p, TOK_WITH)) {
        advance(p);
        expect_article(p);
        expect(p, TOK_VALUE, "expected 'value'");
        expect(p, TOK_OF, "expected 'of'");
        s->decl_init = parse_expr(p);
    }

    skip_optional_dot(p);
    return s;
}

/* AssignStmt := ASSIGN THE VARIABLE CALLED IDENT [ AT THE INDEX Expr ]
 *               THE VALUE OF Expr [DOT] */
static Stmt *parse_assign(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_ASSIGN, "expected 'assign'");
    expect(p, TOK_THE, "expected 'the'");
    expect(p, TOK_VARIABLE, "expected 'variable'");
    expect(p, TOK_CALLED, "expected 'called'");
    Token *name = expect(p, TOK_IDENT, "expected variable name");
    Expr *idx = parse_optional_index(p);
    expect(p, TOK_THE, "expected 'the'");
    expect(p, TOK_VALUE, "expected 'value'");
    expect(p, TOK_OF, "expected 'of'");
    Expr *value = parse_expr(p);

    Stmt *s = ast_new_stmt(STMT_ASSIGN, line);
    s->assign_name = strdup(name->text);
    s->assign_index = idx;
    s->assign_value = value;

    skip_optional_dot(p);
    return s;
}

/* ArithStmt := ( ADD | SUBTRACT | MULTIPLY | DIVIDE ) THE VARIABLE IDENT
 *              [ AT THE INDEX Expr ]
 *              ( TO | FROM | BY ) THE VARIABLE IDENT [ AT THE INDEX Expr ]
 *              [DOT]
 * "add the variable x to the variable y" means y += x, i.e. the first
 * name is the source, the second is the destination. The other operators
 * follow the same shape: "subtract ... from ..." (y -= x),
 * "multiply ... by ..." (y *= x) and "divide ... by ..." (y /= x). */
static Stmt *parse_arith(Parser *p) {
    int line = cur(p)->line;

    ArithOp op;
    TokenType join;
    if (check(p, TOK_ADD)) {
        advance(p);
        op = ARITH_ADD;
        join = TOK_TO;
    } else if (check(p, TOK_SUBTRACT)) {
        advance(p);
        op = ARITH_SUB;
        join = TOK_FROM;
    } else if (check(p, TOK_MULTIPLY)) {
        advance(p);
        op = ARITH_MUL;
        join = TOK_BY;
    } else if (check(p, TOK_DIVIDE)) {
        advance(p);
        op = ARITH_DIV;
        join = TOK_BY;
    } else if (check(p, TOK_MODULO)) {
        advance(p);
        op = ARITH_MOD;
        join = TOK_BY;
    } else {
        parser_error(p, "expected an arithmetic operator");
        return NULL; /* unreachable */
    }

    expect(p, TOK_THE, "expected 'the'");
    expect(p, TOK_VARIABLE, "expected 'variable'");
    Token *src = expect(p, TOK_IDENT, "expected source variable name");
    Expr *src_idx = parse_optional_index(p);
    expect(p, join, "expected 'to', 'from' or 'by'");
    expect(p, TOK_THE, "expected 'the'");
    expect(p, TOK_VARIABLE, "expected 'variable'");
    Token *dest = expect(p, TOK_IDENT, "expected destination variable name");
    Expr *dest_idx = parse_optional_index(p);

    Stmt *s = ast_new_stmt(STMT_ARITH, line);
    s->arith_op = op;
    s->arith_src = strdup(src->text);
    s->arith_src_index = src_idx;
    s->arith_dest = strdup(dest->text);
    s->arith_dest_index = dest_idx;

    skip_optional_dot(p);
    return s;
}

/* WriteItem := THE ( VARIABLE IDENT | STRING STRING | CHAR CHAR_LIT )
 * WriteStmt := WRITE OUT WriteItem ( AND WriteItem )* [DOT]
 * Items joined with 'and' print adjacent on the same line: `write out the
 * variable x and the string "!"` prints both with a single trailing newline. */
static WriteItem parse_write_item(Parser *p) {
    expect(p, TOK_THE, "expected 'the'");
    if (check(p, TOK_VARIABLE)) {
        advance(p);
        Token *name = expect(p, TOK_IDENT, "expected variable name");
        WriteItem item = ast_new_write_var(name->text);
        item.index = parse_optional_index(p);
        return item;
    } else if (check(p, TOK_STRING_TYPE)) {
        advance(p);
        Token *str = expect(p, TOK_STRING, "expected a string literal");
        return ast_new_write_string(str->text);
    } else if (check(p, TOK_CHAR)) {
        advance(p);
        Token *clit = expect(p, TOK_CHAR_LIT, "expected a character literal");
        return ast_new_write_char(clit->ch);
    }
    parser_error(p, "expected 'variable', 'string' or 'char' after 'the'");
    return (WriteItem){0}; /* unreachable */
}

static Stmt *parse_write(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_WRITE, "expected 'write'");
    expect(p, TOK_OUT, "expected 'out'");

    Stmt *s = ast_new_stmt(STMT_WRITE, line);
    s->write_items = malloc(sizeof(WriteList));
    writelist_init(s->write_items);
    writelist_push(s->write_items, parse_write_item(p));
    while (check(p, TOK_AND)) {
        advance(p);
        writelist_push(s->write_items, parse_write_item(p));
    }

    skip_optional_dot(p);
    return s;
}

/* WhileStmt := REPEAT WHILE Condition COLON Stmt* STOP REPEATING [DOT] */
static Stmt *parse_while(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_REPEAT, "expected 'repeat'");
    expect(p, TOK_WHILE, "expected 'while'");
    Condition *cond = parse_condition(p);
    expect(p, TOK_COLON, "expected ':' after while-condition");

    StmtList *body = malloc(sizeof(StmtList));
    stmtlist_init(body);
    while (!check(p, TOK_STOP) && !check(p, TOK_EOF)) {
        stmtlist_push(body, parse_stmt(p));
    }
    expect(p, TOK_STOP, "expected 'stop' to close the loop");
    expect(p, TOK_REPEATING, "expected 'repeating' after 'stop'");
    skip_optional_dot(p);

    Stmt *s = ast_new_stmt(STMT_WHILE, line);
    s->while_cond = cond;
    s->while_body = body;
    return s;
}

/* WhenStmt := WHEN Condition Stmt [ OTHERWISE Stmt ]
 *           | WHEN Condition COLON Stmt* [ OTHERWISE COLON Stmt* ] STOP WHEN [DOT]
 * The single-statement form takes one statement (and one for the optional
 * else); the block form mirrors `repeat while ... stop repeating.` */
static Stmt *parse_when(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_WHEN, "expected 'when'");
    Condition *cond = parse_condition(p);

    Stmt *s = ast_new_stmt(STMT_WHEN, line);
    s->when_cond = cond;
    s->when_body = malloc(sizeof(StmtList));
    stmtlist_init(s->when_body);
    s->when_else = NULL;

    if (check(p, TOK_COLON)) {
        /* Block form: when <cond>: <body> [otherwise: <else>] stop when. */
        advance(p);
        while (!check(p, TOK_OTHERWISE) && !check(p, TOK_STOP) && !check(p, TOK_EOF)) {
            stmtlist_push(s->when_body, parse_stmt(p));
        }
        if (check(p, TOK_OTHERWISE)) {
            advance(p);
            expect(p, TOK_COLON, "expected ':' after 'otherwise'");
            s->when_else = malloc(sizeof(StmtList));
            stmtlist_init(s->when_else);
            while (!check(p, TOK_STOP) && !check(p, TOK_EOF)) {
                stmtlist_push(s->when_else, parse_stmt(p));
            }
        }
        expect(p, TOK_STOP, "expected 'stop' to close the when");
        expect(p, TOK_WHEN, "expected 'when' after 'stop'");
        skip_optional_dot(p);
    } else {
        /* Single-statement form: when <cond> <stmt> [otherwise <stmt>] */
        stmtlist_push(s->when_body, parse_stmt(p));
        if (check(p, TOK_OTHERWISE)) {
            advance(p);
            s->when_else = malloc(sizeof(StmtList));
            stmtlist_init(s->when_else);
            stmtlist_push(s->when_else, parse_stmt(p));
        }
    }
    return s;
}

/* ReadStmt := READ THE INPUT INTO THE VARIABLE CALLED IDENT
 *             [ AT THE INDEX Expr ] [DOT]
 * Reads one line from stdin into the named variable (or array element). */
static Stmt *parse_read(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_READ, "expected 'read'");
    expect(p, TOK_THE, "expected 'the'");
    expect(p, TOK_INPUT, "expected 'input'");
    expect(p, TOK_INTO, "expected 'into'");
    expect(p, TOK_THE, "expected 'the'");
    expect(p, TOK_VARIABLE, "expected 'variable'");
    expect(p, TOK_CALLED, "expected 'called'");
    Token *name = expect(p, TOK_IDENT, "expected variable name");
    Expr *idx = parse_optional_index(p);

    Stmt *s = ast_new_stmt(STMT_READ, line);
    s->read_name = strdup(name->text);
    s->read_index = idx;

    skip_optional_dot(p);
    return s;
}

/* WaitStmt := WAIT FOR Expr ( SECONDS | MILLISECONDS ) [DOT]
 * The amount may be a literal or a variable holding the delay. */
static Stmt *parse_wait(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_WAIT, "expected 'wait'");
    expect(p, TOK_FOR, "expected 'for'");
    Expr *time = parse_expr(p);

    Stmt *s = ast_new_stmt(STMT_WAIT, line);
    s->wait_time = time;
    if (check(p, TOK_SECONDS)) {
        advance(p);
        s->wait_is_ms = 0;
    } else if (check(p, TOK_MILLISECONDS)) {
        advance(p);
        s->wait_is_ms = 1;
    } else {
        parser_error(p, "expected 'seconds' or 'milliseconds'");
        return NULL; /* unreachable */
    }

    skip_optional_dot(p);
    return s;
}

/* CallStmt := CALL THE FUNCTION CALLED IDENT CallArgs [DOT]
 * Invokes the function and discards its result. */
static Stmt *parse_call(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_CALL, "expected 'call'");
    expect(p, TOK_THE, "expected 'the'");
    expect(p, TOK_FUNCTION, "expected 'function'");
    expect(p, TOK_CALLED, "expected 'called'");
    Token *name = expect(p, TOK_IDENT, "expected function name");

    Stmt *s = ast_new_stmt(STMT_CALL, line);
    s->call_name = strdup(name->text);
    s->call_args = parse_call_args(p);

    skip_optional_dot(p);
    return s;
}

/* ReturnStmt := RETURN [ THE VALUE OF Expr ] [DOT]
 * Leaves the current function, pushing the value as its result (0 when the
 * value is omitted). Only valid inside a function body. */
static Stmt *parse_return(Parser *p) {
    int line = cur(p)->line;
    expect(p, TOK_RETURN, "expected 'return'");

    Stmt *s = ast_new_stmt(STMT_RETURN, line);
    s->ret_value = NULL;
    if (check(p, TOK_THE)) {
        advance(p);
        expect(p, TOK_VALUE, "expected 'value'");
        expect(p, TOK_OF, "expected 'of'");
        s->ret_value = parse_expr(p);
    }

    skip_optional_dot(p);
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    switch (cur(p)->type) {
        case TOK_CREATE:  return parse_decl(p);
        case TOK_ASSIGN:  return parse_assign(p);
        case TOK_ADD:
        case TOK_SUBTRACT:
        case TOK_MULTIPLY:
        case TOK_DIVIDE:
        case TOK_MODULO:  return parse_arith(p);
        case TOK_WRITE:   return parse_write(p);
        case TOK_READ:    return parse_read(p);
        case TOK_WAIT:    return parse_wait(p);
        case TOK_REPEAT:  return parse_while(p);
        case TOK_WHEN:    return parse_when(p);
        case TOK_CALL:    return parse_call(p);
        case TOK_RETURN:  return parse_return(p);
        default:
            parser_error(p, "expected a statement");
            return NULL; /* unreachable */
    }
}

StmtList *parser_parse_program(Parser *p) {
    StmtList *program = malloc(sizeof(StmtList));
    stmtlist_init(program);
    while (!check(p, TOK_EOF)) {
        stmtlist_push(program, parse_stmt(p));
    }
    return program;
}
