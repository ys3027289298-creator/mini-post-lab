/*
 * lut3d: bake + trilinear sampling, uniform and power-law non-uniform nodes.
 */
#include "lut.h"

#include <math.h>

/* Smooth filmic-ish per-channel response. Curvature magnitude 2k/(x+k)^3 is
 * largest in the shadows, so a uniform LUT is least accurate exactly where
 * the eye is most sensitive — the failure mode this module exists to
 * quantify. */
float lut_curve(float x) {
    const float k = 0.18f;
    if (x <= 0.0f) return 0.0f;
    float y = x * (1.0f + k) / (x + k);
    return y > 1.0f ? 1.0f : y;
}

void lut_reference(const float in[3], float out[3]) {
    out[0] = lut_curve(in[0]);
    out[1] = lut_curve(in[1]);
    out[2] = lut_curve(in[2]);
}

void lut_set_nodes(Lut3d* lut, int n, float spacing_law) {
    lut->n = n;
    lut->spacing_law = spacing_law;
    float denom = (float)(n - 1);
    for (int i = 0; i < n; i++) {
        float u = (float)i / denom;           /* uniform parameter in [0,1] */
        lut->nodes[i] = powf(u, spacing_law); /* p=1 -> uniform in x */
    }
}

static int vidx(int n, int i, int j, int k, int ch) {
    return ((i * n + j) * n + k) * 3 + ch;
}

void lut_bake(Lut3d* lut, LutTransform f) {
    float in[3], out[3];
    for (int i = 0; i < lut->n; i++) {
        in[0] = lut->nodes[i];
        for (int j = 0; j < lut->n; j++) {
            in[1] = lut->nodes[j];
            for (int k = 0; k < lut->n; k++) {
                in[2] = lut->nodes[k];
                f(in, out);
                lut->data[vidx(lut->n, i, j, k, 0)] = out[0];
                lut->data[vidx(lut->n, i, j, k, 1)] = out[1];
                lut->data[vidx(lut->n, i, j, k, 2)] = out[2];
            }
        }
    }
}

/* locate x on the node axis: cell index c and fractional t in [0,1].
 * Generic path — binary search, required for non-uniform nodes. */
static int locate_generic(const Lut3d* lut, float x, int* c, float* t) {
    int n = lut->n;
    if (n < 2) return -1;
    if (x <= lut->nodes[0]) { *c = 0; *t = 0.0f; return 0; }
    if (x >= lut->nodes[n - 1]) { *c = n - 2; *t = 1.0f; return 0; }
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (lut->nodes[mid] <= x) lo = mid; else hi = mid;
    }
    *c = lo;
    *t = (x - lut->nodes[lo]) / (lut->nodes[lo + 1] - lut->nodes[lo]);
    return 0;
}

/* Uniform-only fast path: one FMA instead of a binary search. */
static int locate_uniform(const Lut3d* lut, float x, int* c, float* t) {
    int n = lut->n;
    float u = x * (float)(n - 1);
    int i = (int)u;
    if (i < 0) { *c = 0; *t = 0.0f; return 0; }
    if (i >= n - 1) { *c = n - 2; *t = 1.0f; return 0; }
    *c = i;
    *t = u - (float)i;
    return 0;
}

static int sample_with(const Lut3d* lut, const float in[3], float out[3],
                       int (*locate)(const Lut3d*, float, int*, float*)) {
    int c[3];
    float t[3], w[3][2];
    for (int a = 0; a < 3; a++) {
        float v = in[a] < 0.0f ? 0.0f : (in[a] > 1.0f ? 1.0f : in[a]);
        if (locate(lut, v, &c[a], &t[a]) != 0) return -1;
        w[a][0] = 1.0f - t[a];
        w[a][1] = t[a];
    }
    int n = lut->n;
    for (int ch = 0; ch < 3; ch++) {
        float acc = 0.0f;
        for (int dx = 0; dx <= 1; dx++)
            for (int dy = 0; dy <= 1; dy++)
                for (int dz = 0; dz <= 1; dz++) {
                    float weight = w[0][dx] * w[1][dy] * w[2][dz];
                    acc += weight *
                           lut->data[vidx(n, c[0] + dx, c[1] + dy,
                                          c[2] + dz, ch)];
                }
        out[ch] = acc;
    }
    return 0;
}

int lut_sample(const Lut3d* lut, const float in[3], float out[3]) {
    return sample_with(lut, in, out, locate_generic);
}

int lut_sample_fast(const Lut3d* lut, const float in[3], float out[3]) {
    if (lut->spacing_law != 1.0f) return -1;
    return sample_with(lut, in, out, locate_uniform);
}
