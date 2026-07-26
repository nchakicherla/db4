#ifndef AST_H
#define AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EXPR_COLUMN,
    EXPR_LIT_INT,
    EXPR_LIT_DOUBLE,
    EXPR_LIT_STRING,
    EXPR_LIT_BOOL,
    EXPR_LIT_NULL,
    EXPR_NOT,
    EXPR_BINARY,
} ExprKind;

typedef enum {
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_AND,
    OP_OR,
} BinaryOp;

typedef struct Expr Expr;

/* All pointers/strings here are arena-allocated by the parser, into the
 * same arena as the enclosing SelectStmt - the whole tree is one
 * "throw it away as a unit" lifetime. */
struct Expr {
    ExprKind kind;
    union {
        const char *column;

        int64_t int_value;
        double  double_value;
        bool    bool_value;
        struct {
            const char *data;
            size_t      len;
        } string_value;

        Expr *not_operand;

        struct {
            BinaryOp op;
            Expr    *left;
            Expr    *right;
        } binary;
    } as;
};

typedef struct {
    char  **names;
    size_t  count;
    bool    is_star;
} ColumnList;

typedef struct {
    ColumnList columns;
    char      *table;

    Expr *where;

    bool  has_order_by;
    char *order_by_col;
    bool  order_desc;

    bool    has_limit;
    int64_t limit;
} SelectStmt;

#endif
