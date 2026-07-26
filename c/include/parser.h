#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "arena.h"
#include "ast.h"
#include "lexer.h"

/* Fails latched, like Arena's ArenaFailure: the first error is recorded
 * and every later parse_* call is a no-op until the caller reads it back
 * via parser_error()/parser_error_line() - no error code threaded through
 * every grammar production. */
typedef struct {
    Lexer  lexer;
    Token  cur;
    Arena *arena;

    bool   failed;
    char   err[128];
    size_t err_line;
} Parser;

void parser_init(Parser *p, const char *sql, size_t len, Arena *a);

/* Parses "SELECT <cols> FROM <table> [WHERE <expr>] [ORDER BY <col> [ASC|DESC]]
 * [LIMIT <n>] [;]". Returns NULL on failure - check parser_failed(p). */
SelectStmt *parser_parse_select(Parser *p);

bool        parser_failed(const Parser *p);
const char *parser_error(const Parser *p);
size_t      parser_error_line(const Parser *p);

void select_stmt_print(FILE *f, const SelectStmt *stmt);

#endif
