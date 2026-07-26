#include "budget.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

static bool   g_init    = false;
static bool   g_enforce = false;
static size_t g_total   = 0;
static size_t g_limit   = 0;
static size_t g_used    = 0;

static size_t clamp_to_size_t(uint64_t v) {
    return v > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)v;
}

static size_t detect_total_ram(void) {
#if defined(_WIN32)
    MEMORYSTATUSEX s;
    s.dwLength = sizeof s;
    if (GlobalMemoryStatusEx(&s)) return clamp_to_size_t(s.ullTotalPhys);
    return 0;
#elif defined(HW_MEMSIZE)
    int      mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t mem    = 0;
    size_t   len    = sizeof mem;
    if (sysctl(mib, 2, &mem, &len, NULL, 0) == 0) return clamp_to_size_t(mem);
    return 0;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0) return clamp_to_size_t((uint64_t)pages * (uint64_t)psize);
    return 0;
#else
    return 0;
#endif
}

unsigned budget_default_percent(size_t total_ram) {
    if (total_ram == 0) return 0;

    double gb = (double)total_ram / (double)((uint64_t)1024 * 1024 * 1024);
    if (gb <= (double)BUDGET_MIN_GB) return BUDGET_MIN_PERCENT;
    if (gb >= (double)BUDGET_MAX_GB) return BUDGET_MAX_PERCENT;

    double pct = (double)BUDGET_MIN_PERCENT +
                 (gb - (double)BUDGET_MIN_GB) *
                     (double)(BUDGET_MAX_PERCENT - BUDGET_MIN_PERCENT) /
                     (double)(BUDGET_MAX_GB - BUDGET_MIN_GB);
    return (unsigned)(pct + 0.5);
}

bool budget_parse_mb(const char *s, size_t *out_bytes) {
    if (!s || *s == '\0') return false;

    char  *end = NULL;
    double mb  = strtod(s, &end);
    while (*end == ' ') end++;
    if (end == s || *end != '\0') return false;
    if (!isfinite(mb) || mb < 0) return false;

    double bytes = mb * 1024.0 * 1024.0;
    if (bytes >= (double)SIZE_MAX) return false;

    *out_bytes = (size_t)bytes;
    return true;
}

static bool limit_from_env(bool *enforce, size_t *limit) {
    const char *env = getenv("DB4_MEM_LIMIT_MB");
    if (!env || *env == '\0') return false;

    if (strcmp(env, "none") == 0 || strcmp(env, "off") == 0 || strcmp(env, "unlimited") == 0) {
        *enforce = false;
        *limit   = 0;
        return true;
    }

    size_t bytes;
    if (!budget_parse_mb(env, &bytes)) return false;
    *enforce = bytes > 0;
    *limit   = bytes;
    return true;
}

static void budget_ensure_init(void) {
    if (g_init) return;
    g_init  = true;
    g_total = detect_total_ram();

    if (limit_from_env(&g_enforce, &g_limit)) return;

    if (g_total > 0) {
        g_limit   = g_total / 100 * budget_default_percent(g_total);
        g_enforce = true;
    } else {
        g_enforce = false;
    }
}

bool budget_would_exceed(size_t additional) {
    budget_ensure_init();
    if (!g_enforce) return false;
    if (g_used >= g_limit) return additional > 0;
    return additional > g_limit - g_used;
}

static void format_bytes(char *buf, size_t buf_len, size_t n) {
    if (n >= 1024 * 1024)      snprintf(buf, buf_len, "%.2f MB", n / (1024.0 * 1024.0));
    else if (n >= 1024)        snprintf(buf, buf_len, "%.2f KB", n / 1024.0);
    else                       snprintf(buf, buf_len, "%zu B", n);
}

void budget_describe(char *buf, size_t buf_len) {
    budget_ensure_init();
    if (!g_enforce) {
        snprintf(buf, buf_len, "no memory cap in effect");
        return;
    }

    char used[32], cap[32];
    format_bytes(used, sizeof used, g_used);
    format_bytes(cap, sizeof cap, g_limit);
    if (g_total > 0) {
        char ram[32];
        format_bytes(ram, sizeof ram, g_total);
        snprintf(buf, buf_len, "%s in use, cap is %s of %s RAM", used, cap, ram);
    } else {
        snprintf(buf, buf_len, "%s in use, cap is %s", used, cap);
    }
}

bool budget_charge(size_t bytes) {
    budget_ensure_init();
    if (g_enforce && budget_would_exceed(bytes)) return false;
    g_used += bytes;
    return true;
}

void budget_uncharge(size_t bytes) {
    g_used = bytes > g_used ? 0 : g_used - bytes;
}

size_t budget_total_ram(void) {
    budget_ensure_init();
    return g_total;
}

size_t budget_limit(void) {
    budget_ensure_init();
    return g_enforce ? g_limit : 0;
}

size_t budget_used(void) {
    return g_used;
}

bool budget_enforced(void) {
    budget_ensure_init();
    return g_enforce;
}

void budget_set_limit(size_t bytes) {
    budget_ensure_init();
    if (bytes == 0) {
        g_enforce = false;
        g_limit   = 0;
    } else {
        g_enforce = true;
        g_limit   = bytes;
    }
}
