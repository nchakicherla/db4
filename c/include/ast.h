#ifndef AST_H
#define AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "table.h"

typedef enum {
    EXPR_COLUMN,
    EXPR_LIT_INT,
    EXPR_LIT_DOUBLE,
    EXPR_LIT_STRING,
    EXPR_LIT_BOOL,
    EXPR_LIT_NULL,
    EXPR_NOT,
    EXPR_NEG,
    EXPR_BINARY,
    EXPR_PARAM,
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
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
} BinaryOp;

typedef struct Expr Expr;

/* All pointers/strings here are arena-allocated by the parser, into the
 * same arena as the enclosing Stmt - the whole tree is one "throw it away
 * as a unit" lifetime. */
struct Expr {
    ExprKind kind;
    union {
        struct {
            const char *table; /* qualifier ("t.col") - NULL if unqualified */
            const char *name;
        } column;

        int64_t int_value;
        double  double_value;
        bool    bool_value;
        struct {
            const char *data;
            size_t      len;
        } string_value;

        Expr *unary_operand; /* EXPR_NOT, EXPR_NEG */

        struct {
            BinaryOp op;
            Expr    *left;
            Expr    *right;
        } binary;

        int param_index; /* EXPR_PARAM - 1-based position among this statement's "?"s */
    } as;
};

typedef struct {
    char *table; /* the joined table's name (no aliasing yet) */
    Expr *on;
} JoinClause;

typedef enum {
    AGG_COUNT,
    AGG_SUM,
    AGG_AVG,
} AggFunc;

/* One entry in a SELECT's column list - either a plain (optionally
 * table-qualified) column reference, or an aggregate call. */
typedef struct {
    bool is_agg;

    const char *table;  /* plain projection only - qualifier, NULL if none */
    const char *column; /* plain projection only */

    AggFunc     agg_func;      /* aggregate only */
    bool        agg_arg_is_star; /* aggregate only - COUNT(*) */
    const char *agg_arg_table;   /* aggregate only - qualifier, NULL if none */
    const char *agg_arg_column;  /* aggregate only - NULL if agg_arg_is_star */
} SelectItem;

typedef struct {
    SelectItem *items;
    size_t      count;
    bool        is_star;
} ColumnList;

typedef struct {
    ColumnList columns;
    char      *table;

    JoinClause *joins; /* zero or more INNER JOINs, applied left to right */
    size_t      n_joins;

    Expr *where;

    char  **group_by;
    char  **group_by_table; /* per-entry qualifier, NULL if that entry was unqualified - see parser.c's
                              * parse_qualified_name call in GROUP BY parsing and interp.c's
                              * exec_select_grouped, which is what actually validates a qualifier
                              * (GROUP BY is single-table-only, so the only valid qualifier is the
                              * query's own FROM table) - the parser itself has no semantic knowledge
                              * of which table names are meaningful. */
    size_t  n_group_by;

    bool  has_order_by;
    const char *order_by_table; /* qualifier, NULL if unqualified */
    const char *order_by_col;
    bool  order_desc;

    bool    has_limit;
    int64_t limit;
} SelectStmt;

typedef struct {
    Expr  **values;
    size_t  n_values;
} ValueRow;

typedef struct {
    char     *table;
    char    **columns; /* NULL if the "(col, ...)" list was omitted -
                        * values then map onto all table columns in order */
    size_t    n_columns;
    ValueRow *rows; /* one or more "(v1, v2, ...)" tuples */
    size_t    n_rows;
} InsertStmt;

typedef struct {
    char *column;
    Expr *value;
} Assignment;

typedef struct {
    char       *table;
    Assignment *assignments;
    size_t      n_assignments;
    Expr       *where;
} UpdateStmt;

typedef struct {
    char *table;
    Expr *where;
} DeleteStmt;

typedef struct {
    char     *name;
    FieldType type;
    bool      primary;

    bool      has_fk;
    char     *fk_table;
    char     *fk_column;
    FkAction  fk_on_delete;
    FkAction  fk_on_update;
} ColumnDef;

typedef struct {
    char      *table;
    ColumnDef *columns;
    size_t     n_columns;
} CreateTableStmt;

typedef enum {
    STMT_SELECT,
    STMT_INSERT,
    STMT_UPDATE,
    STMT_DELETE,
    STMT_CREATE_TABLE,
    STMT_BEGIN,
    STMT_COMMIT,
    STMT_ROLLBACK,
} StmtKind;

typedef struct {
    StmtKind kind;
    size_t   n_params; /* count of distinct "?" placeholders parsed in this statement */
    union {
        SelectStmt      select;
        InsertStmt      insert;
        UpdateStmt      update;
        DeleteStmt      del;
        CreateTableStmt create_table;
    } as;
} Stmt;

#endif
