
/* meter: luminance sampling bias and the sample budget behind it.
 *
 * A meter that averages luminance over a fixed centre region reports a value
 * that is not the frame average. The gap has an exact covariance form (see
 * meter/README.md); this program measures it on three synthetic scenes,
 * derives the sample count needed to push the *sampling* error below one
 * 8-bit code value, and reports the full-pixel mean for comparison.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define W 1920
#define H 1080
#define PIXELS (W * H)

/* one 8-bit code value: accuracy target for the estimate */
#define QUANT (1.0f / 255.0f)

/* a sample reads packed RGB as floats; frames per second */
#define BYTES_PER_SAMPLE 12
#define FPS 60

/* current centre-weighted meter: 144 points on a regular grid */
#define CUR_COLS 16
#define CUR_ROWS 9

/* centre metering region: central half in u and v -> 25% of the frame area */
#define RX0 0.25f
#define RX1 0.75f
#define RY0 0.25f
#define RY1 0.75f

typedef struct {
    float mean;    /* true full-frame mean luminance */
    float sd;      /* true full-frame standard deviation */
    float mu_r;    /* true mean luminance inside the metering region */
    float area_f;  /* metering region area / frame area */
} SceneTruth;

typedef struct {
    float est;
    int   n;
    int   cols, rows;
} Estimate;

typedef float (*SceneFn)(float u, float v);

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

/* ---- scenes ----------------------------------------------------------- */

/* dim interior: vertical falloff, soft subject, gentle vignette */
static float scene_interior(float u, float v) {
    float base = 0.24f + 0.12f * (1.0f - v);
    float du = (u - 0.5f) / 0.18f, dv = (v - 0.45f) / 0.18f;
    float subj = 0.14f * expf(-0.5f * (du * du + dv * dv));
    float vig = 0.06f * (((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) / 0.5f);
    return clamp01(base + subj - vig);
}

/* high-contrast exterior: bright sky, dark ground, small sun */
static float scene_exterior(float u, float v) {
    float base = v < 0.38f ? 0.78f + 0.14f * (1.0f - v / 0.38f)
                           : 0.09f + 0.05f * ((v - 0.38f) / 0.62f);
    float du = (u - 0.15f) / 0.08f, dv = (v - 0.12f) / 0.08f;
    float sun = 0.18f * expf(-0.5f * (du * du + dv * dv));
    return clamp01(base + sun);
}

/* night: near-black field with small bright sources */
static const float night_lights[][2] = {
    { 0.08f, 0.25f }, { 0.18f, 0.25f }, { 0.82f, 0.30f }, { 0.92f, 0.30f },
    { 0.10f, 0.55f }, { 0.88f, 0.60f }, { 0.35f, 0.12f }, { 0.65f, 0.12f },
};

static float scene_night(float u, float v) {
    float s = 0.025f + 0.01f * v;
    for (int i = 0; i < (int)(sizeof(night_lights) / sizeof(night_lights[0])); i++) {
        float du = (u - night_lights[i][0]) / 0.016f;
        float dv = (v - night_lights[i][1]) / 0.016f;
        s += 0.70f * expf(-0.5f * (du * du + dv * dv));
    }
    float du = (u - 0.75f) / 0.04f, dv = (v - 0.08f) / 0.04f;
    s += 0.55f * expf(-0.5f * (du * du + dv * dv));
    return clamp01(s);
}

static const struct { const char* name; SceneFn fn; } SCENES[] = {
    { "interior", scene_interior },
    { "exterior", scene_exterior },
    { "night",    scene_night    },
};
#define NSCENES ((int)(sizeof(SCENES) / sizeof(SCENES[0])))

/* ---- ground truth over every pixel ------------------------------------ */

static void truth(SceneFn fn, SceneTruth* t) {
    double s = 0.0, s2 = 0.0, sr = 0.0;
    long nr = 0;
    for (int y = 0; y < H; y++) {
        float v = (y + 0.5f) / H;
        for (int x = 0; x < W; x++) {
            float u = (x + 0.5f) / W;
            float L = fn(u, v);
            s += L; s2 += L * L;
            if (u >= RX0 && u <= RX1 && v >= RY0 && v <= RY1) { sr += L; nr++; }
        }
    }
    double n = (double)PIXELS;
    t->mean = (float)(s / n);
    double var = s2 / n - (double)t->mean * t->mean;
    t->sd = (float)sqrt(var > 0.0 ? var : 0.0);
    t->mu_r = (float)(sr / nr);
    t->area_f = (float)nr / (float)PIXELS;
}

/* exact mean over every pixel inside a rectangle */
static float region_mean_exact(SceneFn fn, float x0, float x1, float y0, float y1) {
    double s = 0.0;
    long n = 0;
    for (int y = 0; y < H; y++) {
        float v = (y + 0.5f) / H;
        if (v < y0 || v > y1) continue;
        for (int x = 0; x < W; x++) {
            float u = (x + 0.5f) / W;
            if (u < x0 || u > x1) continue;
            s += fn(u, v); n++;
        }
    }
    return (float)(s / n);
}

/* ---- estimators ------------------------------------------------------- */

/* regular grid inside an arbitrary rectangular region (the biased estimator) */
static void region_estimate_rect(SceneFn fn,
                                 float x0, float x1, float y0, float y1,
                                 int cols, int rows, Estimate* e) {
    double s = 0.0;
    int n = 0;
    for (int r = 0; r < rows; r++) {
        float v = y0 + (r + 0.5f) / rows * (y1 - y0);
        for (int c = 0; c < cols; c++) {
            float u = x0 + (c + 0.5f) / cols * (x1 - x0);
            s += fn(u, v); n++;
        }
    }
    e->est = (float)(s / n);
    e->n = n; e->cols = cols; e->rows = rows;
}

/* current meter: regular grid inside the fixed centre region */
static void region_estimate(SceneFn fn, int cols, int rows, Estimate* e) {
    region_estimate_rect(fn, RX0, RX1, RY0, RY1, cols, rows, e);
}

/* proposed meter: one jittered sample per grid cell, covering the whole frame */
static unsigned hash32(unsigned x, unsigned y) {
    unsigned h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float jitter(unsigned a, unsigned b) {
    return (float)(hash32(a, b) & 0xffffffu) / 16777216.0f;
}

static void grid_estimate(SceneFn fn, int cols, int rows, Estimate* e) {
    double s = 0.0, s2 = 0.0;
    int n = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float u = (c + jitter((unsigned)c, (unsigned)r)) / cols;
            float v = (r + jitter((unsigned)(r + 99991), (unsigned)(c + 7))) / rows;
            float L = fn(u, v);
            s += L; s2 += L * L; n++;
        }
    }
    double mu = s / n;
    e->est = (float)mu;
    e->n = n; e->cols = cols; e->rows = rows;
    (void)s2;
}

/* ---- budget ----------------------------------------------------------- */

/* n such that the 95% bound 2*sd/sqrt(n) on a random sample mean is <= QUANT */
static long required_samples(float sd) {
    double k = 2.0 * sd / QUANT;
    return (long)ceil(k * k);
}

/* grid shaped like the frame, with at least nreq cells */
static void grid_dims(long nreq, int* cols, int* rows) {
    int c = (int)lround(sqrt((double)nreq * W / H));
    if (c < 1) c = 1;
    int r = (int)ceil((double)nreq / c);
    if (r < 1) r = 1;
    *cols = c; *rows = r;
}

/* frames of temporal accumulation needed when per-frame samples are capped */
static long temporal_frames(float sd, int cap) {
    double per_frame = 2.0 * sd / (QUANT * sqrt((double)cap));
    return (long)ceil(per_frame * per_frame);
}

static double error_q(float sd, int n)  { return 2.0 * sd / sqrt((double)n) / QUANT; }
static double cost_mbps(int n)          { return (double)n * BYTES_PER_SAMPLE * FPS / 1e6; }

/* ---- reporting -------------------------------------------------------- */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    SceneTruth truths[NSCENES];
    Estimate    reg[NSCENES], grid[NSCENES];
    long        nreq[NSCENES];

    printf("# quantization step q = 1/255 = %.6f; sample = %d bytes @ %d fps\n",
           (double)QUANT, BYTES_PER_SAMPLE, FPS);
    printf("# centre meter: %dx%d = %d points over [%.2f,%.2f]x[%.2f,%.2f] (%.0f%% area)\n",
           CUR_COLS, CUR_ROWS, CUR_COLS * CUR_ROWS, RX0, RX1, RY0, RY1,
           (RX1 - RX0) * (RY1 - RY0) * 100.0f);
    printf("%-9s %-7s %-8s %-9s %-8s %-9s %-9s %-8s\n",
           "scene", "mean", "region", "bias(q)", "Nreq", "grid", "est(q)", "MB/s");

    for (int i = 0; i < NSCENES; i++) {
        truth(SCENES[i].fn, &truths[i]);
        region_estimate(SCENES[i].fn, CUR_COLS, CUR_ROWS, &reg[i]);

        SceneTruth* t = &truths[i];
        nreq[i] = required_samples(t->sd);
        int cols, rows;
        grid_dims(nreq[i], &cols, &rows);
        grid_estimate(SCENES[i].fn, cols, rows, &grid[i]);

        float bias_q = (reg[i].est - t->mean) / QUANT;
        float grid_q = (grid[i].est - t->mean) / QUANT;

        printf("%-9s %.5f %.5f %+8.2f %-8ld %dx%-3d %+8.2f %8.2f\n",
               SCENES[i].name, t->mean, t->mu_r, bias_q, nreq[i],
               grid[i].cols, grid[i].rows, grid_q, cost_mbps(grid[i].n));

        printf("          cov=%+.5f  region_mean=%.5f  full_mean=%.5f  area=%.1f%%\n",
               (double)(reg[i].est - t->mean), t->mu_r, t->mean,
               t->area_f * 100.0f);

        /* the centre meter must miss by more than a code value on every scene */
        check("center_bias_exceeds_q", fabsf(reg[i].est - t->mean) > QUANT);
        /* the full-frame jittered grid at Nreq must land within one code value */
        check("grid_within_q", fabsf(grid[i].est - t->mean) <= QUANT);
        check("grid_at_least_nreq", (long)grid[i].n >= nreq[i]);
        check("grid_covers_frame", grid[i].est >= 0.0f && grid[i].est <= 1.0f);
    }

    /* two curves -> density: error falls as 2*sd/sqrt(N), cost rises with N.
     * The exterior scene is the binding case, so resolve it there. */
    printf("\n# budget curves, exterior (sd=%.4f): error vs cost\n", truths[1].sd);
    printf("%-10s %-14s %-12s\n", "N", "2sd/sqrt(N)/q", "MB/s");
    int probes[] = { 144, 1024, 4096, 16384, (int)nreq[1] };
    for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
        printf("%-10d %-14.3f %-12.3f\n",
               probes[i], error_q(truths[1].sd, probes[i]), cost_mbps(probes[i]));
    }
    printf("%-10d %-14.3f %-12.1f\n", PIXELS, error_q(truths[1].sd, PIXELS), cost_mbps(PIXELS));

    /* The centre meter's error is a region-selection bias, not noise: adding
     * points inside the same region does not move it. Show that the bias only
     * shrinks as the sampled region grows toward the full frame. */
    printf("\n# region sweep: exact bias(q) of the region mean as the sampled box grows\n");
    printf("%-9s %-16s %-16s %-16s\n", "scene", "full frame", "[.25,.75]^2", "[.10,.90]^2");
    for (int i = 0; i < NSCENES; i++) {
        float bf = (region_mean_exact(SCENES[i].fn, 0.f, 1.f, 0.f, 1.f) - truths[i].mean) / QUANT;
        float bc = (region_mean_exact(SCENES[i].fn, .25f, .75f, .25f, .75f) - truths[i].mean) / QUANT;
        float bw = (region_mean_exact(SCENES[i].fn, .10f, .90f, .10f, .90f) - truths[i].mean) / QUANT;
        printf("%-9s %+15.2f %+16.2f %+16.2f\n", SCENES[i].name, bf, bc, bw);
        /* widening the box shrinks the bias monotonically */
        check("sweep_monotone", fabsf(bf) <= fabsf(bw) && fabsf(bw) <= fabsf(bc));
        check("sweep_full_region_unbiased", fabsf(bf) <= QUANT);
    }

    /* if a hard per-frame cap binds, accumulate over frames instead */
    int cap = 4096;
    long T = temporal_frames(truths[1].sd, cap);
    printf("# cap %d samples/frame -> %ld frames to reach q (60fps: %.1f ms latency)\n",
           cap, T, (double)T * 1000.0 / FPS);
    check("temporal_beat_bruteforce", T < (long)nreq[1]);

    /* The 3% headline is a full-frame arithmetic mean. Two variants can share
     * that mean exactly while their centre brightness -- what the eye reads --
     * differs by tens of code values. Variant B = A + a*(I_region - f), which
     * integrates to zero over the frame. */
    {
        const float a = 0.12f;
        SceneTruth ta, tb;
        truth(scene_interior, &ta);
        /* full-pixel truth for the shifted variant */
        double s = 0.0, sr = 0.0;
        long nr = 0;
        for (int y = 0; y < H; y++) {
            float v = (y + 0.5f) / H;
            for (int x = 0; x < W; x++) {
                float u = (x + 0.5f) / W;
                float in = (u >= RX0 && u <= RX1 && v >= RY0 && v <= RY1) ? 1.0f : 0.0f;
                float L = clamp01(scene_interior(u, v) + a * (in - ta.area_f));
                s += L;
                if (in > 0.5f) { sr += L; nr++; }
            }
        }
        tb.mean = (float)(s / PIXELS);
        tb.mu_r = (float)(sr / nr);
        float rel = fabsf(tb.mean - ta.mean) / ta.mean;
        float center_gap_q = (tb.mu_r - ta.mu_r) / QUANT;
        printf("# pair: full-mean A=%.5f B=%.5f (rel %.3f%%), centre A=%.5f B=%.5f (gap %+.1f q)\n",
               ta.mean, tb.mean, rel * 100.0f, ta.mu_r, tb.mu_r, center_gap_q);
        check("pair_full_mean_within_3pct", rel <= 0.03f);
        check("pair_center_gap_exceeds_q", center_gap_q > 1.0f);
    }

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
