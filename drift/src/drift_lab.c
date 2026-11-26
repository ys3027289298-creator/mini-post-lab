/*
 * drift_lab — deterministic accumulation-drift calibration and reset-policy lab.
 *
 * The checked-in post modules have no temporal accumulator or pressure driver,
 * so this is a reproducible instrument for the two hypotheses in the task:
 *
 *   linear:  E_N = N * d
 *   sqrt:    RMS(E_N) = s * sqrt(N), independent stationary rounding residuals
 *
 * Time is expressed at FPS=60; a "600 s" tier executes 36,000 frames.
 */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define FPS 60.0
#define Q_LEVEL (1.0 / 256.0)       /* 256 equal steps on [0,1] */
#define BUDGET (0.5 * Q_LEVEL)       /* half quantization level = 1/512 */
#define N10 ((int)(FPS * 600.0))
#define LANES 4096
#define POLICY_R 600                 /* 10 s at 60 fps: 95% continuity target */
#define NOISE_FRAMES 2160000

struct tier {
    int seconds;
    const char *slug;
    const char *label;
};

static const struct tier TIERS[] = {
    {  60, "1min",  "1 min"  },
    { 600, "10min", "10 min" },
    {3600, "60min", "60 min" },
};
#define TIER_COUNT (int)(sizeof(TIERS) / sizeof(TIERS[0]))

static uint32_t hash_seed(uint32_t lane, int tier, uint32_t salt) {
    uint32_t x = lane * 2654435761u
               + (uint32_t)(tier + 17) * 2246822519u
               + salt * 3266489917u;
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x ? x : 1u;
}

static float next_signed(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 1u;
    /* 24-bit mantissa -> approximately uniform on [-1, 1]. */
    return ((float)(x >> 8) / 8388608.0f) - 1.0f;
}

struct run_stats {
    double signed_mean;
    double rms;
    double max_abs;
    double boundary_rms;
    double policy_max_abs;
    int frames;
};

static struct run_stats run_linear(int frames) {
    /* Calibrated from the field anchor D(600 s)=BUDGET. */
    const double d = BUDGET / (double)N10;
    struct run_stats st = {0};
    st.frames = frames;
    double sum = 0.0, sq = 0.0, max_abs = 0.0;
    double boundary_sq = 0.0;
    long boundary_count = 0;

    for (int p = 0; p < LANES; p++) {
        float e = 0.0f;
        float boundary_acc = 0.0f;
        double lane_max = 0.0;
        for (int i = 0; i < frames; i++) {
            float step = (float)d;
            e += step;
            boundary_acc += step;
            double ae = fabs((double)e);
            if (ae > lane_max) lane_max = ae;
            if ((i + 1) % POLICY_R == 0) {
                boundary_sq += (double)boundary_acc * (double)boundary_acc;
                boundary_count++;
                boundary_acc = 0.0f;
            }
        }
        sum += (double)e;
        sq += (double)e * (double)e;
        if (lane_max > max_abs) max_abs = lane_max;
        if (lane_max > st.policy_max_abs) st.policy_max_abs = lane_max;
    }

    st.signed_mean = sum / LANES;
    st.rms = sqrt(sq / LANES);
    st.max_abs = max_abs;
    /* The linear policy max is the same lane max; boundary RMS is separate. */
    st.boundary_rms = sqrt(boundary_sq / (double)boundary_count);
    st.policy_max_abs = d * (double)POLICY_R;
    return st;
}

static struct run_stats run_sqrt(int frames, int tier_index) {
    /*
     * sigma is the standard deviation of one stationary rounding residual.
     * Uniform residuals use amplitude sqrt(3)*sigma to have that variance.
     */
    const double sigma = BUDGET / sqrt((double)N10);
    const float amp = (float)(sqrt(3.0) * sigma);
    struct run_stats st = {0};
    st.frames = frames;
    double sum = 0.0, sq = 0.0, max_abs = 0.0;
    double boundary_sq = 0.0;
    long boundary_count = 0;
    double policy_max = 0.0;

    for (int p = 0; p < LANES; p++) {
        uint32_t rng = hash_seed((uint32_t)p, tier_index, 0x51a7u);
        float e = 0.0f;
        float epoch = 0.0f;
        double lane_max = 0.0;
        for (int i = 0; i < frames; i++) {
            float residual = next_signed(&rng) * amp;
            e += residual;
            epoch += residual;
            double ae = fabs((double)e);
            if (ae > lane_max) lane_max = ae;

            double pe = fabs((double)epoch);
            if (pe > policy_max) policy_max = pe;

            if ((i + 1) % POLICY_R == 0) {
                boundary_sq += (double)epoch * (double)epoch;
                boundary_count++;
                epoch = 0.0f;
            }
        }
        sum += (double)e;
        sq += (double)e * (double)e;
        if (lane_max > max_abs) max_abs = lane_max;
    }

    st.signed_mean = sum / LANES;
    st.rms = sqrt(sq / LANES);
    st.max_abs = max_abs;
    st.boundary_rms = sqrt(boundary_sq / (double)boundary_count);
    st.policy_max_abs = policy_max;
    return st;
}

static void write_run_csv(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "model,tier,duration_s,frames,lanes,signed_mean_norm,rms_norm,"
               "max_abs_norm,policy_r_frames,boundary_rms_norm,policy_max_abs_norm,"
               "budget_norm,rms_pass,max_pass\n");

    for (int ti = 0; ti < TIER_COUNT; ti++) {
        int frames = (int)(FPS * TIERS[ti].seconds);
        struct run_stats ls = run_linear(frames);
        int rms_pass = ls.boundary_rms <= BUDGET;
        int max_pass = ls.policy_max_abs <= BUDGET;
        fprintf(f, "linear,%s,%d,%d,%d,%.12g,%.12g,%.12g,%d,%.12g,%.12g,%.12g,%d,%d\n",
                TIERS[ti].slug, TIERS[ti].seconds, frames, LANES,
                ls.signed_mean, ls.rms, ls.max_abs, POLICY_R,
                ls.boundary_rms, ls.policy_max_abs, BUDGET, rms_pass, max_pass);
    }

    for (int ti = 0; ti < TIER_COUNT; ti++) {
        int frames = (int)(FPS * TIERS[ti].seconds);
        struct run_stats rs = run_sqrt(frames, ti);
        int rms_pass = rs.boundary_rms <= BUDGET;
        int max_pass = rs.policy_max_abs <= BUDGET;
        fprintf(f, "sqrt,%s,%d,%d,%d,%.12g,%.12g,%.12g,%d,%.12g,%.12g,%.12g,%d,%d\n",
                TIERS[ti].slug, TIERS[ti].seconds, frames, LANES,
                rs.signed_mean, rs.rms, rs.max_abs, POLICY_R,
                rs.boundary_rms, rs.policy_max_abs, BUDGET, rms_pass, max_pass);
    }
    fclose(f);
}

static void write_reset_curves(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    const double d = BUDGET / (double)N10;
    const double sigma = BUDGET / sqrt((double)N10);
    fprintf(f, "reset_frames,reset_seconds,linear_drift_quant,sqrt_drift_rms_quant,"
               "continuity\n");
    int last_r = -1;
    for (int k = 0; k <= 120; k++) {
        double log_r = log10(1.0)
                     + (log10(216000.0) - log10(1.0)) * k / 120.0;
        int r = (int)llround(pow(10.0, log_r));
        if (r < 1 || r == last_r) continue;
        last_r = r;
        double lin_q = d * r / Q_LEVEL;
        double sqrt_q = sigma * sqrt((double)r) / Q_LEVEL;
        double continuity = 1.0 - 1.0 / sqrt((double)r);
        fprintf(f, "%d,%.9g,%.12g,%.12g,%.12g\n",
                r, r / FPS, lin_q, sqrt_q, continuity);
    }
    fclose(f);
}

static double continuity_measurement(int reset_frames) {
    uint32_t rng = hash_seed(7, 99, 0xc017u);
    double sum = 0.0;
    int count = 0;
    double prev = 0.0;
    double diff_sq = 0.0;
    int diffs = 0;
    const float noise_amp = sqrtf(3.0f); /* unit variance uniform noise */

    for (int i = 0; i < NOISE_FRAMES; i++) {
        float x = next_signed(&rng) * noise_amp;
        double out;
        if (reset_frames == 1 || i % reset_frames == 0) {
            sum = x;
            count = 1;
            out = x;
        } else {
            sum += x;
            count++;
            out = sum / count;
        }
        if (i > 0) {
            double diff = out - prev;
            diff_sq += diff * diff;
            diffs++;
        }
        prev = out;
    }
    return sqrt(diff_sq / diffs);
}

static void write_continuity_csv(const char *path) {
    static const int RS[] = {1, 30, 60, 120, 300, 600, 3600, 21600};
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "reset_frames,reset_seconds,measured_diff_rms,theory_diff_rms,"
               "measured_continuity\n");
    for (unsigned i = 0; i < sizeof(RS) / sizeof(RS[0]); i++) {
        int r = RS[i];
        double measured = continuity_measurement(r);
        double theory = sqrt(2.0 / r);
        double raw = continuity_measurement(1);
        double continuity = 1.0 - measured / raw;
        fprintf(f, "%d,%.9g,%.12g,%.12g,%.12g\n",
                r, r / FPS, measured, theory, continuity);
    }
    fclose(f);
}

int main(void) {
    write_run_csv("out/runs.csv");
    write_reset_curves("out/reset_curves.csv");
    write_continuity_csv("out/continuity_check.csv");

    const double d = BUDGET / (double)N10;
    const double sigma = BUDGET / sqrt((double)N10);
    printf("fps=%.0f q=1/256 budget=1/512=%.12g frames_10min=%d\n",
           FPS, BUDGET, N10);
    printf("linear_delta_per_frame=%.12g\n", d);
    printf("sqrt_sigma_per_frame=%.12g\n", sigma);
    printf("policy_reset_frames=%d policy_reset_seconds=%.1f\n",
           POLICY_R, POLICY_R / FPS);
    printf("wrote out/runs.csv out/reset_curves.csv out/continuity_check.csv\n");
    return 0;
}
