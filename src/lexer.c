#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "lexer.h"

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->len = strlen(src);
    lx->line = 1;
}

static char peek(Lexer *lx) {
    if (lx->pos >= lx->len) return '\0';
    return lx->src[lx->pos];
}

static char peek2(Lexer *lx) {
    if (lx->pos + 1 >= lx->len) return '\0';
    return lx->src[lx->pos + 1];
}

static char advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') lx->line++;
    return c;
}

static void skip_whitespace_and_comments(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
        } else if (c == '#') {
            /* '#' starts a comment that runs to end of line */
            while (peek(lx) != '\0' && peek(lx) != '\n') advance(lx);
        } else if (c == '/' && peek2(lx) == '/') {
            while (peek(lx) != '\0' && peek(lx) != '\n') advance(lx);
        } else {
            break;
        }
    }
}

typedef struct {
    const char *word;
    TokenType type;
} KeywordEntry;

/* Keyword table. Anything not in this table is treated as an identifier,
 * so variable names like "x" or "counter" fall through naturally. */
static const KeywordEntry KEYWORDS[] = {
    {"create",    TOK_CREATE},
    {"a",         TOK_A},
    {"an",        TOK_AN},
    {"variable",  TOK_VARIABLE},
    {"called",    TOK_CALLED},
    {"as",        TOK_AS},
    {"with",      TOK_WITH},
    {"value",     TOK_VALUE},
    {"of",        TOK_OF},
    {"assign",    TOK_ASSIGN},
    {"the",       TOK_THE},
    {"add",       TOK_ADD},
    {"subtract",  TOK_SUBTRACT},
    {"multiply",  TOK_MULTIPLY},
    {"divide",    TOK_DIVIDE},
    {"modulo",    TOK_MODULO},
    {"to",        TOK_TO},
    {"from",      TOK_FROM},
    {"by",        TOK_BY},
    {"write",     TOK_WRITE},
    {"out",       TOK_OUT},
    {"read",      TOK_READ},
    {"input",     TOK_INPUT},
    {"into",      TOK_INTO},
    {"wait",      TOK_WAIT},
    {"for",       TOK_FOR},
    {"second",    TOK_SECONDS},
    {"seconds",   TOK_SECONDS},
    {"millisecond",  TOK_MILLISECONDS},
    {"milliseconds", TOK_MILLISECONDS},
    {"repeat",    TOK_REPEAT},
    {"while",     TOK_WHILE},
    {"stop",      TOK_STOP},
    {"repeating", TOK_REPEATING},
    {"when",      TOK_WHEN},
    {"otherwise", TOK_OTHERWISE},
    {"is",        TOK_IS},
    {"less",      TOK_LESS},
    {"greater",   TOK_GREATER},
    {"equal",     TOK_EQUAL},
    {"than",      TOK_THAN},
    {"and",       TOK_AND},
    {"or",        TOK_OR},
    {"not",       TOK_NOT},
    {"function",  TOK_FUNCTION},
    {"parameter", TOK_PARAMETER},
    {"call",      TOK_CALL},
    {"return",    TOK_RETURN},
    {"integer",   TOK_INTEGER},
    {"double",    TOK_DOUBLE},
    {"char",      TOK_CHAR},
    {"string",    TOK_STRING_TYPE},
    {"array",     TOK_ARRAY},
    {"size",      TOK_SIZE},
    {"at",        TOK_AT},
    {"index",     TOK_INDEX},
    {"pi",        TOK_PI},
    {NULL, TOK_UNKNOWN}
};

static TokenType lookup_keyword(const char *word) {
    for (int i = 0; KEYWORDS[i].word != NULL; i++) {
        if (strcasecmp(word, KEYWORDS[i].word) == 0) {
            return KEYWORDS[i].type;
        }
    }
    return TOK_IDENT;
}

static char *dup_range(const char *src, size_t start, size_t end) {
    size_t n = end - start;
    char *s = malloc(n + 1);
    memcpy(s, src + start, n);
    s[n] = '\0';
    return s;
}

/* Value of a hex digit, or -1 when `c` isn't one. */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Token lexer_next(Lexer *lx) {
    skip_whitespace_and_comments(lx);

    Token tok;
    tok.text = NULL;
    tok.line = lx->line;

    char c = peek(lx);

    if (c == '\0') {
        tok.type = TOK_EOF;
        return tok;
    }

    if (c == '.') {
        advance(lx);
        tok.type = TOK_DOT;
        return tok;
    }

    if (c == ':') {
        advance(lx);
        tok.type = TOK_COLON;
        return tok;
    }

    if (c == '"') {
        advance(lx); /* opening quote */
        size_t cap = 16, n = 0;
        char *buf = malloc(cap);
        for (;;) {
            char ch = peek(lx);
            if (ch == '\0') break;          /* unterminated: take what we have */
            if (ch == '"') { advance(lx); break; }  /* closing quote */
            if (ch == '\\') {
                advance(lx);
                char e = peek(lx);
                if (e == '\0') break;
                advance(lx);
                switch (e) {
                    case 'n':  buf[n++] = '\n'; break;
                    case 't':  buf[n++] = '\t'; break;
                    case 'r':  buf[n++] = '\r'; break;
                    case 'e':  buf[n++] = 0x1b; break; /* ESC, starts ANSI sequences */
                    case '\\': buf[n++] = '\\'; break;
                    case '"':  buf[n++] = '"';  break;
                    case '\'': buf[n++] = '\''; break;
                    case '0':  buf[n++] = '\0'; break;
                    case 'x': {
                        int d1 = hex_val(peek(lx));
                        int d2 = hex_val(peek2(lx));
                        if (d1 >= 0 && d2 >= 0) {
                            advance(lx);
                            advance(lx);
                            buf[n++] = (char)((d1 << 4) | d2);
                        } else {
                            buf[n++] = 'x';
                        }
                        break;
                    }
                    default:   buf[n++] = e;    break;
                }
            } else {
                buf[n++] = ch;
                advance(lx);
            }
            if (n >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
        }
        buf[n] = '\0';
        tok.type = TOK_STRING;
        tok.text = buf;
        return tok;
    }

    if (c == '\'') {
        advance(lx); /* opening quote */
        int value;
        if (peek(lx) == '\\') {
            advance(lx);
            char e = peek(lx);
            if (e == '\0') {
                tok.type = TOK_UNKNOWN;
                tok.text = strdup("unterminated character literal");
                return tok;
            }
            advance(lx);
            switch (e) {
                case 'n':  value = '\n'; break;
                case 't':  value = '\t'; break;
                case 'r':  value = '\r'; break;
                case 'e':  value = 0x1b; break;
                case '\\': value = '\\'; break;
                case '\'': value = '\''; break;
                case '0':  value = '\0'; break;
                default:   value = (unsigned char)e; break;
            }
        } else {
            char ch = peek(lx);
            if (ch == '\0') {
                tok.type = TOK_UNKNOWN;
                tok.text = strdup("unterminated character literal");
                return tok;
            }
            advance(lx);
            value = (unsigned char)ch;
        }
        if (peek(lx) == '\'') {
            advance(lx); /* closing quote */
            tok.type = TOK_CHAR_LIT;
            tok.ch = value;
            char buf[2] = {(char)value, '\0'};
            tok.text = strdup(buf);
        } else {
            /* multi-character or missing closing quote: bail */
            while (peek(lx) != '\0' && peek(lx) != '\'') advance(lx);
            if (peek(lx) == '\'') advance(lx);
            tok.type = TOK_UNKNOWN;
            tok.text = strdup("malformed character literal");
        }
        return tok;
    }

    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)peek2(lx)))) {
        size_t start = lx->pos;
        if (c == '-') advance(lx);
        while (isdigit((unsigned char)peek(lx))) advance(lx);
        if (peek(lx) == '.' && isdigit((unsigned char)peek2(lx))) {
            advance(lx);
            while (isdigit((unsigned char)peek(lx))) advance(lx);
        }
        if (peek(lx) == 'e' || peek(lx) == 'E') {
            size_t exp_start = lx->pos;
            advance(lx);
            if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
            if (isdigit((unsigned char)peek(lx))) {
                while (isdigit((unsigned char)peek(lx))) advance(lx);
            } else {
                lx->pos = exp_start; /* not an exponent after all */
            }
        }
        char *s = dup_range(lx->src, start, lx->pos);
        tok.type = TOK_NUMBER;
        tok.text = s;
        return tok;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = lx->pos;
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        char *s = dup_range(lx->src, start, lx->pos);
        TokenType kw = lookup_keyword(s);
        if (kw == TOK_IDENT) {
            tok.type = TOK_IDENT;
            tok.text = s;
        } else {
            tok.type = kw;
            free(s);
        }
        return tok;
    }

    /* unrecognized character: consume it and report it as TOK_UNKNOWN */
    advance(lx);
    char buf[2] = {c, '\0'};
    tok.type = TOK_UNKNOWN;
    tok.text = strdup(buf);
    return tok;
}

Token *lexer_tokenize(const char *src, int *out_count) {
    Lexer lx;
    lexer_init(&lx, src);

    size_t cap = 64;
    size_t n = 0;
    Token *tokens = malloc(cap * sizeof(Token));

    for (;;) {
        Token t = lexer_next(&lx);
        if (n >= cap) {
            cap *= 2;
            tokens = realloc(tokens, cap * sizeof(Token));
        }
        tokens[n++] = t;
        if (t.type == TOK_EOF) break;
    }

    *out_count = (int)(n - 1); /* exclude the EOF sentinel from the count */
    return tokens;
}
