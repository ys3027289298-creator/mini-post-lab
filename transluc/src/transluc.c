/* transluc: transparency vs depth-of-field ordering conflict.
 *
 * One pixel covered by a stack of n translucent layers over an opaque
 * background. Measures:
 *   - error extent of the two naive pipeline orders:
 *       A (dof-then-blend): translucent layers are never defocused
 *       B (blend-then-dof): background seen through a front layer is
 *                           blurred with the front layer's CoC
 *   - see-through count P(b): layer pairs whose compositing order is
 *     wrong or ambiguous when depth is quantized to b bits
 *   - bandwidth W(b) = n*b bits/pixel, to pick min b with P(b) = 0
 *
 * Zero see-through requires the quantizer to be strictly order-preserving
 * on the layer depths: (Z_FAR - Z_NEAR) / (2^b - 1) < min pairwise gap.
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define MAX_LAYERS 64
#define Z_NEAR 1.0
#define Z_FAR 100.0
#define COC_EPS 1e-9

typedef struct {
    double focus;    /* focus distance, m */
    double aperture; /* aperture diameter, m */
    double focal;    /* focal length, m */
} Lens;

/* thin-lens circle of confusion, same model as dof/ */
static double coc(const Lens* l, double z) {
    double den = z * (l->focus - l->focal);
    if (fabs(den) < 1e-12) return 0.0;
    return fabs(l->aperture * l->focal * (z - l->focus) / den);
}

static uint64_t rng_s;
static void rng_seed(uint64_t s) { rng_s = s; }
static double rng_depth(void) {
    rng_s = rng_s * 6364136223846793005ULL + 1442695040888963407ULL;
    double u = (rng_s >> 11) * (1.0 / 9007199254740992.0);
    return Z_NEAR + (Z_FAR - Z_NEAR) * u;
}

/* uniform quantization of depth to b bits */
static uint64_t depth_key(double z, int bits) {
    double t = (z - Z_NEAR) / (Z_FAR - Z_NEAR);
    double levels = (double)((1ULL << bits) - 1);
    return (uint64_t)(t * levels + 0.5);
}

/* stable sort of layer indices by key, far to near */
static void sort_back_to_front(const uint64_t* key, int* idx, int n) {
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && key[idx[j]] < key[v]) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
}

static double min_gap(const double* z, int n) {
    double g = Z_FAR - Z_NEAR;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            double d = fabs(z[i] - z[j]);
            if (d < g) g = d;
        }
    return g;
}

/* pairs (i behind j) composited in the wrong or an ambiguous order */
static int count_penetrations(const double* z, const uint64_t* key,
                              const int* order, int n,
                              int* front_id, int* back_id) {
    int pos[MAX_LAYERS];
    for (int p = 0; p < n; p++) pos[order[p]] = p;
    int count = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (i == j || z[i] <= z[j]) continue; /* i strictly behind j */
            if (pos[i] > pos[j] || key[i] == key[j]) {
                if (count == 0) { *front_id = j; *back_id = i; }
                count++;
            }
        }
    return count;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Lens lens = { 5.0, 0.1, 0.05 };
    const int tiers[] = { 4, 16, 64 };
    const int precisions[] = { 8, 16, 24, 32 };
    const int n_prec = 4;
    int pen_total[4] = { 0, 0, 0, 0 };
    int pen8_deep = 0;

    for (int t = 0; t < 3; t++) {
        int n = tiers[t];
        double z[MAX_LAYERS];
        rng_seed(1234 + (uint64_t)n);
        for (int i = 0; i < n; i++) z[i] = rng_depth();

        double g = min_gap(z, n);
        int rb = (int)ceil(log2((Z_FAR - Z_NEAR) / g + 1.0));
        printf("tier=%d layers=%d gmin=%.6f required_bits=%d\n", n, n, g, rb);

        /* error extent of the two naive orders on this stack */
        int err_a = 0, err_b = 0;
        for (int i = 0; i < n; i++)
            if (coc(&lens, z[i]) > COC_EPS) err_a++;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (i != j && z[i] > z[j] &&
                    fabs(coc(&lens, z[i]) - coc(&lens, z[j])) > COC_EPS)
                    err_b++;
        printf("order_a_unblurred_layers tier=%d count=%d\n", n, err_a);
        printf("order_b_bleeding_pairs tier=%d count=%d\n", n, err_b);

        for (int p = 0; p < n_prec; p++) {
            int b = precisions[p];
            uint64_t key[MAX_LAYERS];
            int order[MAX_LAYERS];
            for (int i = 0; i < n; i++) key[i] = depth_key(z[i], b);
            sort_back_to_front(key, order, n);
            int front = -1, back = -1;
            int pen = count_penetrations(z, key, order, n, &front, &back);
            pen_total[p] += pen;
            printf("tier=%d bits=%d penetrations=%d bandwidth_bits=%d\n",
                   n, b, pen, n * b);
            if (pen > 0)
                printf("violation tier=%d bits=%d front_id=%d back_id=%d\n",
                       n, b, front, back);
            if (n == 64 && b == 8) pen8_deep = pen;
        }

        /* zero see-through must hold at the solved precision and at full precision */
        uint64_t key[MAX_LAYERS];
        int order[MAX_LAYERS];
        int front, back;
        for (int i = 0; i < n; i++) key[i] = depth_key(z[i], rb);
        sort_back_to_front(key, order, n);
        int pen_rb = count_penetrations(z, key, order, n, &front, &back);
        char name[64];
        snprintf(name, sizeof name, "zero_seethrough_at_required_bits_t%d", n);
        check(name, pen_rb == 0);
    }

    /* the failure the review reported must reproduce at 8 bits, 64 layers */
    check("expect_8bit_penetrates_64", pen8_deep > 0);
    check("zero_seethrough_at_full_precision_all", pen_total[3] == 0);

    /* cheapest precision with zero see-through across all tiers */
    int chosen = 32;
    for (int p = 0; p < n_prec; p++)
        if (pen_total[p] == 0) { chosen = precisions[p]; break; }
    printf("chosen_bits=%d\n", chosen);
    check("cheaper_8bit_insufficient", pen_total[0] > 0);
    check("chosen_is_16bit", chosen == 16);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
