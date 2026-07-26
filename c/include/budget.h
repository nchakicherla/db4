#ifndef BUDGET_H
#define BUDGET_H

#include <stdbool.h>
#include <stddef.h>

// Process-wide memory budget. db3 keeps everything in RAM (arena.c is the
// one allocator of consequence), so on a low-spec machine a single .load of
// a file larger than physical memory would otherwise thrash the box or get
// the process OOM-killed. This module caps how much db3 will hand out before
// it stops - by default a fraction of detected physical RAM - so it stays a
// well-behaved tenant instead of trying to use the whole machine.
//
// The limit is discovered lazily on first use (see budget_ensure_init in
// budget.c): a RAM-dependent percentage of detected RAM (see
// budget_default_percent), unless DB3_MEM_LIMIT_MB overrides it (an absolute
// cap in megabytes, or "none"/"0" to disable enforcement entirely). If
// physical RAM can't be detected on this platform, enforcement stays off
// rather than guessing.
//
// Everything here is process-global on purpose: the "budget" is the whole
// process's live footprint, summed across every Table's arenas (and the
// transient CSV file buffer .load charges while streaming), not any one
// table's. It's leaf-level - it depends on nothing else in the codebase, so
// arena.c can call into it without a dependency cycle.

// The default cap is a percentage of physical RAM that itself scales with how
// much RAM there is: a bigger machine can spare a bigger share, a small one
// has to keep more in reserve for the OS and everything else. The percentage
// slides linearly between two anchor points and is clamped outside them:
//   <= BUDGET_MIN_GB of RAM -> BUDGET_MIN_PERCENT
//   >= BUDGET_MAX_GB of RAM -> BUDGET_MAX_PERCENT
//   in between              -> linear interpolation
// e.g. 4 GB -> 40%, 8 GB -> ~46%, 16 GB -> ~57%, 32 GB -> 80%.
#define BUDGET_MIN_GB       4
#define BUDGET_MAX_GB       32
#define BUDGET_MIN_PERCENT  40
#define BUDGET_MAX_PERCENT  80

// The default cap percentage for a machine with total_ram bytes of physical
// RAM, per the sliding scale above (0 if total_ram is 0). Pure function of its
// argument - exposed so the scaling can be unit-tested at synthetic RAM sizes
// without depending on the host's actual RAM.
unsigned budget_default_percent(size_t total_ram);

// Parses a memory size given in megabytes (e.g. "512", "0.5", "1024  ") into
// a byte count. Returns false - writing nothing - if s is empty, isn't a
// clean non-negative number, is non-finite (nan/inf), or is so large it would
// overflow size_t once scaled to bytes; refusing those keeps callers from
// casting a bad double straight to size_t, which is undefined behavior. A
// parse of exactly 0 - or a value so small it rounds down to 0 bytes - yields
// *out_bytes == 0, which every caller treats as "disable enforcement" rather
// than as an unusable zero-byte cap. Backs both DB3_MEM_LIMIT_MB and the
// `.budget <MB>` command so they validate identically.
bool budget_parse_mb(const char *s, size_t *out_bytes);

// Adds/removes `bytes` from the live total. budget_charge is the enforcement
// point: if the charge would push the live total past the cap it charges
// nothing and returns false, which its caller (arena.c) reports as an
// allocation failure - proactively, before the OS has to. Callers that want
// to refuse a whole operation up front rather than mid-allocation should
// still budget_would_exceed first (see cmd_load / read_file), which is the
// difference between "that file is too big" and "ran out of memory partway
// through". budget_uncharge must be paired with a prior successful charge of
// the same amount so the total doesn't drift across load/free cycles.
bool budget_charge(size_t bytes);
void budget_uncharge(size_t bytes);

// Writes a one-line, human-readable summary of the cap and current usage
// (e.g. "512.00 MB in use, cap is 6.40 GB of 16.00 GB RAM") into buf, always
// NUL-terminating. For whoever reports a refused allocation outward - the
// refusal itself happens in arena.c, which has no business printing, so the
// explanation has to be fetchable rather than emitted at the point of failure.
void budget_describe(char *buf, size_t buf_len);

// True if charging `additional` more bytes right now would exceed the cap.
// Always false when enforcement is off (unknown RAM, or explicitly disabled).
// The graceful counterpart to budget_charge's hard stop.
bool budget_would_exceed(size_t additional);

// Detected physical RAM in bytes, or 0 if this platform's detection failed.
size_t budget_total_ram(void);

// The current cap in bytes, or 0 when enforcement is off (budget_enforced()
// is false). budget_used is the live total budget_charge/uncharge track.
size_t budget_limit(void);
size_t budget_used(void);
bool   budget_enforced(void);

// Overrides the cap at runtime (backs `.budget <MB>`). A positive value turns
// enforcement on at that many bytes; 0 turns enforcement off entirely. Lowering
// the cap below the current live total is allowed - it just blocks further
// allocation until enough is freed, it never retroactively frees anything.
void budget_set_limit(size_t bytes);

#endif
