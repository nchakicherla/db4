#ifndef BUDGET_H
#define BUDGET_H

#include <stdbool.h>
#include <stddef.h>

#define BUDGET_MIN_GB       4
#define BUDGET_MAX_GB       32
#define BUDGET_MIN_PERCENT  40
#define BUDGET_MAX_PERCENT  80

unsigned budget_default_percent(size_t total_ram);

bool budget_parse_mb(const char *s, size_t *out_bytes);

bool budget_charge(size_t bytes);
void budget_uncharge(size_t bytes);

void budget_describe(char *buf, size_t buf_len);

bool budget_would_exceed(size_t additional);

size_t budget_total_ram(void);

size_t budget_limit(void);
size_t budget_used(void);
bool   budget_enforced(void);

void budget_set_limit(size_t bytes);

#endif
