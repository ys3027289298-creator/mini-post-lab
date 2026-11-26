
/* bloom: bright-pass extraction and separable box blur. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 8
#define H 8

static int bright_pass(const float* src, float* dst, int n, float threshold) {
    if (n <= 0 || threshold < 0.0f) return -1;
    for (int i = 0; i < n; i++) {
        float v = src[i] - threshold;
        dst[i] = v > 0.0f ? v : 0.0f;
    }
    return 0;
}

/* separable box blur with radius r, clamped edges */
static int box_blur(const float* src, float* dst, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0 || r * 2 + 1 > w || r * 2 + 1 > h) return -1;
    float* tmp = malloc((size_t)w * (size_t)h * sizeof(*tmp));
    if (tmp == NULL) return -2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float s = 0.0f;
            for (int k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= w) xx = w - 1;
                s += src[y * w + xx];
            }
            tmp[y * w + x] = s / (float)(2 * r + 1);
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float s = 0.0f;
            for (int k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= h) yy = h - 1;
                s += tmp[yy * w + x];
            }
            dst[y * w + x] = s / (float)(2 * r + 1);
        }
    }
    free(tmp);
    return 0;
}

static float sum(const float* a, int n) { float s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    float src[W*H], dst[W*H];
    for (int i = 0; i < W*H; i++) src[i] = 0.5f;

    check("bright_ok", bright_pass(src, dst, W*H, 0.3f) == 0);
    check("bright_all_above", fabsf(sum(dst, W*H) - 0.2f * W*H) < 1e-3f);

    check("bright_none", bright_pass(src, dst, W*H, 0.9f) == 0 && sum(dst, W*H) == 0.0f);

    /* blur preserves total energy on flat field */
    float flat[W*H], out[W*H];
    for (int i = 0; i < W*H; i++) flat[i] = 1.0f;
    check("blur_ok", box_blur(flat, out, W, H, 2) == 0);
    check("blur_flat_preserved", fabsf(out[W*H/2] - 1.0f) < 1e-5f);

    /* a single spike spreads out but keeps the mean */
    float spike[W*H]; memset(spike, 0, sizeof(spike));
    spike[3*W + 3] = (float)(W*H);
    box_blur(spike, out, W, H, 1);
    check("blur_spreads", out[3*W + 4] > 0.0f);
    check("blur_mean_kept", fabsf(sum(out, W*H) / (W*H) - sum(spike, W*H) / (W*H)) < 1e-2f);
    check("blur_peak_drops", out[3*W + 3] < spike[3*W + 3]);

    check("blur_radius_too_big", box_blur(flat, out, W, H, 10) == -1);
    check("bright_bad_threshold", bright_pass(src, dst, W*H, -1.0f) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
