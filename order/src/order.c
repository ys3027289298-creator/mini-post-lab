/* order: bloom<->tonemap ordering experiment.
 *
 * order A (bloom_first):  out = T(E*L + k*blur(bright(E*L - tau)))
 * order B (tonemap_first):out = T(E*L) + k*blur(bright(T(E*L) - T(tau)))
 *
 * Threshold in B is the tonemapped linear threshold, so both orders
 * express the same artistic intent and the measured delta is purely
 * an ordering effect.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define W 8
#define H 8
#define N (W * H)

#define BLUR_RADIUS 1
#define BLOOM_K 0.5f
#define TAU 1.0f
#define EXPOSURE 1.0f
#define TOL 0.01f          /* acceptable |delta| in display space */
#define VIS (1.0f / 255.0f) /* visible bloom contribution */

enum { CURVE_REINHARD, CURVE_ACES };

static float tonemap(float x, int curve) {
    if (curve == CURVE_REINHARD) return x / (1.0f + x);
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (x * (a * x + b)) / (x * (c * x + d) + e);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

static void bright_pass(const float* src, float* dst, float threshold) {
    for (int i = 0; i < N; i++) {
        float v = src[i] - threshold;
        dst[i] = v > 0.0f ? v : 0.0f;
    }
}

/* separable box blur, clamped edges (same as bloom/) */
static void box_blur(const float* src, float* dst) {
    float tmp[N];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float s = 0.0f;
            for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= W) xx = W - 1;
                s += src[y * W + xx];
            }
            tmp[y * W + x] = s / (float)(2 * BLUR_RADIUS + 1);
        }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float s = 0.0f;
            for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= H) yy = H - 1;
                s += tmp[yy * W + x];
            }
            dst[y * W + x] = s / (float)(2 * BLUR_RADIUS + 1);
        }
}

/* img += k * blur(bright_pass(src, tau)) */
static void bloom_add(const float* src, float* img, float tau, float k) {
    float bp[N], bl[N];
    bright_pass(src, bp, tau);
    box_blur(bp, bl);
    for (int i = 0; i < N; i++) img[i] += k * bl[i];
}

/* order A: exposure -> bloom (linear) -> tonemap curve */
static void order_bloom_first(const float* lin, float* out, int curve) {
    float acc[N];
    for (int i = 0; i < N; i++) acc[i] = lin[i] * EXPOSURE;
    bloom_add(acc, acc, TAU, BLOOM_K);
    for (int i = 0; i < N; i++) out[i] = tonemap(acc[i], curve);
}

/* order B: exposure -> tonemap curve -> bloom (display space, matched threshold) */
static void order_tonemap_first(const float* lin, float* out, int curve) {
    for (int i = 0; i < N; i++) out[i] = tonemap(lin[i] * EXPOSURE, curve);
    bloom_add(out, out, tonemap(TAU, curve), BLOOM_K);
}

/* scene: flat base with a 2x2 hot block at `peak` */
static void make_scene(float* lin, float peak) {
    for (int i = 0; i < N; i++) lin[i] = 0.1f;
    for (int y = 3; y <= 4; y++)
        for (int x = 3; x <= 4; x++) lin[y * W + x] = peak;
}

static int coverage(const float* out, const float* lin, int curve) {
    int n = 0;
    for (int i = 0; i < N; i++)
        if (out[i] - tonemap(lin[i] * EXPOSURE, curve) > VIS) n++;
    return n;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    static const struct { const char* name; int curve; } curves[] = {
        { "reinhard", CURVE_REINHARD }, { "aces", CURVE_ACES },
    };
    static const float peaks[] = { 1.0f, 8.0f, 64.0f }; /* LDR / HDR-mid / HDR-high */

    float lin[N], a[N], b[N];
    float diff[2][3];
    int cov_a[2][3], cov_b[2][3];

    for (int c = 0; c < 2; c++) {
        for (int t = 0; t < 3; t++) {
            make_scene(lin, peaks[t]);
            order_bloom_first(lin, a, curves[c].curve);
            order_tonemap_first(lin, b, curves[c].curve);
            float mx = 0.0f, mean = 0.0f;
            for (int i = 0; i < N; i++) {
                float d = fabsf(a[i] - b[i]);
                if (d > mx) mx = d;
                mean += d;
            }
            mean /= N;
            diff[c][t] = mx;
            cov_a[c][t] = coverage(a, lin, curves[c].curve);
            cov_b[c][t] = coverage(b, lin, curves[c].curve);
            printf("curve=%s peak=%.1f max_diff=%.6f mean_diff=%.6f "
                   "cov_A=%d/%d cov_B=%d/%d within_tol=%d\n",
                   curves[c].name, peaks[t], mx, mean,
                   cov_a[c][t], N, cov_b[c][t], N, mx <= TOL);
        }
    }

    /* correctness curve: max |delta| vs scene peak (reinhard) */
    for (float p = 0.5f; p <= 64.0f; p *= 2.0f) {
        make_scene(lin, p);
        order_bloom_first(lin, a, CURVE_REINHARD);
        order_tonemap_first(lin, b, CURVE_REINHARD);
        float mx = 0.0f;
        for (int i = 0; i < N; i++) {
            float d = fabsf(a[i] - b[i]);
            if (d > mx) mx = d;
        }
        printf("sweep peak=%.1f max_diff=%.6f\n", p, mx);
    }

    /* self-checks */
    check("ldr_within_tol_reinhard", diff[0][0] <= TOL);
    check("ldr_within_tol_aces", diff[1][0] <= TOL);
    check("hdr_high_exceeds_tol_reinhard", diff[0][2] > TOL);
    check("hdr_high_exceeds_tol_aces", diff[1][2] > TOL);
    check("diff_grows_with_dr_reinhard", diff[0][0] <= diff[0][1] && diff[0][1] <= diff[0][2]);
    check("diff_grows_with_dr_aces", diff[1][0] <= diff[1][1] && diff[1][1] <= diff[1][2]);
    check("bloom_wider_in_B_hdr_high", cov_b[0][2] >= cov_a[0][2] && cov_b[1][2] >= cov_a[1][2]);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
