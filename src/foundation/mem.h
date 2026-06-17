/*
 * mem.h — Unified memory management via mimalloc.
 *
 * Provides budget tracking based on actual RSS (not partial vmem tracking).
 * Uses mi_process_info() as the single source of truth for memory pressure.
 * Replaces the old vmem.h budget-tracked virtual memory allocator.
 */
#ifndef CBM_MEM_H
#define CBM_MEM_H

#include <stdbool.h>
#include <stddef.h>

/* Initialize memory budget = ram_fraction * total_physical_ram.
 * Thread-safe: only the first call takes effect.
 * Configures mimalloc options for reduced upfront memory. */
void cbm_mem_init(double ram_fraction);

/* Current RSS in bytes via mi_process_info().
 * Falls back to OS-specific queries when MI_OVERRIDE=0 (ASan builds). */
size_t cbm_mem_rss(void);

/* Peak RSS in bytes. */
size_t cbm_mem_peak_rss(void);

/* Current virtual address-space footprint in bytes (the "VSZ" column in ps).
 * Distinct from RSS because mimalloc reserves arenas lazily — VSZ can be much
 * higher than RSS for long-running allocator-heavy processes, and on systems
 * where the operator caps address space with `ulimit -v` (FreeBSD's default,
 * notably) the process gets SIGKILL'd on VSZ overflow long before RSS reaches
 * the budget. Returns 0 if VSZ cannot be determined on this platform. */
size_t cbm_mem_vsz(void);

/* Total budget in bytes. */
size_t cbm_mem_budget(void);

/* Returns true if current RSS exceeds the budget, or VSZ exceeds the
 * address-space ceiling derived from min(budget * 1.5, RLIMIT_AS * 0.9).
 * The VSZ check catches the "VSZ runs ahead of RSS" failure mode (FreeBSD
 * under `ulimit -v`) before the kernel kills the process. */
bool cbm_mem_over_budget(void);

/* Per-worker budget hint: budget / num_workers. */
size_t cbm_mem_worker_budget(int num_workers);

/* Return unused pages to the OS. Call between files to bound per-file peak. */
void cbm_mem_collect(void);

#endif /* CBM_MEM_H */
