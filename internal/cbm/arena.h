#ifndef CBM_ARENA_H
#define CBM_ARENA_H

#include <stddef.h>

// CBMArena is a simple bump allocator that allocates from fixed-size blocks.
// All memory is freed at once via cbm_arena_destroy(). Individual frees are not
// supported — this is by design for per-file extraction where all data has the
// same lifetime.
#define CBM_ARENA_MAX_BLOCKS 256
// Initial block size kept in sync with src/foundation/arena.h. The legacy
// arena.c in this directory is no longer linked into the production binary —
// src/foundation/arena.c is the one that actually runs — but the constant is
// kept consistent here to avoid confusion if a developer reads this header.
#define CBM_ARENA_DEFAULT_BLOCK_SIZE (4 * 1024) // 4 KB (one page)

typedef struct {
    char *blocks[CBM_ARENA_MAX_BLOCKS];
    size_t block_sizes[CBM_ARENA_MAX_BLOCKS]; // per-block sizes (for stats)
    int nblocks;
    size_t block_size;
    size_t used;        // bytes used in current block
    size_t total_alloc; // cumulative bytes allocated (for stats)
} CBMArena;

// Initialize an arena with the default block size.
void cbm_arena_init(CBMArena *a);

// Allocate n bytes from the arena. Returns NULL on OOM or block exhaustion.
// All returned pointers are 8-byte aligned.
void *cbm_arena_alloc(CBMArena *a, size_t n);

// Duplicate a string into arena memory. Returns arena-owned copy.
char *cbm_arena_strdup(CBMArena *a, const char *s);

// Duplicate a string of known length into arena memory. NUL-terminates.
char *cbm_arena_strndup(CBMArena *a, const char *s, size_t len);

// sprintf into arena memory. Returns arena-owned string.
char *cbm_arena_sprintf(CBMArena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Free all blocks. Arena is invalid after this call.
void cbm_arena_destroy(CBMArena *a);

#endif // CBM_ARENA_H
