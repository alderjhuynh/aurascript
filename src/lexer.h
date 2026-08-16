#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    int line;
} Lexer;

void lexer_init(Lexer *lx, const char *src);

/* Returns a heap-allocated Token; caller does not need to free token.text
 * for keyword tokens (it's NULL), but should free it for IDENT/NUMBER/STRING
 * if they care about memory (this is a toy compiler, so we mostly don't). */
Token lexer_next(Lexer *lx);

/* Tokenizes the whole source into a NULL-terminated dynamic array.
 * out_count receives the number of tokens (excluding a trailing TOK_EOF
 * sentinel, which is still present at index out_count). */
Token *lexer_tokenize(const char *src, int *out_count);

#endif
