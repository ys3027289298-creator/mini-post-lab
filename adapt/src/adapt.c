
/* adapt: dark adaptation model + local exposure window sizing.
   Sweeps the local-statistics window against two error curves
   (accuracy vs stability, both in JND units) for three cave sizes. */
#include <stdio.h>
#include <math.h>

#define FW 256
#define FH 256
#define CX 128.0f
#define CY 128.0f
#define PI 3.14159265358979f

static float frame[FH][FW];

/* scene luminances */
#define L_C 0.05f   /* character */
#define L_F 0.10f   /* cave floor around character */
#define L_W 0.55f   /* rock wall */
#define L_E 2.50f   /* entrance glow */
#define R_C 10.0f   /* character radius, px */
#define R_V 20.0f   /* design vicinity radius ("角色周围"), px */

/* perception */
#define K_W 0.02f                        /* Weber fraction: 1 JND */
#define JND 0.0198026272961797f          /* ln(1 + K_W) */
#define L_KEY 0.18f                      /* exposure key luminance */
#define L_TH0 0.25f                      /* light-adapted threshold */
#define L_THINF 0.004f                   /* fully dark-adapted threshold */
#define TAU 3.0f                         /* adaptation time constant, s */

/* jitter */
#define D_CAM 2.0f    /* camera shake radius, px */
#define A_ANIM 0.12f  /* character animation luminance fluctuation */

static unsigned rng = 1;
static float frand(void) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) * (1.0f / 16777216.0f);
}

static void build_scene(float r_f) {
    rng = 777;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            float dx = x - CX, dy = y - CY;
            float d2 = dx * dx + dy * dy;
            float v;
            if (d2 < R_C * R_C) v = L_C;
            else if (d2 < r_f * r_f) v = L_F;
            else v = L_W * (0.85f + 0.30f * frand());   /* noisy rock */
            float ex = x - 200.0f, ey = y - 40.0f;      /* entrance glow */
            if (ex * ex + ey * ey < 18.0f * 18.0f) v = L_E;
            frame[y][x] = v;
        }
}

static float mean_disk(float cx, float cy, float r) {
    double s = 0;
    long n = 0;
    int r0 = (int)ceilf(r);
    for (int y = (int)(cy - r0); y <= (int)(cy + r0); y++)
        for (int x = (int)(cx - r0); x <= (int)(cx + r0); x++) {
            if (x < 0 || y < 0 || x >= FW || y >= FH) continue;
            float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) { s += frame[y][x]; n++; }
        }
    return n ? (float)(s / n) : 0.0f;
}

static float frame_mean(void) {
    double s = 0;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) s += frame[y][x];
    return (float)(s / (FW * FH));
}

/* accuracy: |ln(E_jitter[mean(w)] / l_ref)|
   stability: log-std of mean(w) under camera shake, plus the character
   animation term A*L_c*(R_c/w)^2/mean (small window = character-dominated) */
static void window_err(float w, float l_ref, float* acc, float* stab) {
    rng = 1234;   /* common random offsets across w for comparability */
    double s = 0, s2 = 0, sm = 0;
    int n = 48;
    for (int i = 0; i < n; i++) {
        float ang = 2.0f * PI * frand();
        float rr = D_CAM * sqrtf(frand());
        float m = mean_disk(CX + rr * cosf(ang), CY + rr * sinf(ang), w);
        float lm = logf(m);
        s += lm; s2 += lm * lm; sm += m;
    }
    float em = (float)(sm / n);
    *acc = fabsf(logf(em / l_ref));
    double var = s2 / n - (s / n) * (s / n);
    if (var < 0) var = 0;
    float fc = R_C < w ? R_C / w : 1.0f;
    float sa = A_ANIM * L_C * fc * fc / em;
    *stab = sqrtf((float)var + sa * sa);
}

static float lth(float t) { return L_THINF + (L_TH0 - L_THINF) * expf(-t / TAU); }
static float perceived(float L, float t) {
    float r = L / lth(t);
    return r > 1.0f ? logf(r) : 0.0f;
}
static float t_visible(float L) {
    if (L <= L_THINF) return -1.0f;   /* never visible */
    if (L >= L_TH0) return 0.0f;      /* visible immediately */
    return TAU * logf((L_TH0 - L_THINF) / (L - L_THINF));
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    check("lth_decays", lth(0.0f) > lth(3.0f) && lth(3.0f) > lth(30.0f));
    check("lth_floor", fabsf(lth(200.0f) - L_THINF) < 1e-4f);
    check("perceived_below", perceived(0.001f, 0.0f) == 0.0f);
    check("perceived_above", perceived(1.0f, 0.0f) > 0.0f);
    check("perceived_recovers", perceived(0.02f, 0.0f) == 0.0f && perceived(0.02f, 30.0f) > 0.0f);
    check("jnd_value", fabsf(JND - logf(1.0f + K_W)) < 1e-6f);
    check("tvis_order", t_visible(0.10f) < t_visible(0.016f));

    build_scene(30.0f);
    check("mean_char", fabsf(mean_disk(CX, CY, 5.0f) - L_C) < 1e-6f);
    check("frame_mean_range", frame_mean() > 0.3f && frame_mean() < 0.9f);
    check("window_mono", mean_disk(CX, CY, 12.0f) < mean_disk(CX, CY, 26.0f));

    struct { const char* name; float r_f; } tiers[] = {
        { "small", 19.5f }, { "mid", 30.0f }, { "large", 48.0f },
    };
    int pass_count = 0;
    for (int t = 0; t < 3; t++) {
        build_scene(tiers[t].r_f);
        float l_frame = frame_mean();
        float l_v = mean_disk(CX, CY, R_V);
        printf("== %s r_f=%.1f ==\n", tiers[t].name, tiers[t].r_f);
        printf("L_frame=%.4f L_vicinity=%.4f\n", l_frame, l_v);
        printf("w acc/JND stab/JND\n");
        float best_w = -1.0f, best_score = 1e9f, feas_w = -1.0f, feas_dw = 1e9f;
        for (int i = 0; i <= 320; i++) {
            float w = 8.0f + 0.1f * i;
            float acc, stab;
            window_err(w, l_v, &acc, &stab);
            if (i % 20 == 0) printf("%.1f %.2f %.2f\n", w, acc / JND, stab / JND);
            float score = (acc > stab ? acc : stab) / JND;
            if (score < best_score) { best_score = score; best_w = w; }
            if (acc <= JND && stab <= JND) {
                float dw = fabsf(w - R_V);
                if (dw < feas_dw) { feas_dw = dw; feas_w = w; }
            }
        }
        int ok = feas_w > 0.0f;
        float w = ok ? feas_w : best_w;
        float l_loc = mean_disk(CX, CY, w);
        float acc, stab;
        window_err(w, l_v, &acc, &stab);
        float tv_g = t_visible(L_C * (L_KEY / l_frame));
        float tv_l = t_visible(L_C * (L_KEY / l_loc));
        printf("w*=%.1f L_loc=%.4f L_frame=%.4f ratio=%.2f d_stops=%.2f\n",
               w, l_loc, l_frame, l_frame / l_loc, log2f(l_frame / l_loc));
        printf("acc=%.4f (%.1f JND) stab=%.4f (%.1f JND) %s\n",
               acc, acc / JND, stab, stab / JND, ok ? "PASS" : "FAIL");
        printf("t_vis global=%.2fs local=%.2fs\n", tv_g, tv_l);
        if (ok) pass_count++;
    }
    printf("tiers_pass=%d/3\n", pass_count);
    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
