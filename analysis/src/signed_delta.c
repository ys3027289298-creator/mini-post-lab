/*
 * signed_delta: direction of the preview-export difference at b=8.
 * Quantization and clipping only ever lower a pixel, so the signed mean
 * cannot be positive; "preview looks brighter" is contrast flattening.
 * Writes analysis/out/signed_delta.csv under both headroom models.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#define W 256
#define H 256
#define NPIX (W * H)

static float reinhard(float v) { return v / (1.0f + v); }
static float aces(float v) {
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (v * (a * v + b)) / (v * (c * v + d) + e);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}
static float (*T[2])(float) = {reinhard, aces};
static const char* curve_name[2] = {"reinhard", "aces"};

static void make_scene(int tier, float* img) {
    const float base[3] = {0.02f, 0.25f, 1.20f};
    const float span[3] = {0.30f, 2.50f, 8.00f};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = (float)x / (W - 1), v = (float)y / (H - 1);
            float r = base[tier] + span[tier] * u;
            if ((x * 7 + y * 13) % 64 == 0) r *= 3.0f;
            r *= 0.95f + 0.10f * v;
            img[y * W + x] = r;
        }
}

static float quantize_b(float x, int b, float xmax) {
    if (x <= 0.0f) return 0.0f;
    if (x >= xmax) return xmax;
    double levels = (double)((1ULL << b) - 1);
    long q = lround((double)x / (double)xmax * levels);
    return (float)((double)q / levels * (double)xmax);
}

int main(void) {
    const char* tier_name[3] = {"dark", "mid", "bright"};
    const float fixed_headroom[3] = {1.0f, 8.0f, 8.0f};
    FILE* f = fopen("analysis/out/signed_delta.csv", "w");
    fprintf(f, "tier,curve,headroom_model,xmax,clip_fraction,mean_signed_delta,brighter_pixels,darker_pixels\n");

    for (int tier = 0; tier < 3; tier++) {
        float* hdr = malloc(sizeof(float) * NPIX);
        make_scene(tier, hdr);
        float frame_max = 0.0f;
        for (int i = 0; i < NPIX; i++) if (hdr[i] > frame_max) frame_max = hdr[i];

        float models[2] = {frame_max, fixed_headroom[tier]};
        const char* model_name[2] = {"frame_max", "fixed"};
        for (int mi = 0; mi < 2; mi++) {
            float xmax = models[mi];
            for (int ci = 0; ci < 2; ci++) {
                double sd = 0.0; int npos = 0, nneg = 0, nclip = 0;
                for (int i = 0; i < NPIX; i++) {
                    if (hdr[i] >= xmax) nclip++;
                    float d = T[ci](quantize_b(hdr[i], 8, xmax)) - T[ci](hdr[i]);
                    sd += d;
                    if (d > 0) npos++; else if (d < 0) nneg++;
                }
                fprintf(f, "%s,%s,%s,%.4f,%.6f,%.8f,%d,%d\n",
                        tier_name[tier], curve_name[ci], model_name[mi], xmax,
                        (double)nclip / NPIX, sd / NPIX, npos, nneg);
            }
        }
        free(hdr);
    }
    fclose(f);
    printf("wrote analysis/out/signed_delta.csv\n");
    return 0;
}
