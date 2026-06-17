/*
 * mem.c — Unified memory management via mimalloc.
 *
 * Budget tracking based on actual RSS via mi_process_info().
 * When MI_OVERRIDE=0 (ASan builds), falls back to OS-specific
 * RSS queries (task_info on macOS, /proc/self/statm on Linux,
 * GetProcessMemoryInfo on Windows).
 */
#include "mem.h"
#include "platform.h"
#include "log.h"

#include "foundation/constants.h"

#define MAX_RAM_FRACTION 1.0
#define DEFAULT_RAM_FRACTION 0.5
#include <mimalloc.h>
#include <stdatomic.h>
#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

/* ── Static state ─────────────────────────────────────────────── */

static size_t g_budget;          /* budget in bytes */
static atomic_int g_initialized; /* init guard */
static atomic_int g_was_over;    /* pressure hysteresis */

#define MB_DIVISOR ((size_t)(CBM_SZ_1K * CBM_SZ_1K))

/* ── OS fallback for RSS (ASan builds where MI_OVERRIDE=0) ──── */

/* BSD: read kinfo_proc for this PID via sysctl. ki_rssize is in pages,
 * ki_size is the VM map total in bytes. */
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
static int bsd_kinfo(struct kinfo_proc *kp) {
#if defined(__OpenBSD__)
    int mib[6] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid(), sizeof(*kp), 1};
    size_t len = sizeof(*kp);
    return sysctl(mib, 6, kp, &len, NULL, 0);
#else
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    size_t len = sizeof(*kp);
    return sysctl(mib, 4, kp, &len, NULL, 0);
#endif
}
#endif

static size_t os_rss(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return (size_t)pmc.WorkingSetSize;
    }
    return 0;
#elif defined(__APPLE__)
    struct mach_task_basic_info info = {0};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) ==
        KERN_SUCCESS) {
        return (size_t)info.resident_size;
    }
    return 0;
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    struct kinfo_proc kp;
    if (bsd_kinfo(&kp) != 0) {
        return 0;
    }
    long ps = sysconf(_SC_PAGESIZE);
    size_t pgsz = ps > 0 ? (size_t)ps : CBM_SZ_4K;
#if defined(__OpenBSD__)
    return (size_t)kp.p_vm_rssize * pgsz;
#else
    return (size_t)kp.ki_rssize * pgsz;
#endif
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) {
        return 0;
    }
    unsigned long pages = 0;
    unsigned long rss_pages = 0;
    if (fscanf(f, "%lu %lu", &pages, &rss_pages) != 2) {
        rss_pages = 0;
    }
    (void)fclose(f);
    long ps = sysconf(_SC_PAGESIZE);
    return rss_pages * (ps > 0 ? (size_t)ps : CBM_SZ_4K);
#endif
}

static size_t os_vsz(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        return (size_t)pmc.PrivateUsage;
    }
    return 0;
#elif defined(__APPLE__)
    struct mach_task_basic_info info = {0};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) ==
        KERN_SUCCESS) {
        return (size_t)info.virtual_size;
    }
    return 0;
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    struct kinfo_proc kp;
    if (bsd_kinfo(&kp) != 0) {
        return 0;
    }
#if defined(__OpenBSD__)
    long ps = sysconf(_SC_PAGESIZE);
    size_t pgsz = ps > 0 ? (size_t)ps : CBM_SZ_4K;
    /* OpenBSD's kinfo_proc tracks VSZ in pages across text/data/stack. */
    return ((size_t)kp.p_vm_tsize + (size_t)kp.p_vm_dsize + (size_t)kp.p_vm_ssize) * pgsz;
#else
    return (size_t)kp.ki_size;
#endif
#else
    /* Linux: /proc/self/statm column 1 is total VM in pages. */
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) {
        return 0;
    }
    unsigned long pages = 0;
    if (fscanf(f, "%lu", &pages) != 1) {
        pages = 0;
    }
    (void)fclose(f);
    long ps = sysconf(_SC_PAGESIZE);
    return pages * (ps > 0 ? (size_t)ps : CBM_SZ_4K);
#endif
}

/* Address-space ceiling derived from RLIMIT_AS (if set) or 0 when unlimited.
 * The kernel SIGKILLs the process when VSZ crosses this — we want to bail out
 * cleanly somewhat before then. */
static size_t os_addr_space_limit(void) {
#ifdef _WIN32
    /* Windows has no direct RLIMIT_AS equivalent in the standard API surface;
     * commit charge is the practical ceiling and is reported via VSZ already. */
    return 0;
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) != 0) {
        return 0;
    }
    if (rl.rlim_cur == RLIM_INFINITY) {
        return 0;
    }
    return (size_t)rl.rlim_cur;
#endif
}

/* ── Pressure logging (hysteresis) ────────────────────────────── */

static void check_pressure(size_t rss) {
    if (g_budget == 0) {
        return;
    }

    bool over = rss > g_budget;
    int was = atomic_load(&g_was_over);

    if (over && !was) {
        atomic_store(&g_was_over, 1);
        char rss_mb[CBM_SZ_32];
        char budget_mb[CBM_SZ_32];
        char pct_str[CBM_SZ_16];
        snprintf(rss_mb, sizeof(rss_mb), "%zu", rss / MB_DIVISOR);
        snprintf(budget_mb, sizeof(budget_mb), "%zu", g_budget / MB_DIVISOR);
        snprintf(pct_str, sizeof(pct_str), "%zu",
                 g_budget > 0 ? (rss * CBM_PERCENT) / g_budget : 0);
        cbm_log_warn("mem.pressure.warn", "rss_mb", rss_mb, "budget_mb", budget_mb, "pct", pct_str);
    } else if (!over && was) {
        atomic_store(&g_was_over, 0);
        char rss_mb[CBM_SZ_32];
        char budget_mb[CBM_SZ_32];
        char pct_str[CBM_SZ_16];
        snprintf(rss_mb, sizeof(rss_mb), "%zu", rss / MB_DIVISOR);
        snprintf(budget_mb, sizeof(budget_mb), "%zu", g_budget / MB_DIVISOR);
        snprintf(pct_str, sizeof(pct_str), "%zu",
                 g_budget > 0 ? (rss * CBM_PERCENT) / g_budget : 0);
        cbm_log_info("mem.pressure.ok", "rss_mb", rss_mb, "budget_mb", budget_mb, "pct", pct_str);
    }
}

/* ── Public API ────────────────────────────────────────────────── */

void cbm_mem_init(double ram_fraction) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_initialized, &expected, 1)) {
        return;
    }

    if (ram_fraction <= 0.0 || ram_fraction > MAX_RAM_FRACTION) {
        ram_fraction = DEFAULT_RAM_FRACTION;
    }

    /* Reduce upfront memory: don't eagerly commit arenas.
     * Force decommit on purge (MADV_FREE_REUSABLE on macOS) so RSS
     * drops immediately instead of staying high until memory pressure. */
    mi_option_set(mi_option_arena_eager_commit, 0);
    mi_option_set(mi_option_purge_decommits, SKIP_ONE);
    mi_option_set(mi_option_purge_delay, 0); /* immediate purge, no 1s delay */

    cbm_system_info_t info = cbm_system_info();
    g_budget = (size_t)((double)info.total_ram * ram_fraction);

    char budget_mb[CBM_SZ_32];
    char ram_mb[CBM_SZ_32];
    snprintf(budget_mb, sizeof(budget_mb), "%zu", g_budget / MB_DIVISOR);
    snprintf(ram_mb, sizeof(ram_mb), "%zu", info.total_ram / MB_DIVISOR);
    cbm_log_info("mem.init", "budget_mb", budget_mb, "total_ram_mb", ram_mb);
}

size_t cbm_mem_rss(void) {
    size_t current_rss = 0;
    size_t peak_rss = 0;
    mi_process_info(NULL, NULL, NULL, &current_rss, &peak_rss, NULL, NULL, NULL);
    if (current_rss > 0) {
        return current_rss;
    }
    /* Fallback for ASan builds (MI_OVERRIDE=0) */
    return os_rss();
}

size_t cbm_mem_peak_rss(void) {
    size_t peak_rss = 0;
    mi_process_info(NULL, NULL, NULL, NULL, &peak_rss, NULL, NULL, NULL);
    if (peak_rss > 0) {
        return peak_rss;
    }
    /* No OS fallback for peak — return current as best approximation */
    return os_rss();
}

size_t cbm_mem_vsz(void) {
    return os_vsz();
}

size_t cbm_mem_budget(void) {
    return g_budget;
}

/* VSZ ceiling = min(budget * 1.5, RLIMIT_AS * 0.9).
 * The 1.5x factor over the RSS budget tolerates mimalloc's lazy-commit
 * VSZ overhead in normal operation. The 0.9 RLIMIT_AS multiplier leaves
 * headroom so we trip our own back-pressure before the kernel SIGKILLs us
 * on `ulimit -v`. */
static size_t vsz_ceiling(void) {
    size_t soft = g_budget + g_budget / 2; /* budget * 1.5 */
    size_t hard = os_addr_space_limit();
    if (hard == 0) {
        return soft;
    }
    size_t hard90 = hard - (hard / 10); /* hard * 0.9 */
    return soft < hard90 ? soft : hard90;
}

bool cbm_mem_over_budget(void) {
    size_t rss = cbm_mem_rss();
    check_pressure(rss);
    if (rss > g_budget) {
        return true;
    }
    size_t vsz = cbm_mem_vsz();
    return vsz > 0 && vsz > vsz_ceiling();
}

size_t cbm_mem_worker_budget(int num_workers) {
    if (num_workers <= 0) {
        num_workers = SKIP_ONE;
    }
    return g_budget / (size_t)num_workers;
}

void cbm_mem_collect(void) {
    mi_collect(true);
}
