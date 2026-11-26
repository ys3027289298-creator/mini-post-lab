/*
 * seam: split-frame tone-mapping seam experiment.
 *
 * One HDR frame is cut into P horizontal strips, as P workers would split
 * it. Every strip applies Reinhard tone mapping anchored on the log-average
 * luminance of its statistics window. Three policies are compared:
 *
 *   global - one log-average reduced over the whole frame (all-reduce cost,
 *            every worker uses the identical anchor)
 *   local  - each strip uses its own log-average (zero communication)
 *   double - boundary rows are tone mapped twice because both neighbours
 *            claim the overlap row (functional composition of the curve)
 *
 * Seam discontinuities are measured against the 8-bit budget: one
 * quantization level = 1/255 LDR. See README.md for the full derivation.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#define H 256
#define W 256
#define EPS 1e-4f
#define KEY 1.0f
#define CV 255.0f                  /* code values per unit LDR (8-bit) */
#define QUANT (1.0f / CV)

static float img[H][W];
static float ref[H][W];           /* global stats, single mapping */
static float loc[H][W];           /* per-tile (local) stats */
static float dbl[H][W];           /* double-mapped boundary rows */

static float reinhard(float x, float s) { return x / (s + x); }

/* Synthetic HDR frame: vertical gradient + bright band + hot spot, so that
 * different horizontal strips genuinely contain different statistics. */
static void gen_frame(void) {
    for (int r = 0; r < H; r++) {
        float fn = (float)r / H;
        for (int c = 0; c < W; c++) {
            float gn = (float)c / W;
            float v = 0.30f + 0.60f * fn;
            /* bright horizontal band, lower-middle of the frame */
            float d = fn - 0.70f;
            v += 2.6f * expf(-(d * d) / (2.0f * 0.05f * 0.05f));
            /* localized hot spot, upper-right */
            float dr = (r - 70) / 22.0f, dc = (c - 190) / 28.0f;
            v += 9.0f * expf(-(dr * dr + dc * dc) * 0.5f);
            /* mild horizontal modulation: worst column differs per seam */
            v *= 1.0f + 0.15f * sinf(6.2831853f * gn);
            img[r][c] = v;
        }
    }
}

/* Log-average world luminance over rows [r0, r1), all columns. */
static float log_avg(int r0, int r1) {
    double sum = 0.0;
    long n = 0;
    for (int r = r0; r < r1; r++)
        for (int c = 0; c < W; c++) {
            sum += log((double)img[r][c] + EPS);
            n++;
        }
    return (float)exp(sum / (double)n);
}

static int failures = 0;
static void check(const char *name, int ok) {
    printf("check %s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(int argc, char **argv) {
    gen_frame();

    float Lg = log_avg(0, H);
    float sg = KEY * Lg;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            ref[r][c] = reinhard(img[r][c], sg);

    FILE *csv = argc > 1 ? fopen(argv[1], "w") : NULL;
    if (csv)
        fprintf(csv,
                "P,seams,local_max_cv,local_fails,double_max_cv,"
                "double_fails,global_max_cv,reduce_steps,rows_per_worker\n");

    printf("frame=%dx%d global_log_avg=%.6f global_anchor=%.6f\n",
           H, W, Lg, sg);
    float q = QUANT;
    float rho_star = 4.0f * q / (4.0f * q * q + 1.0f);   /* exact inversion */
    printf("budget: seam artifact <= 1 code value = %.6f LDR (%.3f/255)\n",
           QUANT, 1.0f);
    printf("derived anchor tolerance: rho <= 4q/(4q^2+1) = %.4f%% "
           "(first-order 4/255 = %.4f%%; log-lum within +-%.4f)\n\n",
           100.0f * rho_star, 100.0f * 4.0f / CV,
           0.5f * logf((1.0f + rho_star / 2.0f) /
                       (1.0f - rho_star / 2.0f)));

    int tiers[3] = { 2, 4, 8 };
    for (int ti = 0; ti < 3; ti++) {
        int P = tiers[ti];
        int row[9];
        float Lw[8], s[8];
        for (int t = 0; t <= P; t++) row[t] = t * H / P;
        for (int t = 0; t < P; t++) {
            Lw[t] = log_avg(row[t], row[t + 1]);
            s[t] = KEY * Lw[t];
        }

        for (int r = 0; r < H; r++) {
            int t = r * P / H;
            for (int c = 0; c < W; c++) {
                loc[r][c] = reinhard(img[r][c], s[t]);
                dbl[r][c] = ref[r][c];
            }
        }
        /* overlap rows: mapped once in the worker, then again by the
         * compositing pass, which applies the curve in normalized LDR
         * space (anchor 1) */
        for (int k = 1; k < P; k++)
            for (int c = 0; c < W; c++) {
                int r = row[k];
                dbl[r][c] = reinhard(reinhard(img[r][c], sg), 1.0f);
            }

        printf("=== P=%d  rows_per_worker=%d  reduce_steps(tree)=%d ===\n",
               P, H / P, (int)(log((double)P) / log(2.0) + 0.5));
        for (int t = 0; t < P; t++)
            printf("  tile %d rows [%3d,%3d) log_avg=%.6f anchor=%.6f\n",
                   t, row[t], row[t + 1], Lw[t], s[t]);

        float local_max = 0.0f, dbl_max = 0.0f;
        int local_fails = 0, dbl_fails = 0;
        for (int k = 1; k < P; k++) {
            int r = row[k];
            float vis = 0.0f, jump = 0.0f, dd = 0.0f;
            int worst_c = 0;
            for (int c = 0; c < W; c++) {
                /* extra gradient across the seam beyond the content
                 * gradient that the global reference already has */
                float e = fabsf((loc[r][c] - loc[r - 1][c]) -
                                (ref[r][c] - ref[r - 1][c]));
                /* same radiance mapped with the two neighbouring anchors:
                 * this is the quantity the closed form predicts */
                float a = fabsf(reinhard(img[r][c], s[k]) -
                                reinhard(img[r][c], s[k - 1]));
                float m = fabsf(dbl[r][c] - ref[r][c]);
                if (e > vis) vis = e;
                if (a > jump) jump = a;
                if (m > dd) { dd = m; worst_c = c; }
            }
            float s_mean = 0.5f * (s[k] + s[k - 1]);
            float rho = fabsf(s[k] - s[k - 1]) / s_mean;
            int lfail = vis * CV > 1.0f;
            int dfail = dd * CV > 1.0f;
            local_max = fmaxf(local_max, vis);
            dbl_max = fmaxf(dbl_max, dd);
            local_fails += lfail;
            dbl_fails += dfail;

            printf("  seam row=%3d rho(top vs bot)=%7.4f%%  "
                   "local_vis=%6.3f cv anchor_jump=%6.3f cv  %s\n",
                   r, 100.0f * rho, vis * CV, jump * CV,
                   lfail ? "FAIL" : "pass");
            if (lfail)
                printf("    DIAG P=%d seam_row=%d Lw_top=%.6f "
                       "Lw_bottom=%.6f rho=%.4f%% measured=%.3f cv\n",
                       P, r, Lw[k - 1], Lw[k], 100.0f * rho, vis * CV);
            /* exact worst-case bound over u:
             *   dT_max(rho) = rho / [2(1 + sqrt(1 - rho^2/4))]
             * which tends to rho/4 as rho -> 0 (the first-order bound) */
            float exact_bound = rho /
                (2.0f * (1.0f + sqrtf(fmaxf(0.0f, 1.0f - rho * rho / 4.0f))));
            check("bound_rho_exact", jump <= exact_bound + 1e-6f);
            check("bound_rho_first_order_limit",
                  rho < 0.05f ? jump <= rho / 4.0f + 1e-6f : 1);

            float ux = img[r][worst_c];
            printf("    double_map=%6.3f cv (worst col=%d x=%.3f u=%.3f)  "
                   "%s\n", dd * CV, worst_c, ux, ux / sg,
                   dfail ? "FAIL" : "pass");
            if (dfail)
                printf("    DIAG P=%d seam_row=%d double_mapped=%.3f cv "
                       "x=%.3f (single-map ref=%.6f, double=%.6f)\n",
                       P, r, dd * CV, ux, ref[r][worst_c], dbl[r][worst_c]);
        }
        printf("  SUMMARY P=%d local_max=%.3f cv fails=%d  "
               "double_max=%.3f cv fails=%d  global_max=0.000 cv fails=0\n\n",
               P, local_max * CV, local_fails, dbl_max * CV, dbl_fails);
        if (csv)
            fprintf(csv, "%d,%d,%.4f,%d,%.4f,%d,0.0000,%d,%d\n", P, P - 1,
                    local_max * CV, local_fails, dbl_max * CV, dbl_fails,
                    (int)(log((double)P) / log(2.0) + 0.5), H / P);
    }

    /* ---- analytic self-tests ------------------------------------------------ */
    check("reinhard_zero", reinhard(0.0f, 1.0f) == 0.0f);
    check("reinhard_monotone_x",
          reinhard(1.0f, 1.0f) < reinhard(2.0f, 1.0f));
    check("reinhard_decreasing_s",
          reinhard(1.0f, 1.0f) > reinhard(1.0f, 1.1f));

    /* exact closed form: x|sb-sa| / ((sa+x)(sb+x)) */
    for (int i = 1; i <= 5; i++) {
        float x = 0.2f * i, sa = 1.0f, sb = 1.0f + 0.03f * i;
        float direct = fabsf(reinhard(x, sa) - reinhard(x, sb));
        float formula = x * fabsf(sb - sa) / ((sa + x) * (sb + x));
        check("stats_jump_formula", fabsf(direct - formula) < 1e-6f);
    }

    /* g(u) = u/(1+u)^2 peaks at 1/4 (u = 1) */
    float peak = 0.0f;
    for (float u = 0.05f; u < 20.0f; u *= 1.25f)
        peak = fmaxf(peak, u / (1.0f + u) / (1.0f + u));
    check("g_peak_quarter", fabsf(peak - 0.25f) < 0.01f);

    /* threshold inversion at u = 1: rho 1% passes, rho 2% fails 1 cv */
    float u1 = 1.0f, x = u1;
    float d1 = x * 0.01f / ((1.0f + x) * (1.01f + x));
    float d2 = x * 0.02f / ((1.0f + x) * (1.02f + x));
    check("rho_1pct_within_one_cv", d1 * CV <= 1.0f);
    check("rho_2pct_exceeds_one_cv", d2 * CV > 1.0f);
    check("rho_boundary_one_cv",
          fabsf((0.25f * 4.0f / CV) * CV - 1.0f) < 1e-4f);

    /* double mapping is strictly darker, small-u loss ~ u^2 */
    float u = 0.05f, xx = sg * u;
    float once = reinhard(xx, sg), twice = reinhard(once, 1.0f);
    check("double_strictly_darker", twice < once);
    float exact = u * u / ((1.0f + u) * (1.0f + 2.0f * u));
    check("double_small_u_u_sq", fabsf((once - twice) - exact) < 1e-6f &&
                                 exact < u * u);

    /* uniform content: identical anchors -> no seam */
    check("uniform_no_jump",
          fabsf(reinhard(0.7f, 1.0f) - reinhard(0.7f, 1.0f)) == 0.0f);

    if (csv) fclose(csv);
    printf("all_ok=%d\n", failures == 0);
    return failures ? 1 : 0;
}
