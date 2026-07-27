#include "lexer.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *word;
    TokenType   type;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"select", TOK_SELECT},
    {"from",   TOK_FROM},
    {"where",  TOK_WHERE},
    {"order",  TOK_ORDER},
    {"by",     TOK_BY},
    {"limit",  TOK_LIMIT},
    {"and",    TOK_AND},
    {"or",     TOK_OR},
    {"not",    TOK_NOT},
    {"asc",    TOK_ASC},
    {"desc",   TOK_DESC},
    {"null",   TOK_NULL},
    {"true",   TOK_TRUE},
    {"false",  TOK_FALSE},

    {"insert",      TOK_INSERT},
    {"into",        TOK_INTO},
    {"values",      TOK_VALUES},
    {"update",      TOK_UPDATE},
    {"set",         TOK_SET},
    {"delete",      TOK_DELETE},
    {"begin",       TOK_BEGIN},
    {"commit",      TOK_COMMIT},
    {"rollback",    TOK_ROLLBACK},
    {"transaction", TOK_TRANSACTION},

    {"create",     TOK_CREATE},
    {"table",      TOK_TABLE},
    {"primary",    TOK_PRIMARY},
    {"key",        TOK_KEY},
    {"references", TOK_REFERENCES},
    {"on",         TOK_ON},
    {"cascade",    TOK_CASCADE},
    {"restrict",   TOK_RESTRICT},
    {"join",       TOK_JOIN},
    {"inner",      TOK_INNER},
    {"group",      TOK_GROUP},
};

static bool ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool ident_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static char peek_at(const Lexer *lx, size_t off) {
    return lx->pos + off < lx->len ? lx->src[lx->pos + off] : '\0';
}
static char peek(const Lexer *lx) { return peek_at(lx, 0); }

static Token make(TokenType type, const char *start, size_t len, size_t line) {
    Token t = { type, start, len, line };
    return t;
}

static void skip_trivia(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        if (c == '\n') { lx->line++; lx->pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { lx->pos++; continue; }
        if (c == '-' && peek_at(lx, 1) == '-') {
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        break;
    }
}

void lexer_init(Lexer *lx, const char *src, size_t len) {
    lx->src  = src;
    lx->len  = len;
    lx->pos  = 0;
    lx->line = 1;
}

Token lexer_next(Lexer *lx) {
    skip_trivia(lx);

    size_t line = lx->line;
    const char *start = lx->src + lx->pos;

    if (lx->pos >= lx->len) return make(TOK_EOF, start, 0, line);

    char c = peek(lx);

    if (ident_start(c)) {
        size_t n = 0;
        while (ident_char(peek_at(lx, n))) n++;
        lx->pos += n;
        for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
            size_t klen = strlen(KEYWORDS[i].word);
            if (klen == n && strncasecmp(start, KEYWORDS[i].word, n) == 0)
                return make(KEYWORDS[i].type, start, n, line);
        }
        return make(TOK_IDENT, start, n, line);
    }

    if (isdigit((unsigned char)c)) {
        size_t n = 0;
        while (isdigit((unsigned char)peek_at(lx, n))) n++;
        bool is_float = false;
        if (peek_at(lx, n) == '.' && isdigit((unsigned char)peek_at(lx, n + 1))) {
            is_float = true;
            n++;
            while (isdigit((unsigned char)peek_at(lx, n))) n++;
        }
        lx->pos += n;
        return make(is_float ? TOK_FLOAT : TOK_INT, start, n, line);
    }

    if (c == '\'') {
        size_t n = 1;
        for (;;) {
            char cc = peek_at(lx, n);
            if (cc == '\0') {
                lx->pos += n;
                return make(TOK_ERROR, start, n, line);
            }
            if (cc == '\n') lx->line++;
            if (cc == '\'') {
                if (peek_at(lx, n + 1) == '\'') { n += 2; continue; }
                n++;
                break;
            }
            n++;
        }
        lx->pos += n;
        return make(TOK_STRING, start, n, line);
    }

    switch (c) {
        case '*': lx->pos++; return make(TOK_STAR, start, 1, line);
        case ',': lx->pos++; return make(TOK_COMMA, start, 1, line);
        case '.': lx->pos++; return make(TOK_DOT, start, 1, line);
        case ';': lx->pos++; return make(TOK_SEMICOLON, start, 1, line);
        case '(': lx->pos++; return make(TOK_LPAREN, start, 1, line);
        case ')': lx->pos++; return make(TOK_RPAREN, start, 1, line);
        case '=': lx->pos++; return make(TOK_EQ, start, 1, line);
        case '+': lx->pos++; return make(TOK_PLUS, start, 1, line);
        case '-': lx->pos++; return make(TOK_MINUS, start, 1, line);
        case '/': lx->pos++; return make(TOK_SLASH, start, 1, line);
        case '!':
            if (peek_at(lx, 1) == '=') { lx->pos += 2; return make(TOK_NE, start, 2, line); }
            break;
        case '<':
            if (peek_at(lx, 1) == '=') { lx->pos += 2; return make(TOK_LE, start, 2, line); }
            if (peek_at(lx, 1) == '>') { lx->pos += 2; return make(TOK_NE, start, 2, line); }
            lx->pos++;
            return make(TOK_LT, start, 1, line);
        case '>':
            if (peek_at(lx, 1) == '=') { lx->pos += 2; return make(TOK_GE, start, 2, line); }
            lx->pos++;
            return make(TOK_GT, start, 1, line);
        default: break;
    }

    lx->pos++;
    return make(TOK_ERROR, start, 1, line);
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_EOF:       return "end of input";
        case TOK_ERROR:     return "invalid token";
        case TOK_IDENT:     return "identifier";
        case TOK_INT:       return "integer literal";
        case TOK_FLOAT:     return "float literal";
        case TOK_STRING:    return "string literal";
        case TOK_SELECT:    return "SELECT";
        case TOK_FROM:      return "FROM";
        case TOK_WHERE:     return "WHERE";
        case TOK_ORDER:     return "ORDER";
        case TOK_BY:        return "BY";
        case TOK_LIMIT:     return "LIMIT";
        case TOK_AND:       return "AND";
        case TOK_OR:        return "OR";
        case TOK_NOT:       return "NOT";
        case TOK_ASC:       return "ASC";
        case TOK_DESC:      return "DESC";
        case TOK_NULL:      return "NULL";
        case TOK_TRUE:      return "TRUE";
        case TOK_FALSE:     return "FALSE";
        case TOK_INSERT:      return "INSERT";
        case TOK_INTO:        return "INTO";
        case TOK_VALUES:      return "VALUES";
        case TOK_UPDATE:      return "UPDATE";
        case TOK_SET:         return "SET";
        case TOK_DELETE:      return "DELETE";
        case TOK_BEGIN:       return "BEGIN";
        case TOK_COMMIT:      return "COMMIT";
        case TOK_ROLLBACK:    return "ROLLBACK";
        case TOK_TRANSACTION: return "TRANSACTION";
        case TOK_CREATE:     return "CREATE";
        case TOK_TABLE:      return "TABLE";
        case TOK_PRIMARY:    return "PRIMARY";
        case TOK_KEY:        return "KEY";
        case TOK_REFERENCES: return "REFERENCES";
        case TOK_ON:         return "ON";
        case TOK_CASCADE:    return "CASCADE";
        case TOK_RESTRICT:   return "RESTRICT";
        case TOK_JOIN:       return "JOIN";
        case TOK_INNER:      return "INNER";
        case TOK_GROUP:      return "GROUP";
        case TOK_STAR:      return "*";
        case TOK_COMMA:     return ",";
        case TOK_DOT:       return ".";
        case TOK_SEMICOLON: return ";";
        case TOK_LPAREN:    return "(";
        case TOK_RPAREN:    return ")";
        case TOK_EQ:        return "=";
        case TOK_NE:        return "!=";
        case TOK_LT:        return "<";
        case TOK_LE:        return "<=";
        case TOK_GT:        return ">";
        case TOK_GE:        return ">=";
        case TOK_PLUS:      return "+";
        case TOK_MINUS:     return "-";
        case TOK_SLASH:     return "/";
    }
    return "?";
}
