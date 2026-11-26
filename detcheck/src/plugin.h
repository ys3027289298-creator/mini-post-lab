#ifndef PLUGIN_H
#define PLUGIN_H

/* Simulated third-party post plugin: auto-exposure tonemap.
 *
 * Stage 1: average-luminance reduction (parallel, float accumulator).
 * Stage 2: per-pixel tonemap + 8-bit quantize (parallel map, no shared state).
 *
 * Reduction order, threading and cache behavior are configurable so the
 * determinism harness can toggle each defect source independently.
 */

typedef struct {
    int threads;        /* worker threads, >= 1 */
    int fixed_order;    /* 1: combine chunk partials by slot index (deterministic)
                           0: combine in thread-arrival order (racy) */
    int cache_enabled;  /* 1: reuse cached average luminance on key hit */
    int cache_lossy;    /* 1: cache round-trips avg through text (%.6f),
                           so the hit path is not bit-identical to miss */
} PluginCfg;

void plugin_process(const float* in, unsigned char* out, int w, int h,
                    const PluginCfg* cfg);
void plugin_cache_clear(void);
double plugin_last_avg(void); /* avg luminance used by the last process call */

#endif
