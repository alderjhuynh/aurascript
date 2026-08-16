#ifndef PARSER_H
#define PARSER_H

#include "token.h"
#include "ast.h"

typedef struct {
    Token *tokens;
    int count;
    int pos;
} Parser;

void parser_init(Parser *p, Token *tokens, int count);

/* Parses the whole token stream into a program (list of top-level
 * statements). On a syntax error, prints a diagnostic to stderr and
 * exits(1) -- this is a toy compiler, not a language server, so we
 * keep error recovery out of scope for now. */
StmtList *parser_parse_program(Parser *p);

#endif
