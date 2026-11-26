#include "plugin.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNKS_PER_THREAD 16

/* ---- reduction stage ---------------------------------------------------- */

typedef struct {
    const float* in;
    int w, h;
    int nchunks;
    int fixed_order;
    float* partials; /* slot per chunk, used in fixed-order mode */
} ReduceCtx;

static ReduceCtx g_rc;
static float g_total; /* arrival-order combine target (racy mode) */
static int g_next;    /* dynamic chunk queue (racy mode) */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static float chunk_sum(const float* in, int w, int y0, int y1) {
    float s = 0.0f;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < w; x++)
            s += in[y * w + x];
    return s;
}

static void chunk_rows(int h, int nchunks, int i, int* y0, int* y1) {
    *y0 = (int)((long long)i * h / nchunks);
    *y1 = (int)((long long)(i + 1) * h / nchunks);
}

/* racy: threads pull chunks from a shared queue and fold each partial into
 * one shared float in arrival order -> combine order depends on scheduling */
static void* reduce_racy(void* unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_mtx);
        int i = g_next++;
        pthread_mutex_unlock(&g_mtx);
        if (i >= g_rc.nchunks) return NULL;
        int y0, y1;
        chunk_rows(g_rc.h, g_rc.nchunks, i, &y0, &y1);
        float s = chunk_sum(g_rc.in, g_rc.w, y0, y1);
        pthread_mutex_lock(&g_mtx);
        g_total += s; /* arrival order: non-associative float add */
        pthread_mutex_unlock(&g_mtx);
    }
}

/* fixed: chunk i is computed by thread i%T, stored in slot i, and the main
 * thread folds slots in index order -> combine order is fixed */
static void* reduce_fixed(void* arg) {
    int tid = (int)(intptr_t)arg;
    int T = g_rc.nchunks / CHUNKS_PER_THREAD;
    for (int i = tid; i < g_rc.nchunks; i += T) {
        int y0, y1;
        chunk_rows(g_rc.h, g_rc.nchunks, i, &y0, &y1);
        g_rc.partials[i] = chunk_sum(g_rc.in, g_rc.w, y0, y1);
    }
    return NULL;
}

static double reduce_avg(const float* in, int w, int h, const PluginCfg* cfg) {
    int T = cfg->threads < 1 ? 1 : cfg->threads;
    int nchunks = T * CHUNKS_PER_THREAD;
    if (nchunks > h) nchunks = h;

    g_rc.in = in;
    g_rc.w = w;
    g_rc.h = h;
    g_rc.nchunks = nchunks;
    g_rc.fixed_order = cfg->fixed_order;
    g_rc.partials = calloc((size_t)nchunks, sizeof(float));
    g_total = 0.0f;
    g_next = 0;

    pthread_t* tid = malloc((size_t)T * sizeof(pthread_t));
    for (int t = 0; t < T; t++)
        pthread_create(&tid[t], NULL,
                       cfg->fixed_order ? reduce_fixed : reduce_racy,
                       (void*)(intptr_t)t);
    for (int t = 0; t < T; t++) pthread_join(tid[t], NULL);

    float total;
    if (cfg->fixed_order) {
        float s = 0.0f;
        for (int i = 0; i < nchunks; i++) s += g_rc.partials[i];
        total = s;
    } else {
        total = g_total;
    }

    free(g_rc.partials);
    free(tid);
    return (double)total / (double)((long long)w * h);
}

/* ---- cache -------------------------------------------------------------- */

static int g_cache_valid;
static uint64_t g_cache_key;
static double g_cache_avg_full;
static double g_cache_avg_lossy;

void plugin_cache_clear(void) { g_cache_valid = 0; }

static double g_last_avg;
double plugin_last_avg(void) { return g_last_avg; }

static uint64_t cache_key(const float* in, int w, int h) {
    uint64_t k = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    int n = w * h, step = n / 16 + 1;
    for (int i = 0; i < n; i += step) {
        uint32_t b;
        memcpy(&b, &in[i], sizeof(b));
        k = (k ^ b) * 1099511628211ULL;
    }
    return k;
}

/* ---- map stage ---------------------------------------------------------- */

typedef struct {
    const float* in;
    unsigned char* out;
    int w, y0, y1;
    double exposure;
} MapJob;

static void* map_band(void* arg) {
    MapJob* j = arg;
    for (int y = j->y0; y < j->y1; y++)
        for (int x = 0; x < j->w; x++) {
            int i = y * j->w + x;
            double v = 1.0 - exp(-(double)j->in[i] * j->exposure);
            int q = (int)(v * 255.0 + 0.5);
            if (q < 0) q = 0;
            if (q > 255) q = 255;
            j->out[i] = (unsigned char)q;
        }
    return NULL;
}

/* ---- entry point -------------------------------------------------------- */

void plugin_process(const float* in, unsigned char* out, int w, int h,
                    const PluginCfg* cfg) {
    double avg;
    uint64_t key = cfg->cache_enabled ? cache_key(in, w, h) : 0;
    if (cfg->cache_enabled && g_cache_valid && g_cache_key == key) {
        /* hit path: lossy cache returns the %.6f round-tripped value */
        avg = cfg->cache_lossy ? g_cache_avg_lossy : g_cache_avg_full;
    } else {
        avg = reduce_avg(in, w, h, cfg);
        if (cfg->cache_enabled) {
            char buf[32];
            g_cache_valid = 1;
            g_cache_key = key;
            g_cache_avg_full = avg;
            /* plugin persists its cache through a settings file */
            snprintf(buf, sizeof(buf), "%.6f", avg);
            g_cache_avg_lossy = strtod(buf, NULL);
        }
    }
    double exposure = 0.18 / (avg + 1e-6);
    g_last_avg = avg;

    int T = cfg->threads < 1 ? 1 : cfg->threads;
    if (T > h) T = h;
    pthread_t* tid = malloc((size_t)T * sizeof(pthread_t));
    MapJob* jobs = malloc((size_t)T * sizeof(MapJob));
    for (int t = 0; t < T; t++) {
        jobs[t] = (MapJob){ in, out, w,
                            (int)((long long)t * h / T),
                            (int)((long long)(t + 1) * h / T),
                            exposure };
        pthread_create(&tid[t], NULL, map_band, &jobs[t]);
    }
    for (int t = 0; t < T; t++) pthread_join(tid[t], NULL);
    free(jobs);
    free(tid);
}
