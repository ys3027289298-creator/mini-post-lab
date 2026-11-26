
/* xplat_test: model PC vs console post-processing differences.
 *
 * Two platform profiles differ along three independent axes:
 *   - compute precision: fp32 vs fp16 rounding after each ALU step
 *   - default gamma:      out = pow(tonemapped, 1/gamma), gamma per platform
 *   - intermediate buffer: R16F vs R11G11B10F vs RGBA8 round-trip
 *
 * A test image of three known-luminance blocks (low / mid / high) runs
 * through exposure -> ACES -> gamma -> buffer round-trip. Outputs are
 * compared in 8-bit code values: one quantization level q = 1/255.
 * Pass criterion: every block differs by at most one code value.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ---------------- fp16 (binary16: 1 sign / 5 exp / 10 mant) ---------------- */

static uint16_t f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof x);
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t exp8 = (x >> 23) & 0xffu;
    uint32_t m = x & 0x7fffffu;

    if (exp8 == 0xffu) /* inf / nan */
        return (uint16_t)(sign | (m ? 0x7e00u : 0x7c00u));

    int e = (int)exp8 - 127;
    if (e >= 16) return (uint16_t)(sign | 0x7c00u); /* overflow -> inf */
    if (e >= -14) {                                  /* normal, RNE */
        uint32_t r = m + 0xfffu + ((m >> 13) & 1u);
        if (r & 0x800000u) { e++; r = 0; }           /* mantissa carry */
        if (e >= 16) return (uint16_t)(sign | 0x7c00u);
        return (uint16_t)(sign | ((uint32_t)(e + 15) << 10) | ((r >> 13) & 0x3ffu));
    }
    if (e < -24) return (uint16_t)sign;              /* underflow -> signed zero */

    uint32_t mm = m | 0x800000u;                     /* subnormal, RNE */
    int shift = -e - 1;
    uint32_t r = mm + (1u << (shift - 1)) - 1u + ((mm >> shift) & 1u);
    return (uint16_t)(sign | (r >> shift));
}

static float f16_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16;
    uint32_t e = (h >> 10) & 0x1fu;
    uint32_t m = h & 0x3ffu;
    uint32_t f;
    if (e == 0) {
        if (m == 0) { f = sign; }
        else {
            while (!(m & 0x400u)) { m <<= 1; e--; }
            m &= 0x3ffu;
            e++;
            f = sign | ((e + 127 - 15) << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = sign | 0x7f800000u | (m << 13);
    } else {
        f = sign | ((e + (127 - 15)) << 23) | (m << 13);
    }
    float out;
    memcpy(&out, &f, sizeof out);
    return out;
}

static float f16_rt(float f) { return f16_to_f32(f32_to_f16(f)); }

/* ulp of the fp16 grid nearest to x (one bit-step in the f16 encoding) */
static float ulp16(float x) {
    uint16_t h = f32_to_f16(x);
    float q = f16_to_f32(h);
    float q1 = f16_to_f32((uint16_t)(h + 1)); /* monotone encoding for +x */
    return q1 - q;
}

/* ----------- unsigned float used by R11G11B10F (6 exp, 5/4 mant) ----------- */

static uint32_t pack_ufloat(float f, int mbits) {
    if (!(f >= 0.0f)) return 0;                      /* NaN / negative -> 0 */
    uint32_t x;
    memcpy(&x, &f, sizeof x);
    uint32_t exp8 = (x >> 23) & 0xffu;
    uint32_t m = x & 0x7fffffu;
    const int bias = 15, ebits = 6;
    const uint32_t emax_field = (1u << (ebits - 1)) - 2u; /* max finite exponent field, 30 */
    uint32_t mmask = (1u << mbits) - 1u;
    const uint32_t max_packed = (emax_field << mbits) | mmask;

    if (exp8 == 0xffu)                               /* inf / nan -> max finite */
        return max_packed;

    int e = (int)exp8 - 127;
    if (e > (int)emax_field - bias)
        return max_packed;

    if (e >= -14) {                                  /* normal, RNE */
        uint32_t guard = 1u << (23 - mbits - 1);
        uint32_t r = m + guard - 1u + ((m >> (23 - mbits)) & 1u);
        if (r & 0x800000u) { e++; r = 0; }
        if (e > (int)emax_field - bias)
            return max_packed;
        return ((uint32_t)(e + bias) << mbits) | ((r >> (23 - mbits)) & mmask);
    }

    int shift = 9 - e - mbits;                       /* subnormal, RNE */
    if (shift > 31) return 0;
    uint32_t mm = m | 0x800000u;
    uint32_t r = mm + (1u << (shift - 1)) - 1u + ((mm >> shift) & 1u);
    return (r >> shift) & mmask;
}

static float unpack_ufloat(uint32_t p, int mbits) {
    uint32_t e = (p >> mbits) & 0x3fu;
    uint32_t m = p & ((1u << mbits) - 1u);
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    if (e == 0x3fu) e = 0x3eu;                       /* reserved -> max finite */
    return ldexpf((float)((1u << mbits) | m), (int)e - 15 - mbits);
}

static float r11f_rt(float f) { return unpack_ufloat(pack_ufloat(f, 5), 5); }

/* ulp of the r11f grid nearest to x (one bit-step in the packed encoding) */
static float ulp11f(float x) {
    uint32_t p = pack_ufloat(x, 5);
    return unpack_ufloat(p + 1, 5) - unpack_ufloat(p, 5);
}

/* ------------------------------- pipeline ---------------------------------- */

enum { BUF_R16F = 0, BUF_R11F = 1, BUF_RGBA8 = 2 };

typedef struct {
    const char *name;
    int fp16_compute;     /* round every ALU result to binary16 */
    float gamma;          /* output encode exponent 1/gamma */
    int buffer;           /* intermediate buffer format */
} Profile;

static float aces(float x) {
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (x * (a * x + b)) / (x * (c * x + d) + e);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

static float buffer_rt(int buffer, float v) {
    switch (buffer) {
    case BUF_R16F:  return f16_rt(v);
    case BUF_R11F:  return r11f_rt(v);
    case BUF_RGBA8: {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return lroundf(v * 255.0f) / 255.0f;
    }
    }
    return v;
}

/* exposure -> ACES -> gamma encode -> intermediate buffer round-trip */
static float pipeline(const Profile *p, float L, float exposure) {
    float v = L * exposure;
    if (p->fp16_compute) v = f16_rt(v);

    float t = aces(v);
    if (p->fp16_compute) t = f16_rt(t);

    float g = powf(t, 1.0f / p->gamma);
    if (p->fp16_compute) g = f16_rt(g);

    return buffer_rt(p->buffer, g);
}

/* same chain but no buffer round-trip: continuous output for sensitivity work */
static float pipeline_cont(const Profile *p, float L, float exposure) {
    float v = L * exposure;
    if (p->fp16_compute) v = f16_rt(v);
    float t = aces(v);
    if (p->fp16_compute) t = f16_rt(t);
    float g = powf(t, 1.0f / p->gamma);
    if (p->fp16_compute) g = f16_rt(g);
    return g;
}

static int code8(float g) {
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    return (int)lroundf(g * 255.0f);
}

/* ------------------------------- test cases -------------------------------- */

#define NBLOCKS 3
static const float block_L[NBLOCKS] = { 0.05f, 0.5f, 4.0f };
static const char *block_name[NBLOCKS] = { "low", "mid", "high" };

typedef struct {
    const char *name;
    Profile pc, con;
} Scenario;

static void run_scenario(const Scenario *s, float exposure) {
    printf("== %s ==\n", s->name);
    printf("  pc : fp%d compute, gamma=%.2f, buffer=%s\n",
           s->pc.fp16_compute ? 16 : 32, s->pc.gamma,
           s->pc.buffer == BUF_R16F ? "R16F" :
           s->pc.buffer == BUF_R11F ? "R11G11B10F" : "RGBA8");
    printf("  con: fp%d compute, gamma=%.2f, buffer=%s\n",
           s->con.fp16_compute ? 16 : 32, s->con.gamma,
           s->con.buffer == BUF_R16F ? "R16F" :
           s->con.buffer == BUF_R11F ? "R11G11B10F" : "RGBA8");

    int worst = 0, worst_dq = 0;
    int all_pass = 1;
    for (int i = 0; i < NBLOCKS; i++) {
        float gp = pipeline(&s->pc, block_L[i], exposure);
        float gc = pipeline(&s->con, block_L[i], exposure);
        int cp = code8(gp), cc = code8(gc);
        int dq = cc > cp ? cc - cp : cp - cc;
        if (dq > worst_dq) { worst_dq = dq; worst = i; }
        if (dq > 1) all_pass = 0;
        printf("  %-4s L=%-4.2f  pc=%3d (%.5f)  con=%3d (%.5f)  dq=%2d %s\n",
               block_name[i], block_L[i], cp, gp, cc, gc, dq,
               dq <= 1 ? "ok" : "FAIL");
    }
    printf("  verdict: %s", all_pass ? "PASS (all blocks within 1 q-level)\n"
                                     : "FAIL\n");
    if (!all_pass) {
        printf("  worst block: %s (L=%.2f), dq=%d q-levels\n",
               block_name[worst], block_L[worst], worst_dq);
        printf("  pc params : fp%d gamma=%.2f buffer=%d | con params: fp%d gamma=%.2f buffer=%d\n",
               s->pc.fp16_compute ? 16 : 32, s->pc.gamma, s->pc.buffer,
               s->con.fp16_compute ? 16 : 32, s->con.gamma, s->con.buffer);
    }
    printf("\n");
}

/* -------- invert the one-q-level budget against each free parameter -------- */

static void budget(const Profile *ref, float exposure) {
    const float tau = 1.0f / 255.0f;
    printf("== parameter budget (tau = 1 q-level = %.6f, reference gamma=%.2f) ==\n",
           tau, ref->gamma);
    printf("  block   dO/dE     dE_abs    dE/E      dO/dg     dg_abs    dg/g\n");
    for (int i = 0; i < NBLOCKS; i++) {
        float L = block_L[i];
        float hE = exposure * 1e-3f;
        float dOdE = (pipeline_cont(ref, L, exposure + hE) -
                      pipeline_cont(ref, L, exposure - hE)) / (2 * hE);

        Profile up = *ref, dn = *ref;
        float hg = ref->gamma * 1e-3f;
        up.gamma += hg;
        dn.gamma -= hg;
        float dOdg = (pipeline_cont(&up, L, exposure) -
                      pipeline_cont(&dn, L, exposure)) / (2 * hg);

        if (fabsf(dOdE) < 1e-4f || fabsf(dOdg) < 1e-4f) {
            printf("  %-5s  %9.5f  saturated: tonemap shoulder, budget unbounded on this block\n",
                   block_name[i], dOdE);
            continue;
        }
        float dE = tau / fabsf(dOdE);
        float dg = tau / fabsf(dOdg);
        printf("  %-5s  %9.5f %9.5f %8.4f  %9.5f %9.5f %8.4f\n",
               block_name[i], dOdE, dE, dE / exposure,
               dOdg, dg, dg / ref->gamma);
    }
    printf("\n");
}

int main(void) {
    const float exposure = 1.0f;
    const Profile pc_ref = { "pc",  0, 2.2f, BUF_R16F };

    /* codec self-checks on known encodings */
    int codec_ok = 1;
    codec_ok &= f32_to_f16(0.0f) == 0x0000;
    codec_ok &= f32_to_f16(1.0f) == 0x3c00;
    codec_ok &= f32_to_f16(-1.0f) == 0xbc00;
    codec_ok &= f32_to_f16(2.0f) == 0x4000;
    codec_ok &= f32_to_f16(65504.0f) == 0x7bff;   /* max fp16 */
    codec_ok &= f32_to_f16(1e9f) == 0x7c00;       /* overflow -> inf */
    codec_ok &= f16_to_f32(0x3c00) == 1.0f;
    codec_ok &= fabsf(f16_rt(0.1f) - 0.0999755859375f) < 1e-9f;
    codec_ok &= f16_rt(0.1f) == f16_to_f32(f32_to_f16(0.1f));
    codec_ok &= unpack_ufloat(pack_ufloat(1.0f, 5), 5) == 1.0f;
    codec_ok &= pack_ufloat(1.0f, 5) == 0x1e0u;       /* 1.0 = exp field 15 << 5 */
    codec_ok &= unpack_ufloat(1, 5) == ldexpf(1.0f, -19); /* min r11f subnormal */
    codec_ok &= fabsf(unpack_ufloat(0x1eu << 5 | 0x1f, 5) - 64512.0f) < 1e-3f;
    codec_ok &= pack_ufloat(64512.0f, 5) == (0x1eu << 5 | 0x1f); /* max finite, exact */
    codec_ok &= pack_ufloat(65504.0f, 5) == (0x1eu << 5 | 0x1f); /* RNE carry -> clamp */
    codec_ok &= pack_ufloat(1e9f, 5) == (0x1eu << 5 | 0x1f);     /* far overrange -> clamp */
    printf("== codec self-check: %s ==\n\n", codec_ok ? "PASS" : "FAIL");
    if (!codec_ok) return 1;

    Scenario scenarios[] = {
        { "precision only: fp32 vs fp16",
          pc_ref,
          { "con", 1, 2.2f, BUF_R16F } },

        { "gamma default mismatch: 2.2 vs 2.4",
          pc_ref,
          { "con", 0, 2.4f, BUF_R16F } },

        { "gamma default mismatch: 2.2 vs 1.8 (console brighter -> highlight clip)",
          pc_ref,
          { "con", 0, 1.8f, BUF_R16F } },

        { "buffer format only: R16F vs R11G11B10F",
          pc_ref,
          { "con", 0, 2.2f, BUF_R11F } },

        { "as-shipped combination: fp16 + gamma 1.8 + R11F",
          pc_ref,
          { "con", 1, 1.8f, BUF_R11F } },

        { "unified profile: matched gamma, R16F on both (fp16 allowed)",
          { "pc",  1, 2.2f, BUF_R16F },
          { "con", 1, 2.2f, BUF_R16F } },
    };
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
        run_scenario(&scenarios[i], exposure);

    budget(&pc_ref, exposure);

    /* raw ulp of each format's grid at representative magnitudes */
    printf("== format grid step (ulp) check ==\n");
    float probes[] = { 0.05f, 0.5f, 4.0f };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        float x = probes[i];
        printf("  x=%.2f  fp16 ulp=%.3e (rel %.2e)  r11f ulp=%.3e (rel %.2e)  rgba8 step=%.3e (rel %.2e)\n",
               x, ulp16(x), ulp16(x) / x,
               ulp11f(x), ulp11f(x) / x,
               1.0f / 255.0f, (1.0f / 255.0f) / x);
    }
    return 0;
}
