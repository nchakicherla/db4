#include "schema.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "field.h"

static void set_error(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static bool split_reference(Arena *a, const char *ref, char **out_table, char **out_column) {
    const char *dot = strchr(ref, '.');
    if (!dot || dot == ref || dot[1] == '\0') return false;

    *out_table  = arena_strndup(a, ref, (size_t)(dot - ref));
    *out_column = arena_strdup(a, dot + 1);
    return true;
}

static bool fk_action_from_name(const char *name, FkAction *out) {
    size_t len = strlen(name);
    if (len == 0 || len >= 16) return false;

    char buf[16];
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)name[i]);
    buf[len] = '\0';

    if (strcmp(buf, "cascade") == 0)  { *out = FK_ACTION_CASCADE;  return true; }
    if (strcmp(buf, "set_null") == 0) { *out = FK_ACTION_SET_NULL; return true; }
    if (strcmp(buf, "restrict") == 0) { *out = FK_ACTION_RESTRICT; return true; }
    return false;
}

bool schema_parse(const char *json, Arena *a, SchemaOverride *out, char *err, size_t err_len) {
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &parse_end, 0);
    if (!root) {
        set_error(err, err_len, "malformed json");
        return false;
    }
    while (*parse_end == ' ' || *parse_end == '\t' || *parse_end == '\n' || *parse_end == '\r') parse_end++;
    if (*parse_end != '\0') {
        set_error(err, err_len, "unexpected text after schema json");
        cJSON_Delete(root);
        return false;
    }

    if (!cJSON_IsObject(root)) {
        set_error(err, err_len, "schema must be a json object");
        cJSON_Delete(root);
        return false;
    }

    size_t count = (size_t)cJSON_GetArraySize(root);
    char      **names        = arena_alloc(a, count * sizeof(char *), _Alignof(char *));
    FieldType  *types        = arena_alloc(a, count * sizeof(FieldType), _Alignof(FieldType));
    bool       *primary      = arena_zalloc(a, count * sizeof(bool), _Alignof(bool));
    char      **fk_tables    = arena_zalloc(a, count * sizeof(char *), _Alignof(char *));
    char      **fk_columns   = arena_zalloc(a, count * sizeof(char *), _Alignof(char *));
    FkAction   *fk_on_delete = arena_zalloc(a, count * sizeof(FkAction), _Alignof(FkAction));
    FkAction   *fk_on_update = arena_zalloc(a, count * sizeof(FkAction), _Alignof(FkAction));
    if (arena_failed(a)) {
        set_error(err, err_len, "out of memory");
        cJSON_Delete(root);
        return false;
    }

    size_t i = 0;
    cJSON *field = NULL;
    cJSON_ArrayForEach(field, root) {
        const char *type_str      = NULL;
        const char *ref_str       = NULL;
        const char *on_delete_str = NULL;
        const char *on_update_str = NULL;
        bool        is_primary    = false;

        if (cJSON_IsString(field)) {
            type_str = field->valuestring;
        } else if (cJSON_IsObject(field)) {
            cJSON *type_item = cJSON_GetObjectItemCaseSensitive(field, "type");
            if (!type_item || !cJSON_IsString(type_item)) {
                set_error(err, err_len, "column \"%s\" is missing \"type\"", field->string);
                cJSON_Delete(root);
                return false;
            }
            type_str = type_item->valuestring;

            cJSON *primary_item = cJSON_GetObjectItemCaseSensitive(field, "primary");
            if (primary_item) {
                if (!cJSON_IsBool(primary_item)) {
                    set_error(err, err_len, "\"primary\" value for column \"%s\" is not a boolean", field->string);
                    cJSON_Delete(root);
                    return false;
                }
                is_primary = cJSON_IsTrue(primary_item);
            }

            cJSON *ref_item = cJSON_GetObjectItemCaseSensitive(field, "references");
            if (ref_item) {
                if (!cJSON_IsString(ref_item)) {
                    set_error(err, err_len, "\"references\" value for column \"%s\" is not a string", field->string);
                    cJSON_Delete(root);
                    return false;
                }
                ref_str = ref_item->valuestring;
            }

            cJSON *on_delete_item = cJSON_GetObjectItemCaseSensitive(field, "on_delete");
            if (on_delete_item) {
                if (!cJSON_IsString(on_delete_item)) {
                    set_error(err, err_len, "\"on_delete\" value for column \"%s\" is not a string", field->string);
                    cJSON_Delete(root);
                    return false;
                }
                on_delete_str = on_delete_item->valuestring;
            }

            cJSON *on_update_item = cJSON_GetObjectItemCaseSensitive(field, "on_update");
            if (on_update_item) {
                if (!cJSON_IsString(on_update_item)) {
                    set_error(err, err_len, "\"on_update\" value for column \"%s\" is not a string", field->string);
                    cJSON_Delete(root);
                    return false;
                }
                on_update_str = on_update_item->valuestring;
            }

            if ((on_delete_str || on_update_str) && !ref_str) {
                set_error(err, err_len, "column \"%s\" declares \"on_delete\"/\"on_update\" without \"references\"", field->string);
                cJSON_Delete(root);
                return false;
            }
        } else {
            set_error(err, err_len, "value for column \"%s\" is not a string or object", field->string);
            cJSON_Delete(root);
            return false;
        }

        FieldType type;
        if (!field_type_from_name(type_str, &type)) {
            set_error(err, err_len, "unknown type \"%s\" for column \"%s\"", type_str, field->string);
            cJSON_Delete(root);
            return false;
        }

        if (ref_str && !split_reference(a, ref_str, &fk_tables[i], &fk_columns[i])) {
            set_error(err, err_len, "malformed \"references\" value \"%s\" for column \"%s\" (expected \"table.column\")",
                       ref_str, field->string);
            cJSON_Delete(root);
            return false;
        }

        if (on_delete_str && !fk_action_from_name(on_delete_str, &fk_on_delete[i])) {
            set_error(err, err_len, "unknown \"on_delete\" value \"%s\" for column \"%s\" (expected \"cascade\", \"set_null\", or \"restrict\")",
                       on_delete_str, field->string);
            cJSON_Delete(root);
            return false;
        }
        if (on_update_str && !fk_action_from_name(on_update_str, &fk_on_update[i])) {
            set_error(err, err_len, "unknown \"on_update\" value \"%s\" for column \"%s\" (expected \"cascade\", \"set_null\", or \"restrict\")",
                       on_update_str, field->string);
            cJSON_Delete(root);
            return false;
        }

        names[i]   = arena_strdup(a, field->string);
        types[i]   = type;
        primary[i] = is_primary;
        i++;
    }

    if (arena_failed(a)) {
        set_error(err, err_len, "out of memory");
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);

    out->names        = names;
    out->types        = types;
    out->primary      = primary;
    out->fk_tables    = fk_tables;
    out->fk_columns   = fk_columns;
    out->fk_on_delete = fk_on_delete;
    out->fk_on_update = fk_on_update;
    out->count        = count;
    return true;
}

bool schema_lookup(const SchemaOverride *s, const char *name, FieldType *out_type) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) {
            *out_type = s->types[i];
            return true;
        }
    }
    return false;
}

bool schema_lookup_fk(const SchemaOverride *s, const char *name, const char **out_table, const char **out_column) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) {
            if (!s->fk_tables[i]) return false;
            *out_table  = s->fk_tables[i];
            *out_column = s->fk_columns[i];
            return true;
        }
    }
    return false;
}
