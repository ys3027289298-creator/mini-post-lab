
/* bloomcast: per-channel bloom attenuation, chromatic cast from
   brightness-weighted kernel widths, and JND-based width bound.

   Model (see CAST.md for the full derivation):
   - bright-pass is taken per linear-HDR channel;
   - each channel is blurred with its own kernel width w_c, where a width
     weighted by per-channel brightness is equivalent to stretching the
     kernel per channel: w_c = w0 * (E_c / E0)^gamma, E_c the post-threshold
     channel energy, E0 the mean channel energy;
   - halo color cast is cast_c = H_c / mean(H) - 1, measured on the OUTER
     ring (d=2) where kernel-tail attenuation dominates. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define N 16
#define NN (N * N)

#define THRESH 0.15f   /* bright-pass threshold in linear radiance */
#define W0 5.0f        /* baseline kernel width (pixels), radius 2 */
#define WMIN 1.0f
#define WMAX 9.0f
#define HALO_D 2       /* outer halo ring, in Chebyshev distance */
#define EPS 0.05f      /* just-noticeable cast threshold */

enum { R = 0, G = 1, B = 2, CH = 3 };

/* measured spectral fall-off of a white-hot lamp whose core is clipped to
   pure white: outer fall-off keeps slightly more R/G than B energy */
static const float SPECT[CH] = { 1.000f, 0.985f, 0.910f };
static const float LEVELS[3] = { 2.0f, 8.0f, 32.0f };
static const char* TIER_NAME[3] = { "low", "mid", "high" };

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* per-channel bright pass: subtract threshold, clamp at zero */
static void bright_pass_ch(const float* src, float* dst, float t) {
    for (int i = 0; i < NN; i++) {
        float v = src[i] - t;
        dst[i] = v > 0.0f ? v : 0.0f;
    }
}

/* separable triangular (tent) blur, integer radius, zero-padded border.
   The tent kernel is what two stacked box passes converge to and is the
   standard bloom approximation to a Gaussian. With radius r the 1-D weights
   are (2r+1-|j|)/Z, Z = (2r+1)^2 - r(r+1) = sum of the raw weights
   (r=2: 5+4+4+3+3+2+2+1+1 = 19), so the kernel is exactly normalized.
   Out-of-range taps contribute zero: interior pixels carry the exact
   analytic kernel and a source r from the border keeps all of its energy. */
static void tent_blur_r(const float* src, float* dst, int r) {
    float tmp[NN];
    int w = 2 * r + 1;
    float z = (float)(w * w - r * (r + 1));
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float s = 0.0f;
            for (int k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx < 0 || xx >= N) continue;
                s += (float)(w - abs(k)) * src[y * N + xx];
            }
            tmp[y * N + x] = s / z;
        }
    }
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float s = 0.0f;
            for (int k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy < 0 || yy >= N) continue;
                s += (float)(w - abs(k)) * tmp[yy * N + x];
            }
            dst[y * N + x] = s / z;
        }
    }
}

/* fractional-width tent blur: interpolate between the two bracketing integer
   radii so width can be treated as a continuous knob. width w corresponds to
   radius (w-1)/2; integer widths land exactly on their normalized kernel. */
static void tent_blur_w(const float* src, float* dst, float w,
                        float* b0, float* b1) {
    if (w < WMIN) w = WMIN;
    if (w > WMAX) w = WMAX;
    int r0 = (int)floorf((w - 1.0f) * 0.5f);
    float frac = (w - 1.0f) * 0.5f - (float)r0;
    tent_blur_r(src, b0, r0);
    if (frac > 0.0f) tent_blur_r(src, b1, r0 + 1);
    else memcpy(b1, b0, sizeof(float) * NN);
    for (int i = 0; i < NN; i++)
        dst[i] = (1.0f - frac) * b0[i] + frac * b1[i];
}

static float arr_sum(const float* a) {
    float s = 0.0f;
    for (int i = 0; i < NN; i++) s += a[i];
    return s;
}

/* clipped white-core street lamp on a dark tile:
   d=0 core is pure white (equal channels); d=1,2 fall-off carries the
   spectral imbalance of the source; the rest is a flat 0.02 ambient */
static void build_tile(float tile[CH][NN], float L) {
    const int c = N / 2;
    for (int ch = 0; ch < CH; ch++)
        for (int i = 0; i < NN; i++) tile[ch][i] = 0.02f;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int d = (int)fmaxf(fabsf((float)(x - c)), fabsf((float)(y - c)));
            float v;
            if (d == 0) v = L;                       /* white-hot clipped core */
            else if (d == 1) v = 0.30f * L;
            else if (d == 2) v = 0.06f * L;
            else v = 0.02f;
            for (int ch = 0; ch < CH; ch++)
                tile[ch][y * N + x] = d == 0 ? v : v * SPECT[ch];
        }
    }
}

/* mean of the ring at Chebyshev distance d around the lamp core */
static float ring_mean(const float* img, int d) {
    const int c = N / 2;
    float s = 0.0f;
    int count = 0;
    for (int dy = -d; dy <= d; dy++)
        for (int dx = -d; dx <= d; dx++) {
            int cd = (int)fmaxf(abs(dx), abs(dy));
            if (cd != d) continue;
            s += img[(c + dy) * N + (c + dx)];
            count++;
        }
    return s / (float)count;
}

typedef struct {
    float energy[CH];
    float width[CH];
    float halo[CH];
    float cast[CH];
    float spread;   /* (wmax - wmin) / mean width: highlight-layering knob */
    int pass;
} Measure;

static void measure_tile(const float tile[CH][NN], float gamma, Measure* m) {
    float bp[CH][NN], out[CH][NN];
    static float b0[NN], b1[NN];
    float e0 = 0.0f;
    for (int ch = 0; ch < CH; ch++) {
        bright_pass_ch(tile[ch], bp[ch], THRESH);
        m->energy[ch] = arr_sum(bp[ch]);
        e0 += m->energy[ch];
    }
    e0 /= (float)CH;

    float wmean = 0.0f, wmin = 1e9f, wmax = 0.0f, hmean = 0.0f;
    for (int ch = 0; ch < CH; ch++) {
        float w = W0 * powf(m->energy[ch] / e0, gamma);
        if (w < WMIN) w = WMIN;
        if (w > WMAX) w = WMAX;
        m->width[ch] = w;
        wmean += w;
        wmin = fminf(wmin, w);
        wmax = fmaxf(wmax, w);
        tent_blur_w(bp[ch], out[ch], w, b0, b1);
        m->halo[ch] = ring_mean(out[ch], HALO_D);
        hmean += m->halo[ch];
    }
    wmean /= (float)CH;
    hmean /= (float)CH;
    m->spread = (wmax - wmin) / wmean;

    float worst = 0.0f;
    for (int ch = 0; ch < CH; ch++) {
        m->cast[ch] = m->halo[ch] / hmean - 1.0f;
        worst = fmaxf(worst, fabsf(m->cast[ch]));
    }
    m->pass = worst <= EPS;
}

/* ---- analytic kernel law ------------------------------------------------ */

/* normalized 1-D tent weight at integer offset a for width w = 2r+1:
   raw weight (w-|a|), continuous normalization Z(w) = (3w^2+1)/4, which
   equals the exact integer-width constant w^2-r(r+1) at every integer w.
   Peak scaling k(0;w) = w/Z ~ 4/(3w), and the 2-D peak ~ 16/(9w^2). */
static float k1d(int a, float w) {
    float z = (3.0f * w * w + 1.0f) * 0.25f;
    float v = (w - fabsf((float)a)) / z;
    return v > 0.0f ? v : 0.0f;
}

/* mean 2-D separable weight on the ring at Chebyshev distance d of a unit
   spike: d=0 is the single center pixel; otherwise 4 corner pixels at
   offsets (d,d), and on each of the 4 sides 2d-1 edge pixels at (d,j) */
static float ring_analytic(float w, int d) {
    if (d == 0) return k1d(0, w) * k1d(0, w);
    float side = k1d(0, w);
    for (int j = 1; j < d; j++) side += 2.0f * k1d(j, w);
    float sum = 4.0f * k1d(d, w) * k1d(d, w)
              + 4.0f * k1d(d, w) * side;
    return sum / (float)(8 * d);
}

/* largest symmetric relative width spread whose cast stays within EPS for an
   equal-energy white spike; exact kernel values, solved by bisection */
static float jnd_spread_numeric(int d) {
    float lo = 0.0f, hi = 0.999f;
    for (int it = 0; it < 60; it++) {
        float dd = 0.5f * (lo + hi);
        float fR = ring_analytic(W0 * (1.0f + dd), d);
        float fG = ring_analytic(W0, d);
        float fB = ring_analytic(W0 * (1.0f - dd), d);
        float fm = (fR + fG + fB) / 3.0f;
        float worst = fmaxf(fabsf(fR / fm - 1.0f), fabsf(fB / fm - 1.0f));
        if (worst <= EPS) lo = dd;
        else hi = dd;
    }
    return lo;
}

int main(void) {
    /* ---- unit checks against the analytic kernel law ----
       a lone spike far from every clamped edge reproduces the exact law */
    float spike[NN] = { 0 }, so[NN];
    static float sb0[NN], sb1[NN];
    spike[(N / 2) * N + (N / 2)] = 1.0f;
    tent_blur_w(spike, so, W0, sb0, sb1);
    /* normalization check on a flat field far from the border: an exactly
       normalized kernel must leave every interior pixel unchanged */
    float flat[NN], fo[NN];
    for (int i = 0; i < NN; i++) flat[i] = 1.0f;
    static float fb0[NN], fb1[NN];
    tent_blur_w(flat, fo, W0, fb0, fb1);
    check("blur_flat_preserved",
          fabsf(fo[(N / 2) * N + (N / 2)] - 1.0f) < 1e-5f &&
          fabsf(fo[(N / 2 - 1) * N + (N / 2 + 1)] - 1.0f) < 1e-5f);
    check("blur_zero_outside_support",
          fabsf(so[(N / 2 + 3) * N + (N / 2 + 3)]) < 1e-6f);
    check("spike_center_matches_law",
          fabsf(so[(N / 2) * N + (N / 2)] -
                k1d(0, W0) * k1d(0, W0)) < 1e-5f);
    check("spike_ring_matches_law",
          fabsf(ring_mean(so, HALO_D) - ring_analytic(W0, HALO_D)) < 1e-5f);

    /* closed-form bound from the 1/w^2 2-D peak law at the core (d=0):
       cast = (1+d)^-2 - 1 = -EPS  =>  d = EPS / (2 - EPS).
       the outer ring mixes several kernel offsets, so its bound is looser;
       the peak (tightest) bound is the design limit */
    float d_closed = EPS / (2.0f - EPS);
    float d_peak = jnd_spread_numeric(0);
    float d_ring = jnd_spread_numeric(HALO_D);
    check("jnd_peak_matches_closed_form",
          fabsf(d_peak - d_closed) / d_closed < 0.05f);
    check("jnd_peak_tighter_than_ring", d_peak < d_ring);
    printf("jnd_rel_spread_closed=%.6f\n", d_closed);
    printf("jnd_rel_spread_peak=%.6f\n", d_peak);
    printf("jnd_rel_spread_ring=%.6f\n", d_ring);
    printf("jnd_max_width_diff_w0_peak=%.6f\n",
           W0 * (1.0f + d_closed) - W0 * (1.0f - d_closed));
    printf("jnd_max_width_diff_w0_ring=%.6f\n",
           W0 * (1.0f + d_ring) - W0 * (1.0f - d_ring));

    /* ---- three lamp intensity levels ---- */
    static const float GAMMAS[7] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    for (int t = 0; t < 3; t++) {
        float tile[CH][NN];
        build_tile(tile, LEVELS[t]);
        check("core_is_white",
              fabsf(tile[R][(N / 2) * N + (N / 2)] -
                    tile[B][(N / 2) * N + (N / 2)]) < 1e-6f);

        Measure prev = { 0 };
        for (int gi = 0; gi < 7; gi++) {
            Measure m;
            measure_tile(tile, GAMMAS[gi], &m);
            if (gi == 0) {
                int equal_w = 1;
                for (int ch = 1; ch < CH; ch++)
                    equal_w = equal_w && fabsf(m.width[ch] - W0) < 1e-6f;
                check("gamma0_widths_uniform", equal_w);
            } else {
                /* a narrower blue kernel loses the outer-ring tail, so the
                   blue deficit (yellow cast) deepens monotonically */
                check("yellow_cast_grows_with_gamma",
                      m.cast[B] <= prev.cast[B] + 1e-5f);
            }
            if (GAMMAS[gi] > 0.0f) {
                check("cast_direction_blue_deficit", m.cast[B] < 0.0f);
                check("cast_direction_red_excess", m.cast[R] > 0.0f);
            }
            printf("curve,%s,%.2f,%.6f,%.6f,%.6f,%.6f\n",
                   TIER_NAME[t], GAMMAS[gi], m.spread,
                   m.cast[R], m.cast[G], m.cast[B]);
            prev = m;
        }

        /* operating point: brightness-weighted widths, gamma = 1 */
        Measure wm, um;
        measure_tile(tile, 1.0f, &wm);
        measure_tile(tile, 0.0f, &um);   /* uniform-width recomposite */

        /* measured JND inversion: largest gamma that still passes */
        float lo = 0.0f, hi = 2.0f;
        for (int it = 0; it < 60; it++) {
            float g = 0.5f * (lo + hi);
            Measure mm;
            measure_tile(tile, g, &mm);
            if (mm.pass) lo = g;
            else hi = g;
        }
        Measure gm;
        measure_tile(tile, lo, &gm);

        printf("tier=%s L=%.0f gamma_max=%.4f max_width_diff=%.6f\n",
               TIER_NAME[t], LEVELS[t], lo,
               fmaxf(fmaxf(gm.width[R], gm.width[G]), gm.width[B]) -
               fminf(fminf(gm.width[R], gm.width[G]), gm.width[B]));
        for (int ch = 0; ch < CH; ch++) {
            const char* cn = ch == R ? "R" : ch == G ? "G" : "B";
            printf("  weighted,%s,E=%.6f,w=%.6f,H=%.6f,cast=%.6f\n",
                   cn, wm.energy[ch], wm.width[ch], wm.halo[ch], wm.cast[ch]);
        }
        printf("  weighted,spread=%.6f,pass=%d\n", wm.spread, wm.pass);
        for (int ch = 0; ch < CH; ch++) {
            const char* cn = ch == R ? "R" : ch == G ? "G" : "B";
            printf("  uniform,%s,E=%.6f,w=%.6f,H=%.6f,cast=%.6f\n",
                   cn, um.energy[ch], um.width[ch], um.halo[ch], um.cast[ch]);
        }
        printf("  uniform,spread=%.6f,pass=%d\n", um.spread, um.pass);
        check("uniform_recomposite_passes", um.pass);
    }

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
