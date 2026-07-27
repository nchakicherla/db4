#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,

    TOK_SELECT,
    TOK_FROM,
    TOK_WHERE,
    TOK_ORDER,
    TOK_BY,
    TOK_LIMIT,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_ASC,
    TOK_DESC,
    TOK_NULL,
    TOK_TRUE,
    TOK_FALSE,

    TOK_INSERT,
    TOK_INTO,
    TOK_VALUES,
    TOK_UPDATE,
    TOK_SET,
    TOK_DELETE,
    TOK_BEGIN,
    TOK_COMMIT,
    TOK_ROLLBACK,
    TOK_TRANSACTION,

    TOK_CREATE,
    TOK_TABLE,
    TOK_PRIMARY,
    TOK_KEY,
    TOK_REFERENCES,
    TOK_ON,
    TOK_CASCADE,
    TOK_RESTRICT,
    TOK_JOIN,
    TOK_INNER,
    TOK_GROUP,

    TOK_STAR,
    TOK_COMMA,
    TOK_DOT,
    TOK_QUESTION,
    TOK_SEMICOLON,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_PLUS,
    TOK_MINUS,
    TOK_SLASH,
} TokenType;

/* start/len point into the Lexer's source buffer - the caller owns that
 * buffer for at least as long as any Token or arena copy derived from it
 * is in use. */
typedef struct {
    TokenType   type;
    const char *start;
    size_t      len;
    size_t      line;
} Token;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    size_t      line;
} Lexer;

void lexer_init(Lexer *lx, const char *src, size_t len);

/* On malformed input (unterminated string, stray character) returns a
 * TOK_ERROR token rather than latching a failure itself - the parser is
 * the one with an error boundary to latch against. */
Token lexer_next(Lexer *lx);

const char *token_type_name(TokenType type);

#endif
