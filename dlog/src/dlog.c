/* dlog: run-log emitter + repeated-execution audit for the
 * tonemap -> bloom -> dof -> present chain.
 *
 * Reading used here, declared up front because the overnight parameter
 * change left no record:
 *
 *   - pass m declares an input domain D_m and an output range R_m, both
 *     intervals over code values [0,1].  The overlap criterion is
 *         overlap(m)  <=>  D_m n R_m =/= {}  <=>  d_lo <= r_hi
 *     (d_hi >= r_lo holds for every pass here, so the intersection is
 *     [max(d_lo, r_lo), min(d_hi, r_hi)]).
 *   - That criterion is NECESSARY, not sufficient.  Every pass that can
 *     read a value it could also have written trips it -- all four do.
 *     The sufficient form is order: an execution of pass m consumes the
 *     output of an earlier execution of the same pass, i.e. in_b in R_a.
 *   - A pass run n times composes its curve n times: y = f^n(x).  Reinhard
 *     has the closed form  f^n(x) = x / (1 + n x), so
 *         n = (x - y) / (x y)
 *     recovers the repetition count from output luminance alone -- no
 *     pass line is needed for the count.
 *   - Pass criterion: clean iff |y - f(x)| <= q, q = 1/1023, one
 *     quantization level of a 10-bit present, where f is the INTENDED
 *     chain (each pass once).
 *   - dof and present do not change the value.  Repeating either moves no
 *     pixel, so no luminance test can see it; only a per-pass line can
 *     name it, and it is benign once named.
 *
 * The run header logs tier/src/q/final only -- deliberately no claim about
 * which passes ran, because that claim is exactly what was lost overnight.
 * Tiers are how many pass lines the log carries:
 *   lo  = present only              count recoverable; no pass-level evidence
 *   mid = tonemap, bloom, present   count + the value-changing culprit with
 *                                   its observed intervals
 *   hi  = every pass                count + every pass, including the identity
 *                                   repeat
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define Q (1.0f / 1023.0f)
#define SRC 0.9f
#define BLOOM_T 0.3f

typedef enum { K_T, K_B, K_D, K_O, K_NK } Kind;

static const char* NAME[K_NK]   = { "T", "B", "D", "O" };
static const char* MODULE[K_NK] = { "tonemap", "bloom", "dof", "present" };
/* declared input domain and output range, code units */
static const float DOM_LO[K_NK] = { 0.0f, BLOOM_T, 0.0f, 0.0f };
static const float DOM_HI[K_NK] = { 4.0f, 1.0f, 1.0f, 1.0f };
static const float RNG_HI[K_NK] = { 0.80f, 0.70f, 1.0f, 1.0f };

static float map(Kind k, float x) {
    switch (k) {
    case K_T: { float v = x; return v / (1.0f + v); }
    case K_B: { float v = x - BLOOM_T; return v > 0.0f ? v : 0.0f; }
    default:  return x;                 /* D and O do not change the value */
    }
}

/* Whether a pass can move a pixel; identity passes make repeats invisible
 * to any luminance test. */
static int changes_value(Kind k) { return fabsf(map(k, 0.5f) - 0.5f) > Q; }

/* Preimage of a logged output; NAN where the map is not invertible. */
static float unmap(Kind k, float y) {
    switch (k) {
    case K_T: return (y <= 0.0f || y >= 1.0f) ? NAN : y / (1.0f - y);
    case K_B: return y <= 0.0f ? NAN : y + BLOOM_T;
    default:  return y;
    }
}

/* Intended chain, recorded only in the emitted pass list and in the
 * analyzer's model -- never asserted by a header line. */
static const Kind INTENDED[] = { K_T, K_B, K_D, K_O };
#define N_INTENDED 4

static const Kind DUP[] = { K_T, K_T, K_B, K_D, K_D, K_O };
#define N_DUP 6

static int logged(Kind k, int tier) {
    if (tier == 2) return 1;
    if (tier == 1) return k != K_D;
    return k == K_O;
}

/* ---- emitter -------------------------------------------------------- */

typedef struct { Kind k; float in, out; } Exec;

static int emit(const char* path, int tier, int dup) {
    const Kind* list = dup ? DUP : INTENDED;
    int m = dup ? N_DUP : N_INTENDED;
    Exec ex[8];
    float x = SRC;
    for (int i = 0; i < m; i++) {
        ex[i].k = list[i];
        ex[i].in = x;
        x = map(list[i], x);
        ex[i].out = x;
    }
    FILE* f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fprintf(f, "# dlog tier=%d src=%.6f q=%.9f\n", tier, SRC, Q);
    for (int i = 0; i < m; i++) {
        if (!logged(ex[i].k, tier)) continue;
        fprintf(f, "%s in=%.4f out=%.4f dom=[%.4f,%.4f] rng=[0.0000,%.4f]\n",
                NAME[ex[i].k], ex[i].in, ex[i].out,
                DOM_LO[ex[i].k], DOM_HI[ex[i].k], RNG_HI[ex[i].k]);
    }
    fprintf(f, "final=%.9f\n", ex[m - 1].out);
    fclose(f);
    return 0;
}

/* ---- analyzer ------------------------------------------------------- */

#define MAX_EV 8

typedef struct {
    int tier;
    float src, q, fin;
    Exec ev[MAX_EV];
    int nev;
    long bytes;
    int total_lines;
} Run;

static int read_run(const char* path, Run* r) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[256];
    if (!fgets(line, sizeof line, f) ||
        sscanf(line, "# dlog tier=%d src=%f q=%f", &r->tier, &r->src, &r->q) != 3) {
        fclose(f); return -1;
    }
    r->fin = NAN;
    r->nev = 0;
    r->total_lines = 1;
    while (fgets(line, sizeof line, f)) {
        char nm[8];
        float in, out;
        r->total_lines++;
        if (sscanf(line, "final=%f", &r->fin) == 1) continue;
        if (r->nev >= MAX_EV) { fclose(f); return -1; }
        if (sscanf(line, "%7s in=%f out=%f", nm, &in, &out) != 3) continue;
        Kind k = nm[0] == 'T' ? K_T : nm[0] == 'B' ? K_B :
                 nm[0] == 'D' ? K_D : K_O;
        r->ev[r->nev].k = k;
        r->ev[r->nev].in = in;
        r->ev[r->nev].out = out;
        r->nev++;
    }
    fclose(f);
    struct stat st;
    r->bytes = (stat(path, &st) == 0) ? (long)st.st_size : -1;
    return 0;
}

/* Intended single map, the reference for the deviation test. */
static float apply_intended(float x, Kind dup_k, int dup_n) {
    for (int i = 0; i < N_INTENDED; i++) {
        if (INTENDED[i] == dup_k) {
            for (int j = 0; j < dup_n; j++) x = map(dup_k, x);
        } else {
            x = map(INTENDED[i], x);
        }
    }
    return x;
}

/* Closed form for a repeated Reinhard pass: unwind the passes after the
 * tonemap stage in the intended chain, then n = (x - y) / (x * y). */
static float tonemap_count(float src, float fin, float* y_out) {
    float y = fin;
    for (int i = N_INTENDED - 1; i > 0; i--) {
        y = unmap(INTENDED[i], y);
        if (isnan(y)) return NAN;
    }
    if (y <= 0.0f) return NAN;
    *y_out = y;
    return (src - y) / (src * y);
}

static int observed(const Run* r, Kind k, float* ilo, float* ihi,
                    float* olo, float* ohi) {
    int seen = 0;
    for (int i = 0; i < r->nev; i++) {
        if (r->ev[i].k != k) continue;
        if (!seen) {
            *ilo = *ihi = r->ev[i].in;
            *olo = *ohi = r->ev[i].out;
            seen = 1;
        } else {
            if (r->ev[i].in  < *ilo) *ilo = r->ev[i].in;
            if (r->ev[i].in  > *ihi) *ihi = r->ev[i].in;
            if (r->ev[i].out < *olo) *olo = r->ev[i].out;
            if (r->ev[i].out > *ohi) *ohi = r->ev[i].out;
        }
    }
    return seen;
}

/* Minimum repetitions of any value-changing pass that reproduce fin. */
static int value_culprit(float src, float fin, float q, Kind* which, int* reps) {
    for (int k = 0; k < K_NK; k++) {
        if (!changes_value((Kind)k)) continue;
        for (int c = 2; c <= 8; c++) {
            if (fabsf(apply_intended(src, (Kind)k, c) - fin) <= q) {
                *which = (Kind)k;
                *reps = c;
                return 1;
            }
        }
    }
    return 0;
}

static int clean_pass[3], dup_pass[3], lines_clean[3], lines_dup[3];
static long bytes_clean[3], bytes_dup[3];
static char attribution[3][80];

int main(void) {
    const char* path[3][2] = {
        { "logs/tier_lo_clean.log",  "logs/tier_lo_dup.log"  },
        { "logs/tier_mid_clean.log", "logs/tier_mid_dup.log" },
        { "logs/tier_hi_clean.log",  "logs/tier_hi_dup.log"  },
    };
    const char* tn[3] = { "lo", "mid", "hi" };
    int failures = 0;

    for (int t = 0; t < 3; t++) {
        if (emit(path[t][0], t, 0) || emit(path[t][1], t, 1)) failures++;
    }

    printf("=== per-tier audit (q = %.9f = 1/1023) ===\n\n", Q);

    for (int t = 0; t < 3; t++) {
        for (int mode = 0; mode < 2; mode++) {
            Run r;
            if (read_run(path[t][mode], &r) != 0) { failures++; continue; }

            float ref = apply_intended(r.src, K_O, 1);   /* no repeat */
            float dev = fabsf(r.fin - ref);
            int pass = dev <= r.q;

            printf("tier=%-3s run=%-5s pass_lines=%d total_lines=%d bytes=%ld\n",
                   tn[t], mode ? "dup" : "clean", r.nev, r.total_lines, r.bytes);
            printf("  final=%.6f intended-single-map=%.6f dev=%.6f (%.1f q) -> %s\n",
                   r.fin, ref, dev, dev / r.q, pass ? "PASS" : "EXCEEDS");

            if (mode == 0) {
                clean_pass[t] = pass;
                lines_clean[t] = r.nev;
                bytes_clean[t] = r.bytes;
                if (!pass) failures++;
            } else {
                dup_pass[t] = pass;
                lines_dup[t] = r.nev;
                bytes_dup[t] = r.bytes;
                if (pass) failures++;
            }
            if (mode == 0 || pass) { printf("\n"); continue; }

            /* ---- exceeded: diagnosis available at THIS tier ---- */

            float y = NAN;
            float n = tonemap_count(r.src, r.fin, &y);
            printf("  repetition count from luminance (no pass line needed):\n");
            printf("    y = f^n(x) = %.6f   n = (x-y)/(x*y) = "
                   "(%.4f-%.6f)/(%.4f*%.6f) = %.4f -> %d\n",
                   y, r.src, y, r.src, y, n, (int)lroundf(n));

            Kind culprit = K_NK;
            int reps = 0;
            if (value_culprit(r.src, r.fin, r.q, &culprit, &reps))
                printf("  culprit reproducing it within 1 q: %s(%s) x%d "
                       "(predicted %.6f)\n", NAME[culprit], MODULE[culprit],
                       reps, apply_intended(r.src, culprit, reps));

            printf("  repeated passes asserted by pass lines:");
            int any = 0;
            for (int k = 0; k < K_NK; k++) {
                if (k == K_O) continue;
                float ilo, ihi, olo, ohi;
                if (!observed(&r, (Kind)k, &ilo, &ihi, &olo, &ohi)) continue;
                /* two or more logged executions of k */
                int cnt = 0;
                for (int i = 0; i < r.nev; i++) if (r.ev[i].k == (Kind)k) cnt++;
                if (cnt < 2) continue;
                any++;
                printf("\n    %s(%s) x%d", NAME[k], MODULE[k], cnt);
                printf("\n      declared: in=[%.4f,%.4f] out=[0.0000,%.4f]"
                       "   D n R = [%.4f,%.4f]",
                       DOM_LO[k], DOM_HI[k], RNG_HI[k],
                       fmaxf(DOM_LO[k], 0.0f), fminf(DOM_HI[k], RNG_HI[k]));
                printf("\n      observed: in=[%.4f,%.4f] out=[%.4f,%.4f]"
                       "   D n R = [%.4f,%.4f]",
                       ilo, ihi, olo, ohi,
                       fmaxf(ilo, olo), fminf(ihi, ohi));
                if (!changes_value((Kind)k))
                    printf("\n      identity pass: repeat moves no pixel; "
                           "unreachable by luminance, nameable only here");
            }
            if (!any) printf(" none -- this tier logged no repeated pass");
            printf("\n");

            /* Sufficient form, only where pass lines exist. */
            printf("  sufficient form (same pass consumes its own output):");
            int found = 0;
            for (int b = 0; b < r.nev; b++)
                for (int a = 0; a < b; a++)
                    if (r.ev[a].k == r.ev[b].k && r.ev[a].k != K_O &&
                        fabsf(r.ev[b].in - r.ev[a].out) <= r.q) {
                        printf("\n    %s line %d in=%.4f consumes line %d out=%.4f",
                               NAME[r.ev[b].k], b + 1, r.ev[b].in,
                               a + 1, r.ev[a].out);
                        found++;
                        break;
                    }
            if (!found) printf(" none");
            printf("\n");

            printf("  necessary criterion alone: all %d passes trip D n R"
                   " -> order is what singles out the culprit\n", N_INTENDED);

            /* Attribution for the results table. */
            if (!any)
                snprintf(attribution[t], sizeof attribution[t],
                         "count=%d, culprit=%s, no intervals", (int)lroundf(n),
                         NAME[culprit]);
            else if (t == 2)
                snprintf(attribution[t], sizeof attribution[t],
                         "count=%d, T+D named w/ intervals", (int)lroundf(n));
            else
                snprintf(attribution[t], sizeof attribution[t],
                         "count=%d, T named w/ intervals, D unseen",
                         (int)lroundf(n));
            printf("  attribution here: %s\n\n", attribution[t]);
        }
    }

    printf("=== three-tier results (measured) ===\n");
    printf("%-4s %-8s %-6s %-9s %-6s %s\n",
           "tier", "clean", "lines", "dup", "lines", "duplicate attributed as");
    for (int t = 0; t < 3; t++)
        printf("%-4s %-8s %-6d %-9s %-6d %s\n", tn[t],
               clean_pass[t] ? "PASS" : "FAIL", lines_clean[t],
               dup_pass[t] ? "PASS" : "EXCEEDS", lines_dup[t], attribution[t]);

    /* ---- decision: two curves ---- */
    const int npass = N_DUP;
    const float bits = (float)npass / 8.0f;
    /* The one invented input: probability a reset site is missed or wrong.
     * Every other number below is measured. */
    const float dsite = 0.0015f;
    float marker_correct = powf(1.0f - dsite, (float)npass);

    printf("\n=== decision: two cost curves ===\n");
    printf("A log-only: correctness = 1 - r, measured overhead per tier; "
           "detects after the fact\n");
    printf("B marker:   correctness = (1-d)^%d = %.4f, independent of r; "
           "prevents\n", npass, marker_correct);
    printf("  (d = %.2f%% per reset site is the only assumed number)\n\n",
           dsite * 100.0f);

    printf("  r     | log-only | marker   | agreed\n");
    for (float rr = 0.005f; rr <= 0.051f; rr += 0.005f) {
        float lo_c = 1.0f - rr;
        int agree = fabsf(lo_c - marker_correct) < 0.002f ? 1 : 0;
        printf("  %-5.1f%% | %-8.4f | %-8.4f | %s\n", rr * 100.0f,
               lo_c, marker_correct, agree ? "y" : "n");
    }

    printf("\n  overhead, measured from the log files (incident run):\n");
    for (int t = 0; t < 3; t++)
        printf("    log %-3s %4ld B/frame  %s\n", tn[t], bytes_dup[t],
               attribution[t]);
    printf("    marker   %.2f B/frame  a flag per pass; records %d reset "
           "sites and cannot say which step fired\n", bits, npass);

    float cross = NAN;
    for (int rr = 1; rr <= 2000; rr++)
        if (1.0f - (float)rr / 1000.0f < marker_correct) {
            cross = (float)rr / 10.0f;
            break;
        }
    printf("\n  crossing: log-only is more correct below r = %.1f%%; "
           "marker is more correct above it.\n", cross);
    printf("  marker is also %.0fx cheaper than the lo log and %.0fx cheaper "
           "than hi.\n", (double)bytes_dup[0] / (double)bits,
           (double)bytes_dup[2] / (double)bits);

    printf("\n=== recommendation ===\n");
    printf("  default: a marker per pass (0.75 B/frame). Above r = %.1f%% it "
           "wins on correctness,\n", cross);
    printf("  and it wins on overhead at every r; its cost is the %d reset "
           "sites, not bytes.\n", npass);
    printf("  log-only only wins below r = %.1f%%, where the correctness "
           "curves are within 1%%.\n", cross);
    printf("  marker cannot name the step, so keep the lo log (%.0fx marker "
           "cost, %ld B)\n", (double)bytes_dup[0] / (double)bits, bytes_dup[0]);
    printf("  as the attribution channel: it recovers the count from luminance "
           "at every tier.\n");
    printf("  escalate to hi (%ld B, names the identity repeat) only while "
           "investigating.\n", bytes_dup[2]);

    printf("\nall_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
