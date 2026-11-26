
/* errchain: chained post-process error accumulation experiment.
 *
 * Models an HDR rainy-night signal flowing through a configurable chain:
 *   exposure -> bright-pass -> Kb box-blur rounds (bloom) -> ACES
 *            -> Kd variable-radius blur rounds (DoF) -> composite add -> SDR Q8
 *
 * A reference chain keeps every intermediate buffer in double precision; a
 * test chain runs the identical operators but stores each intermediate through
 * a quantizer (R8_UNORM / R16F / fp32). End-to-end output error is measured on
 * the final 8-bit codes, in LSB units, against a one-quantum budget Q = 1.
 *
 * Outputs (text to stdout, data + SVG under artifacts/):
 *   - per-step local round-off: RMS relative error, sign counts (pos/neg/zero)
 *   - per-step single-step ablation: output error when only that step quantizes
 *   - N sweep: total error and quality vs chain length, feasibility, knee point
 *   - summary / step / sweep CSV and two self-contained SVG figures
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 48
#define H 48
#define NP (W * H)
#define MAX_K 5
#define MAX_STEPS (5 + 2 * MAX_K) /* exposure, bright, Kb, aces, Kd, add, final */
#define CAP (MAX_STEPS + 1)       /* sentinel slack so runtime s stays in bounds */
#define EPS_FLOOR 1e-3            /* relative-error denominator floor */
#define BUDGET_LSB 1.0            /* one 8-bit quantum on the final output */

/* ---------------- quantizer tiers ---------------- */
enum { TIER_REF = 0, TIER_8 = 8, TIER_16 = 16, TIER_32 = 32 };

/* round a float-like value to mbits mantissa bits (half: 10, fp32: 23) */
static double quant_float(double v, int mbits, double maxval) {
    if (v == 0.0) return 0.0;
    double s = v < 0 ? -1.0 : 1.0;
    double a = fabs(v);
    int e;
    frexp(a, &e);
    if (ldexp(1.0, e) > maxval) return s * maxval; /* emulate narrow range */
    if (e < -14) return 0.0;                        /* flush subnormals */
    double r = ldexp(round(ldexp(a, mbits + 1 - e)), e - (mbits + 1));
    return s * r;
}

/* the store operation of an intermediate buffer */
static double quantize(double v, int tier) {
    switch (tier) {
    case TIER_REF: return v;
    case TIER_8: { /* R8_UNORM: 256 levels, hard clamp to [0,1] */
        if (v <= 0.0) return 0.0;
        if (v >= 1.0) return 1.0;
        return round(v * 255.0) / 255.0;
    }
    case TIER_16: return quant_float(v, 10, 65504.0);
    case TIER_32: return quant_float(v, 23, 3.402823466e38);
    default: return v;
    }
}

/* ---------------- deterministic RNG + scenes ---------------- */
typedef struct { unsigned long s; } Rng;
static double rng_next(Rng *r) { /* (0,1) */
    r->s = r->s * 1103515245UL + 12345UL;
    return (double)((r->s >> 8) & 0xffffff) / (double)0x1000000;
}

typedef struct {
    const char *name;
    double rad[NP];   /* HDR scene radiance (linear luminance) */
    double depth[NP]; /* view depth */
    double exposure;
    double bright_thr;
    double peak;
} Scene;

static void make_scene(Scene *sc, int kind) {
    /* kind: 0 light rain, 1 medium rain, 2 heavy rain */
    static const char *names[3] = { "light_rain", "medium_rain", "heavy_rain" };
    Rng rng;
    rng.s = 0x5151u + (unsigned long)kind * 7919u;
    sc->name = names[kind];
    sc->exposure = 1.0 + 0.25 * kind;
    sc->bright_thr = 0.8;
    double base = 0.35 - 0.085 * kind;
    double amp = 1.2 + 2.6 * kind;
    int streaks = 24 + 36 * kind;
    int neon = kind; /* number of bright neon patches */

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            double n = (rng_next(&rng) - 0.5) * 0.08;
            sc->rad[i] = base + n;
            /* depth: 1..12 gradient with foreground patches */
            double d = 1.0 + 11.0 * ((double)x / (W - 1));
            if (((x / 12) + (y / 12)) % 3 == 0) d *= 0.45;
            sc->depth[i] = d;
        }
    }
    for (int k = 0; k < streaks; k++) {
        int x = (int)(rng_next(&rng) * W);
        int y = (int)(rng_next(&rng) * H);
        int len = 3 + (int)(rng_next(&rng) * 6);
        double v = base + amp * (0.35 + rng_next(&rng) * 0.65);
        for (int t = 0; t < len && y + t < H; t++)
            sc->rad[(y + t) * W + x] += v;
    }
    for (int k = 0; k < neon; k++) {
        int x0 = 4 + (int)(rng_next(&rng) * (W - 12));
        int y0 = 4 + (int)(rng_next(&rng) * (H - 12));
        double v = 6.0 + rng_next(&rng) * 6.0;
        for (int y = y0; y < y0 + 5 && y < H; y++)
            for (int x = x0; x < x0 + 6 && x < W; x++)
                sc->rad[y * W + x] += v;
    }
    double peak = 0.0;
    for (int i = 0; i < NP; i++) if (sc->rad[i] > peak) peak = sc->rad[i];
    sc->peak = peak;
}

/* ---------------- operators (mirrors tonemap/bloom/dof modules) ---------------- */
static double aces_curve(double x) {
    double a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    double r = (x * (a * x + b)) / (x * (c * x + d) + e);
    if (r < 0.0) r = 0.0;
    if (r > 1.0) r = 1.0;
    return r;
}

static void box_blur_2d(const double *src, double *dst, int r) {
    double tmp[NP];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double s = 0.0;
            for (int k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0; else if (xx >= W) xx = W - 1;
                s += src[y * W + xx];
            }
            tmp[y * W + x] = s / (2 * r + 1);
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double s = 0.0;
            for (int k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0; else if (yy >= H) yy = H - 1;
                s += tmp[yy * W + x];
            }
            dst[y * W + x] = s / (2 * r + 1);
        }
    }
}

/* thin-lens CoC -> integer pixel blur radius in [0, maxr], like dof.c */
static int dof_radius(const Scene *sc, int i, double kpx, int maxr) {
    double focus = 5.0, aperture = 2.0, focal = 50.0;
    double d = sc->depth[i];
    double num = aperture * focal * (d - focus);
    double den = d * (focus - focal);
    double coc = fabs(num / den);
    int r = (int)(coc * kpx + 0.5);
    if (r > maxr) r = maxr;
    return r;
}

/* variable-radius box blur driven by the per-pixel CoC radius */
static void dof_blur(const Scene *sc, const double *src, double *dst) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            int r = dof_radius(sc, i, 14.0, 3);
            if (r == 0) { dst[i] = src[i]; continue; }
            double s = 0.0; int cnt = 0;
            for (int yy = y - r; yy <= y + r; yy++) {
                int cy = yy < 0 ? 0 : (yy >= H ? H - 1 : yy);
                for (int xx = x - r; xx <= x + r; xx++) {
                    int cx = xx < 0 ? 0 : (xx >= W ? W - 1 : xx);
                    s += src[cy * W + cx];
                    cnt++;
                }
            }
            dst[i] = s / cnt;
        }
    }
}

/* ---------------- pipeline ---------------- */
typedef struct {
    int kb, kd;            /* blur round counts */
    int tiers[CAP];        /* quantizer per step */
    int nsteps;
    const char *names[CAP];
    double local_rms[CAP]; /* RMS relative local round-off */
    long npos[CAP], nneg[CAP], nzero[CAP];
} PipeCfg;

/* stores buf through step i's quantizer, recording the local round-off */
static double store(PipeCfg *c, int i, double v) {
    double q = quantize(v, c->tiers[i]);
    double d = q - v;
    double denom = fabs(v) < EPS_FLOOR ? EPS_FLOOR : fabs(v);
    c->local_rms[i] += (d / denom) * (d / denom);
    if (d > 0.0) c->npos[i]++;
    else if (d < 0.0) c->nneg[i]++;
    else c->nzero[i]++;
    return q;
}

/* runs the full chain; returns final 8-bit codes (0..255) in out */
static void run_pipe(const Scene *sc, PipeCfg *c, double *out) {
    double a[NP], b[NP], bloom[NP], dofbuf[NP];
    int s = 0;

    if (c->kb < 0 || c->kb > MAX_K || c->kd < 0 || c->kd > MAX_K ||
        5 + c->kb + c->kd > MAX_STEPS) {
        fprintf(stderr, "run_pipe: chain length out of bounds\n");
        exit(2);
    }

    for (int i = 0; i < NP; i++) a[i] = sc->rad[i];

    /* step 1: exposure */
    c->names[s] = "exposure";
    for (int i = 0; i < NP; i++) a[i] = store(c, s, a[i] * sc->exposure);
    s++;

    /* step 2: bright-pass */
    c->names[s] = "bright_pass";
    for (int i = 0; i < NP; i++) {
        double v = a[i] - sc->bright_thr;
        b[i] = store(c, s, v > 0.0 ? v : 0.0);
    }
    s++;

    /* bloom: Kb separable box-blur rounds, radius 1 */
    for (int k = 0; k < c->kb; k++) {
        static const char *bnames[MAX_K] = {
            "bloom_blur1", "bloom_blur2", "bloom_blur3", "bloom_blur4", "bloom_blur5" };
        c->names[s] = bnames[k];
        box_blur_2d(b, bloom, 1);
        for (int i = 0; i < NP; i++) b[i] = store(c, s, bloom[i]);
        s++;
    }

    /* ACES tonemap of the exposed image */
    c->names[s] = "aces";
    for (int i = 0; i < NP; i++) a[i] = store(c, s, aces_curve(a[i]));
    s++;

    /* DoF: Kd variable-radius blur rounds over the tonemapped image */
    for (int k = 0; k < c->kd; k++) {
        static const char *dnames[MAX_K] = {
            "dof_blur1", "dof_blur2", "dof_blur3", "dof_blur4", "dof_blur5" };
        c->names[s] = dnames[k];
        dof_blur(sc, a, dofbuf);
        for (int i = 0; i < NP; i++) a[i] = store(c, s, dofbuf[i]);
        s++;
    }

    /* composite: tonemapped image + bloom contribution */
    c->names[s] = "composite";
    for (int i = 0; i < NP; i++) {
        double v = a[i] + 0.6 * b[i];
        out[i] = store(c, s, v > 1.0 ? 1.0 : v);
    }
    s++;

    /* final SDR encode, always Q8 (tier quantizer is not applied here,
       so it does not double-quantize; measured error is the Q8 rounding) */
    c->names[s] = "sdr_q8";
    double sdr_acc = 0.0;
    for (int i = 0; i < NP; i++) {
        double v = out[i];
        double q = quantize(v, TIER_8);
        out[i] = round(q * 255.0);
        double d = q - v;
        double denom = fabs(v) < EPS_FLOOR ? EPS_FLOOR : fabs(v);
        double rr = d / denom;
        sdr_acc += rr * rr;
        if (d > 0.0) c->npos[s]++; else if (d < 0.0) c->nneg[s]++; else c->nzero[s]++;
    }
    c->local_rms[s] = sqrt(sdr_acc / NP);
    s++;
    c->nsteps = s;
    for (int i = 0; i < s; i++)
        if (i != s - 1) c->local_rms[i] = sqrt(c->local_rms[i] / NP);
}

static void base_cfg(PipeCfg *c, int kb, int kd, int tier) {
    memset(c, 0, sizeof(*c));
    c->kb = kb; c->kd = kd;
    int n = 5 + kb + kd;
    for (int i = 0; i < n; i++) c->tiers[i] = tier;
    c->tiers[n - 1] = TIER_8; /* final SDR encode is always 8-bit */
}

/* ---------------- error metrics ---------------- */
typedef struct {
    double rms_lsb, mean_lsb, max_lsb, mean_signed;
    double rms_rel;
    long opos, oneg, ozero;     /* sign counts of the end-to-end output error */
} Err;

static Err measure(const double *tst, const double *ref) {
    Err e = {0};
    double acc = 0.0, absacc = 0.0, maxv = 0.0, signedacc = 0.0, relacc = 0.0;
    for (int i = 0; i < NP; i++) {
        double d = tst[i] - ref[i];
        acc += d * d;
        absacc += fabs(d);
        signedacc += d;
        if (d > 0.0) e.opos++; else if (d < 0.0) e.oneg++; else e.ozero++;
        if (fabs(d) > maxv) maxv = fabs(d);
        double denom = ref[i] < 1.0 ? 1.0 : ref[i];
        double rr = d / denom;
        relacc += rr * rr;
    }
    e.rms_lsb = sqrt(acc / NP);
    e.mean_lsb = absacc / NP;
    e.max_lsb = maxv;
    e.mean_signed = signedacc / NP;
    e.rms_rel = sqrt(relacc / NP);
    return e;
}

/* quality of the bloom response: closeness to an ideal wide (r=6) blur */
static double bloom_quality(const Scene *sc, int kb) {
    double bright[NP], cur[NP], ideal[NP], tmp[NP];
    for (int i = 0; i < NP; i++) {
        double v = sc->rad[i] * sc->exposure - sc->bright_thr;
        bright[i] = v > 0 ? v : 0;
    }
    memcpy(cur, bright, sizeof(cur));
    for (int k = 0; k < kb; k++) { box_blur_2d(cur, tmp, 1); memcpy(cur, tmp, sizeof(cur)); }
    box_blur_2d(bright, ideal, 6);
    double mse_k = 0.0, mse_0 = 0.0;
    for (int i = 0; i < NP; i++) {
        double dk = cur[i] - ideal[i], d0 = bright[i] - ideal[i];
        mse_k += dk * dk; mse_0 += d0 * d0;
    }
    double q = 100.0 * (1.0 - mse_k / mse_0);
    return q < 0 ? 0 : q;
}

/* ---------------- data capture for reports ---------------- */
typedef struct {
    int n;
    double E_lsb, E_rel, quality;
    int pass;
} SweepRow;

typedef struct {
    int idx;
    char name[16];
    double local_rms;
    long pos, neg, zero;
    double pos_frac;          /* pos / (pos+neg), NaN if no nonzero errors */
    double ablation_lsb;      /* output RMS error with only this step quantized */
    double ablation_signed;   /* mean signed output shift of the ablation */
} StepRow;

static void analyze_scenario(const Scene *sc, int tier, SweepRow *sw, int *nsw,
                             StepRow *rows, int *nrows, Err *full_err, int kb_nom) {
    double ref[NP], tst[NP], abl[NP];
    PipeCfg c;

    /* reference chain: no intermediate quantization */
    base_cfg(&c, kb_nom, kb_nom, TIER_REF);
    run_pipe(sc, &c, ref);

    /* full tier chain at the nominal length for the step table */
    base_cfg(&c, kb_nom, kb_nom, tier);
    run_pipe(sc, &c, tst);
    *full_err = measure(tst, ref);

    *nrows = c.nsteps;
    for (int i = 0; i < c.nsteps; i++) {
        StepRow *r = &rows[i];
        r->idx = i;
        snprintf(r->name, sizeof(r->name), "%s", c.names[i]);
        r->local_rms = c.local_rms[i];
        r->pos = c.npos[i]; r->neg = c.nneg[i]; r->zero = c.nzero[i];
        long nnz = r->pos + r->neg;
        r->pos_frac = nnz ? (double)r->pos / nnz : 0.5;

        /* single-step ablation: only step i stores through the tier quantizer */
        PipeCfg ca;
        base_cfg(&ca, kb_nom, kb_nom, TIER_REF);
        ca.tiers[i] = (i == c.nsteps - 1) ? TIER_8 : tier;
        run_pipe(sc, &ca, abl);
        Err ea = measure(abl, ref);
        r->ablation_lsb = ea.rms_lsb;
        r->ablation_signed = ea.mean_signed;
    }

    /* N sweep: kb=kd=k, N = 5 + 2k */
    *nsw = 0;
    for (int k = 1; k <= MAX_K; k++) {
        int n = 5 + 2 * k;
        double rk[NP], tk[NP];
        PipeCfg cr, ct;
        base_cfg(&cr, k, k, TIER_REF);
        run_pipe(sc, &cr, rk);
        base_cfg(&ct, k, k, tier);
        run_pipe(sc, &ct, tk);
        Err e = measure(tk, rk);
        sw[*nsw].n = n;
        sw[*nsw].E_lsb = e.rms_lsb;
        sw[*nsw].E_rel = e.rms_rel;
        sw[*nsw].quality = bloom_quality(sc, k);
        sw[*nsw].pass = e.rms_lsb <= BUDGET_LSB;
        (*nsw)++;
    }
}

/* Knee point from the two-curve trade-off.
 * Restrict to feasible points (error curve under budget Q), then take the
 * smallest N on the quality curve at which either:
 *   - quality has reached 90% of its plateau (the best quality in the sweep), or
 *   - the marginal quality gain of another 2 steps drops below 1 pt/step.
 * If no feasible point exists, return -1 (no usable chain length at this tier). */
static int pick_knee(const SweepRow *sw, int nsw) {
    double qmax = 0.0;
    for (int i = 0; i < nsw; i++) if (sw[i].pass && sw[i].quality > qmax) qmax = sw[i].quality;
    for (int i = 0; i < nsw; i++) {
        if (!sw[i].pass) continue;
        if (sw[i].quality >= 0.9 * qmax) return sw[i].n;
        if (i + 1 < nsw && sw[i + 1].pass) {
            double dq = sw[i + 1].quality - sw[i].quality;
            double dn = sw[i + 1].n - sw[i].n;
            if (dq / dn < 1.0) return sw[i].n;
        }
    }
    return -1;
}

/* ---------------- SVG figures ---------------- */
#define INK "#0b0b0b"
#define INK2 "#52514e"
#define MUTED "#898781"
#define GRID "#e1e0d9"
#define SURF "#fcfcfb"
#define BLUE "#2a78d6"
#define ORANGE "#eb6834"
#define AQUA "#1baf7a"
#define RED "#d03b3b"

static void svg_text(FILE *f, double x, double y, const char *anchor,
                     const char *fill, int size, const char *txt) {
    fprintf(f, "<text x=\"%.1f\" y=\"%.1f\" text-anchor=\"%s\" fill=\"%s\" "
            "font-family=\"system-ui,sans-serif\" font-size=\"%d\">%s</text>\n",
            x, y, anchor, fill, size, txt);
}

static void svg_axes(FILE *f, double x0, double y0, double pw, double ph,
                     double ymax, int ny, const char *ytitle) {
    fprintf(f, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%s\"/>\n",
            x0, y0, pw, ph, SURF);
    for (int t = 0; t <= ny; t++) {
        double yy = y0 + ph - ph * t / ny;
        fprintf(f, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1\"/>\n",
                x0, yy, x0 + pw, yy, GRID);
        char lab[32]; snprintf(lab, sizeof(lab), "%.2g", ymax * t / ny);
        svg_text(f, x0 - 6, yy + 4, "end", MUTED, 11, lab);
    }
    fprintf(f, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\"/>\n",
            x0, y0 + ph, x0 + pw, y0 + ph, "#c3c2b7");
    svg_text(f, x0 - 44, y0 + ph / 2, "middle", INK2, 12, ytitle);
}

static void write_sweep_svg(const char *path,
                            SweepRow sweeps[3][3][MAX_K], const char *scenes[3]) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    double x0 = 90, colw = 420, gapx = 60, rowh = 190, gapy = 70, top = 50;
    double width = x0 + 2 * colw + gapx + 40;
    double height = top + 3 * (rowh + gapy) + 30;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" "
            "viewBox=\"0 0 %.0f %.0f\" font-family=\"system-ui,sans-serif\">\n",
            width, height, width, height);
    fprintf(f, "<rect width=\"100%%\" height=\"100%%\" fill=\"#f9f9f7\"/>\n");
    svg_text(f, width / 2, 26, "middle", INK, 18,
             "Chained post-process: total error and quality vs chain length N");
    const char *tier_col[3] = { BLUE, ORANGE, AQUA };

    for (int sc = 0; sc < 3; sc++) {
        double yrow = top + sc * (rowh + gapy);
        /* left panel: error vs N */
        double lx = x0;
        double emax = BUDGET_LSB;
        for (int t = 0; t < 3; t++)
            for (int k = 0; k < MAX_K; k++)
                if (sweeps[sc][t][k].E_lsb > emax) emax = sweeps[sc][t][k].E_lsb;
        emax *= 1.1;
        svg_axes(f, lx, yrow, colw, rowh, emax, 4, "RMS error (LSB)");
        /* budget line */
        double yb = yrow + rowh - rowh * BUDGET_LSB / emax;
        fprintf(f, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" "
                "stroke-width=\"1.5\" stroke-dasharray=\"6 4\"/>\n", lx, yb, lx + colw, yb, RED);
        svg_text(f, lx + colw - 4, yb - 5, "end", RED, 11, "budget Q = 1 LSB");
        int nmin = sweeps[sc][0][0].n, nmax = sweeps[sc][0][MAX_K - 1].n;
        for (int t = 0; t < 3; t++) {
            fprintf(f, "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"2\" points=\"",
                    tier_col[t]);
            for (int k = 0; k < MAX_K; k++) {
                SweepRow *r = &sweeps[sc][t][k];
                double px = lx + colw * (r->n - nmin) / (nmax - nmin);
                double py = yrow + rowh - rowh * r->E_lsb / emax;
                fprintf(f, "%.1f,%.1f ", px, py);
            }
            fprintf(f, "\"/>\n");
            for (int k = 0; k < MAX_K; k++) {
                SweepRow *r = &sweeps[sc][t][k];
                double px = lx + colw * (r->n - nmin) / (nmax - nmin);
                double py = yrow + rowh - rowh * r->E_lsb / emax;
                fprintf(f, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"3.5\" fill=\"%s\"/>\n",
                        px, py, tier_col[t]);
            }
        }
        for (int k = 0; k < MAX_K; k++) {
            double px = lx + colw * (sweeps[sc][0][k].n - nmin) / (nmax - nmin);
            char lab[8]; snprintf(lab, sizeof(lab), "%d", sweeps[sc][0][k].n);
            svg_text(f, px, yrow + rowh + 18, "middle", MUTED, 11, lab);
        }
        svg_text(f, lx + colw / 2, yrow - 12, "middle", INK, 13, scenes[sc]);
        svg_text(f, lx + colw / 2, yrow + rowh + 36, "middle", INK2, 12, "chain length N (steps)");

        /* right panel: quality vs N (tier independent, reference chain) */
        double rx = x0 + colw + gapx;
        svg_axes(f, rx, yrow, colw, rowh, 100, 4, "bloom quality (0-100)");
        fprintf(f, "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"2\" points=\"", INK2);
        for (int k = 0; k < MAX_K; k++) {
            SweepRow *r = &sweeps[sc][0][k];
            double px = rx + colw * (r->n - nmin) / (nmax - nmin);
            double py = yrow + rowh - rowh * r->quality / 100.0;
            fprintf(f, "%.1f,%.1f ", px, py);
        }
        fprintf(f, "\"/>\n");
        for (int k = 0; k < MAX_K; k++) {
            SweepRow *r = &sweeps[sc][0][k];
            double px = rx + colw * (r->n - nmin) / (nmax - nmin);
            double py = yrow + rowh - rowh * r->quality / 100.0;
            fprintf(f, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"3.5\" fill=\"%s\"/>\n", px, py, INK2);
            char lab[8]; snprintf(lab, sizeof(lab), "%d", r->n);
            svg_text(f, px, yrow + rowh + 18, "middle", MUTED, 11, lab);
        }
        svg_text(f, rx + colw / 2, yrow - 12, "middle", INK, 13, scenes[sc]);
        svg_text(f, rx + colw / 2, yrow + rowh + 36, "middle", INK2, 12, "chain length N (steps)");
    }
    /* legend */
    double lgy = height - 12;
    const char *ln[3] = { "R8_UNORM intermediates", "R16F intermediates", "fp32 intermediates" };
    double lx = x0;
    for (int t = 0; t < 3; t++) {
        fprintf(f, "<line x1=\"%.1f\" y1=\"%.0f\" x2=\"%.1f\" y2=\"%.0f\" stroke=\"%s\" stroke-width=\"2\"/>\n",
                lx, lgy, lx + 22, lgy, tier_col[t]);
        svg_text(f, lx + 28, lgy + 4, "start", INK2, 12, ln[t]);
        lx += 200;
    }
    fprintf(f, "</svg>\n");
    fclose(f);
}

static void write_breakdown_svg(const char *path, const StepRow *rows, int n,
                                const char *scene, const char *tier) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    double rowh = 26, top = 90, x0 = 200, halfw = 260;
    double height = top + n * rowh + 70;
    double width = x0 + halfw * 2 + 60;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" "
            "viewBox=\"0 0 %.0f %.0f\" font-family=\"system-ui,sans-serif\">\n",
            width, height, width, height);
    fprintf(f, "<rect width=\"100%%\" height=\"100%%\" fill=\"#f9f9f7\"/>\n");
    char title[160];
    snprintf(title, sizeof(title), "Per-step error contribution (single-step ablation): %s / %s",
             scene, tier);
    svg_text(f, width / 2, 28, "middle", INK, 17, title);
    svg_text(f, width / 2, 49, "middle", INK2, 12,
             "bar length = output RMS error (LSB) when only that step quantizes; color = majority sign of its local round-off");
    svg_text(f, width / 2, 67, "middle", MUTED, 11,
             "note: color is the in-chain local sign; the bar's output shift can point the other way when the step clamps HDR alone (e.g. bright_pass)");

    double vmax = 0;
    for (int i = 0; i < n; i++) if (rows[i].ablation_lsb > vmax) vmax = rows[i].ablation_lsb;
    if (vmax == 0) vmax = 1;
    double zero_x = x0 + halfw;
    fprintf(f, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#c3c2b7\"/>\n",
            zero_x, top - 10, zero_x, top + n * rowh + 6);
    for (int i = 0; i < n; i++) {
        const StepRow *r = &rows[i];
        double y = top + i * rowh;
        double len = halfw * r->ablation_lsb / vmax;
        /* majority sign: positive fraction >= 0.6 -> blue, <= 0.4 -> red, else gray */
        const char *col = MUTED;
        if (r->pos + r->neg > 0) {
            if (r->pos_frac >= 0.6) col = BLUE;
            else if (r->pos_frac <= 0.4) col = RED;
        }
        fprintf(f, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"14\" rx=\"4\" fill=\"%s\"/>\n",
                zero_x, y + 4, len, col);
        svg_text(f, x0 - 10, y + 16, "end", INK, 12, r->name);
        char vlab[64];
        snprintf(vlab, sizeof(vlab), "%.4f LSB  (+:%ld -:%ld 0:%ld)",
                 r->ablation_lsb, r->pos, r->neg, r->zero);
        svg_text(f, zero_x + len + 6, y + 16, "start", INK2, 11, vlab);
    }
    double lgy = top + n * rowh + 34;
    fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"14\" height=\"14\" rx=\"4\" fill=\"%s\"/>\n", x0, lgy - 11, BLUE);
    svg_text(f, x0 + 20, lgy, "start", INK2, 12, "majority positive round-off (>=60%)");
    fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"14\" height=\"14\" rx=\"4\" fill=\"%s\"/>\n", x0 + 260, lgy - 11, RED);
    svg_text(f, x0 + 280, lgy, "start", INK2, 12, "majority negative round-off (<=40%)");
    fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"14\" height=\"14\" rx=\"4\" fill=\"%s\"/>\n", x0 + 520, lgy - 11, MUTED);
    svg_text(f, x0 + 540, lgy, "start", INK2, 12, "balanced / no local error");
    fprintf(f, "</svg>\n");
    fclose(f);
}

/* ---------------- main ---------------- */
int main(void) {
    const char *scene_names[3] = { "light_rain", "medium_rain", "heavy_rain" };
    const char *tier_names[3] = { "R8_UNORM", "R16F", "fp32" };
    const int tiers[3] = { TIER_8, TIER_16, TIER_32 };
    const int KB_NOM = 3; /* nominal chain: N = 5 + 2*3 = 11 */

    Scene scenes[3];
    for (int s = 0; s < 3; s++) make_scene(&scenes[s], s);

    SweepRow sweeps[3][3][MAX_K];
    FILE *fsum = fopen("artifacts/sweep.csv", "w");
    FILE *fstep = fopen("artifacts/step_metrics.csv", "w");
    FILE *fver = fopen("artifacts/verdict.csv", "w");
    fprintf(fsum, "scenario,tier,N,rms_error_lsb,rms_relative_error,bloom_quality,passes_Q_budget\n");
    fprintf(fver, "scenario,tier,N,rms_error_lsb,budget_lsb,passes,output_neg_fraction,"
                  "coherence_absmean_over_rms,dominant_step,dominant_ablation_lsb,knee_N\n");
    fprintf(fstep, "scenario,tier,step_idx,step_name,local_rms_relative,pos,neg,zero,"
                   "pos_fraction_nonzero,ablation_rms_lsb,ablation_mean_signed_lsb\n");

    printf("================ errchain: chained post-process error experiment ================\n");
    printf("grid %dx%d = %d px | nominal chain N=%d (kb=kd=%d) | budget Q=%.1f LSB (one 8-bit quantum)\n\n",
           W, H, NP, 5 + 2 * KB_NOM, KB_NOM, BUDGET_LSB);

    int N_nom = 5 + 2 * KB_NOM;
    printf("--- per-step budget solved from total <= Q = %.1f LSB (N=%d) ---\n", BUDGET_LSB, N_nom);
    printf("same-direction (linear):  E = sum e_i  -> cap e_i <= Q/N            = %.4f LSB per step\n",
           BUDGET_LSB / N_nom);
    printf("cancelling (RSS, signs mix): E = sqrt(sum e_i^2) -> cap e_i <= Q/sqrt(N) = %.4f LSB per step\n",
           BUDGET_LSB / sqrt((double)N_nom));
    printf("a chain is feasible only when every step's output contribution stays under the matching cap.\n\n");

    for (int si = 0; si < 3; si++) {
        const Scene *sc = &scenes[si];
        printf("############################################################\n");
        printf("scenario %s: peak radiance %.2f, exposure %.2f\n\n", sc->name, sc->peak, sc->exposure);

        for (int ti = 0; ti < 3; ti++) {
            SweepRow sw[MAX_K]; StepRow rows[MAX_STEPS]; Err fe;
            int nsw, nrows;
            analyze_scenario(sc, tiers[ti], sw, &nsw, rows, &nrows, &fe, KB_NOM);
            memcpy(sweeps[si][ti], sw, sizeof(sw));

            printf("---------- tier %s ----------\n", tier_names[ti]);
            printf("end-to-end at N=%d: RMS=%.4f LSB  mean=%.4f  max=%.4f  signed-mean=%+.4f  "
                   "rms-rel=%.2e  => %s (Q=%.0f)\n",
                   5 + 2 * KB_NOM, fe.rms_lsb, fe.mean_lsb, fe.max_lsb, fe.mean_signed,
                   fe.rms_rel, fe.rms_lsb <= BUDGET_LSB ? "PASS" : "FAIL", BUDGET_LSB);
            printf("%-14s %12s %8s %8s %8s %9s %12s %12s\n",
                   "step", "local_rms", "pos", "neg", "zero", "pos_frac", "abl_LSB", "abl_signed");
            double sum_abs = 0.0, sum_sq = 0.0; int dom = 0;
            for (int i = 0; i < nrows; i++) {
                StepRow *r = &rows[i];
                printf("%-14s %12.2e %8ld %8ld %8ld %9.3f %12.4f %+12.4f\n",
                       r->name, r->local_rms, r->pos, r->neg, r->zero,
                       (r->pos + r->neg) ? r->pos_frac : 0.5,
                       r->ablation_lsb, r->ablation_signed);
                fprintf(fstep, "%s,%s,%d,%s,%.6e,%ld,%ld,%ld,%.4f,%.6f,%+.6f\n",
                        sc->name, tier_names[ti], r->idx, r->name, r->local_rms,
                        r->pos, r->neg, r->zero,
                        (r->pos + r->neg) ? r->pos_frac : 0.5,
                        r->ablation_lsb, r->ablation_signed);
                sum_abs += r->ablation_lsb;
                sum_sq += r->ablation_lsb * r->ablation_lsb;
                if (r->ablation_lsb > rows[dom].ablation_lsb) dom = i;
            }
            printf("model check: measured E=%.4f | linear sum |E_i|=%.4f (ratio %.2f) | "
                   "RSS sqrt(sum E_i^2)=%.4f (ratio %.2f)\n",
                   fe.rms_lsb, sum_abs, fe.rms_lsb ? sum_abs / fe.rms_lsb : 0,
                   sqrt(sum_sq), fe.rms_lsb ? sqrt(sum_sq) / fe.rms_lsb : 0);
            printf("dominant step: %s (ablation %.4f LSB)\n", rows[dom].name, rows[dom].ablation_lsb);
            printf("budget check: dominant-step %.4f LSB vs linear cap Q/N=%.4f -> %s\n",
                   rows[dom].ablation_lsb, BUDGET_LSB / nrows,
                   rows[dom].ablation_lsb <= BUDGET_LSB / nrows ? "within cap" : "OVER CAP");

            /* same-direction test, two complementary statistics.
             * PRIMARY: sign of the end-to-end per-pixel output error. If one
             * sign holds >=60% of nonzero pixels, the accumulated error is
             * same-direction (linear accumulation); near 50/50 means errors
             * partially cancel (RSS-like). This is magnitude-aware in effect
             * because a dominant biased step flips most pixel signs.
             * SECONDARY: raw sign count of every local store round-off, shown
             * for completeness; zero-mean rounding noise can make it read ~50/50
             * even when the output is strongly biased, so it is not the verdict. */
            long onz = fe.opos + fe.oneg;
            double nzfrac = (double)onz / NP;            /* fraction of pixels that actually changed */
            double negfrac = onz ? (double)fe.oneg / onz : 0.5;
            const char *odir;
            if (nzfrac < 0.05)
                odir = "negligible: <5% pixels differ (sub-quantum, effectively no error)";
            else if (negfrac >= 0.6)
                odir = "SAME-DIRECTION, net negative (linear accumulation)";
            else if (negfrac <= 0.4)
                odir = "SAME-DIRECTION, net positive (linear accumulation)";
            else
                odir = "mixed output signs (partial cancellation, RSS-like)";
            printf("output sign test: neg=%ld pos=%ld zero=%ld | nonzero=%.3f neg/nonzero=%.3f => %s\n",
                   fe.oneg, fe.opos, fe.ozero, nzfrac, negfrac, odir);

            long gp = 0, gn = 0, gz = 0;
            for (int i = 0; i < nrows; i++) { gp += rows[i].pos; gn += rows[i].neg; gz += rows[i].zero; }
            long gnz = gp + gn;
            double gpf = gnz ? (double)gp / gnz : 0.5;
            printf("local store signs (secondary): pos=%ld neg=%ld zero=%ld | pos/nonzero=%.3f\n",
                   gp, gn, gz, gpf);
            printf("coherence: |signed mean|/RMS = %.2f (near 1 = errors point one way, "
                   "near 0 = they cancel)\n\n",
                   fe.rms_lsb ? fabs(fe.mean_signed) / fe.rms_lsb : 0.0);

            int knee = pick_knee(sw, nsw);
            printf("N sweep:");
            for (int k = 0; k < nsw; k++) {
                fprintf(fsum, "%s,%s,%d,%.6f,%.6e,%.3f,%d\n", sc->name, tier_names[ti],
                        sw[k].n, sw[k].E_lsb, sw[k].E_rel, sw[k].quality, sw[k].pass);
                printf(" N%d[%.3f%s,q%.0f]", sw[k].n, sw[k].E_lsb, sw[k].pass ? "ok" : "X",
                       sw[k].quality);
            }
            if (knee < 0)
                printf("\nselected knee N* = none (no feasible N at this tier)\n\n");
            else
                printf("\nselected knee N* = %d\n\n", knee);

            fprintf(fver, "%s,%s,%d,%.6f,%.1f,%d,%.4f,%.4f,%s,%.6f,%d\n",
                    sc->name, tier_names[ti], 5 + 2 * KB_NOM, fe.rms_lsb, BUDGET_LSB,
                    fe.rms_lsb <= BUDGET_LSB, negfrac,
                    fe.rms_lsb ? fabs(fe.mean_signed) / fe.rms_lsb : 0.0,
                    rows[dom].name, rows[dom].ablation_lsb, knee);
        }
    }
    fclose(fsum);
    fclose(fstep);
    fclose(fver);

    write_sweep_svg("artifacts/sweep_curves.svg", sweeps, scene_names);

    /* breakdown figure for the failing heavy-rain / R8 case */
    {
        StepRow rows[MAX_STEPS]; SweepRow sw[MAX_K]; Err fe; int nsw, nrows;
        analyze_scenario(&scenes[2], TIER_8, sw, &nsw, rows, &nrows, &fe, KB_NOM);
        write_breakdown_svg("artifacts/error_breakdown_heavy_R8.svg", rows, nrows,
                            "heavy_rain", "R8_UNORM");
    }

    printf("wrote artifacts/sweep.csv, artifacts/step_metrics.csv,\n");
    printf("      artifacts/sweep_curves.svg, artifacts/error_breakdown_heavy_R8.svg\n");
    return 0;
}
