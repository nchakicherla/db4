#include "txn.h"

#include <stdio.h>
#include <stdlib.h>

void txn_init(Txn *txn) {
    txn->active = false;
    txn->log    = NULL;
    txn->count  = 0;
    txn->cap    = 0;
}

void txn_term(Txn *txn) {
    free(txn->log);
    txn->log   = NULL;
    txn->count = 0;
    txn->cap   = 0;
}

static UndoEntry *push(Txn *txn) {
    if (txn->count == txn->cap) {
        size_t new_cap = txn->cap ? txn->cap * 2 : 16;
        UndoEntry *grown = realloc(txn->log, new_cap * sizeof(UndoEntry));
        if (!grown) return NULL;
        txn->log = grown;
        txn->cap = new_cap;
    }
    return &txn->log[txn->count++];
}

bool txn_begin(Txn *txn, char *err, size_t err_len) {
    if (txn->active) {
        snprintf(err, err_len, "a transaction is already active");
        return false;
    }
    txn->active = true;
    txn->count  = 0;
    return true;
}

bool txn_log_insert(Txn *txn, size_t table_idx, const char *table_name, size_t row) {
    UndoEntry *e = push(txn);
    if (!e) return false;
    e->kind       = UNDO_INSERT;
    e->table_idx  = table_idx;
    e->table_name = table_name;
    e->row        = row;
    return true;
}

bool txn_log_update(Txn *txn, Table *table, size_t table_idx, const char *table_name, size_t row, size_t col) {
    UndoEntry *e = push(txn);
    if (!e) return false;
    e->kind       = UNDO_UPDATE;
    e->table_idx  = table_idx;
    e->table_name = table_name;
    e->row        = row;
    e->col        = col;
    e->type       = table->types[col];
    e->was_null   = table_is_null(table, row, col);

    if (!e->was_null) {
        switch (e->type) {
            case FT_INT:    e->old.i = table_get_int(table, row, col); break;
            case FT_DOUBLE: e->old.d = table_get_double(table, row, col); break;
            case FT_BOOL:   e->old.b = table_get_bool(table, row, col); break;
            case FT_TEXT:   e->old.s = table_get_text_ref(table, row, col); break;
            default: break;
        }
    }
    return true;
}

bool txn_log_delete(Txn *txn, size_t table_idx, const char *table_name, size_t row) {
    UndoEntry *e = push(txn);
    if (!e) return false;
    e->kind       = UNDO_DELETE;
    e->table_idx  = table_idx;
    e->table_name = table_name;
    e->row        = row;
    return true;
}

static void undo_one(const UndoEntry *e, Catalog *catalog) {
    Table *table = &catalog->tables[e->table_idx].table;
    switch (e->kind) {
        case UNDO_INSERT:
            table_delete_row(table, e->row);
            return;
        case UNDO_DELETE:
            table_undelete_row(table, e->row);
            return;
        case UNDO_UPDATE:
            if (e->was_null) {
                table_set_null(table, e->row, e->col);
                return;
            }
            switch (e->type) {
                case FT_INT:    table_set_int(table, e->row, e->col, e->old.i); return;
                case FT_DOUBLE: table_set_double(table, e->row, e->col, e->old.d); return;
                case FT_BOOL:   table_set_bool(table, e->row, e->col, e->old.b); return;
                case FT_TEXT:   table_set_text_ref(table, e->row, e->col, e->old.s); return;
                default: return;
            }
    }
}

void txn_rollback(Txn *txn, Catalog *catalog) {
    for (size_t i = txn->count; i > 0; i--)
        undo_one(&txn->log[i - 1], catalog);
    txn->count  = 0;
    txn->active = false;
}

void txn_rollback_to(Txn *txn, Catalog *catalog, size_t mark) {
    for (size_t i = txn->count; i > mark; i--)
        undo_one(&txn->log[i - 1], catalog);
    txn->count = mark;
}
