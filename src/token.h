#ifndef TOKEN_H
#define TOKEN_H

typedef enum {
    /* literals / identifiers */
    TOK_IDENT,
    TOK_NUMBER,     /* integer or floating-point literal */
    TOK_STRING,     /* "..." string literal */
    TOK_CHAR_LIT,   /* 'x' character literal */

    /* punctuation */
    TOK_DOT,        /* .  */
    TOK_COLON,      /* :  */

    /* keywords - one token per English word that matters to the grammar */
    TOK_CREATE,
    TOK_A,
    TOK_AN,
    TOK_VARIABLE,
    TOK_CALLED,
    TOK_AS,
    TOK_WITH,
    TOK_VALUE,
    TOK_OF,
    TOK_ASSIGN,
    TOK_THE,
    TOK_ADD,
    TOK_SUBTRACT,
    TOK_MULTIPLY,
    TOK_DIVIDE,
    TOK_MODULO,
    TOK_TO,
    TOK_FROM,
    TOK_BY,
    TOK_WRITE,
    TOK_OUT,
    TOK_READ,
    TOK_INPUT,
    TOK_INTO,
    TOK_WAIT,
    TOK_FOR,
    TOK_SECONDS,
    TOK_MILLISECONDS,
    TOK_REPEAT,
    TOK_WHILE,
    TOK_STOP,
    TOK_REPEATING,
    TOK_WHEN,
    TOK_OTHERWISE,
    TOK_IS,
    TOK_LESS,
    TOK_GREATER,
    TOK_EQUAL,
    TOK_THAN,
    TOK_AND,
    TOK_OR,
    TOK_NOT,

    /* functions */
    TOK_FUNCTION,
    TOK_PARAMETER,
    TOK_CALL,
    TOK_RETURN,

    /* type names */
    TOK_INTEGER,
    TOK_DOUBLE,
    TOK_CHAR,
    TOK_STRING_TYPE,

    /* arrays */
    TOK_ARRAY,
    TOK_SIZE,
    TOK_AT,
    TOK_INDEX,

    /* math */
    TOK_PI,

    TOK_EOF,
    TOK_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char *text;   /* owned copy of the raw lexeme (identifiers, numbers, strings) */
    int line;
    int ch;       /* valid when type == TOK_CHAR_LIT */
} Token;

const char *token_type_name(TokenType type);

#endif
