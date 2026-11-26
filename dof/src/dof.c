
/* dof: circle-of-confusion from depth and focus, with blur radius. */
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct {
    float focus;      /* focus distance */
    float aperture;   /* aperture diameter */
    float focal;      /* focal length */
    float near_limit; /* sensor near limit */
} Lens;

/* thin lens CoC in sensor units */
static float coc(const Lens* l, float depth) {
    if (depth <= 0.0f) return -1.0f;
    float d = depth;
    float num = l->aperture * l->focal * (d - l->focus);
    float den = d * (l->focus - l->focal);
    if (fabsf(den) < 1e-9f) return -1.0f;
    return fabsf(num / den);
}

/* blur radius in pixels = coc * sensor_to_pixels, clamped */
static float blur_radius(const Lens* l, float depth, float k, float max_r) {
    float c = coc(l, depth);
    if (c < 0.0f) return -1.0f;
    float r = c * k;
    if (r > max_r) r = max_r;
    return r;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Lens l = { 5.0f, 2.0f, 50.0f, 0.1f };

    check("at_focus_zero", fabsf(coc(&l, 5.0f)) < 1e-6f);
    check("near_blurs", coc(&l, 2.0f) > 0.0f);
    check("far_blurs", coc(&l, 50.0f) > 0.0f);

    /* blur grows as we move away from focus */
    check("monotone_near", coc(&l, 4.0f) < coc(&l, 2.0f));
    check("monotone_far", coc(&l, 10.0f) < coc(&l, 50.0f));

    /* smaller aperture means less blur */
    Lens sharp = { 5.0f, 0.5f, 50.0f, 0.1f };
    check("aperture_matters", coc(&sharp, 2.0f) < coc(&l, 2.0f));

    check("radius_positive", blur_radius(&l, 2.0f, 10.0f, 100.0f) > 0.0f);
    check("radius_clamped", fabsf(blur_radius(&l, 2.0f, 1000.0f, 8.0f) - 8.0f) < 1e-5f);
    check("radius_zero_at_focus", fabsf(blur_radius(&l, 5.0f, 10.0f, 100.0f)) < 1e-6f);

    check("bad_depth", coc(&l, 0.0f) < 0.0f);
    check("bad_radius_depth", blur_radius(&l, -1.0f, 10.0f, 100.0f) < 0.0f);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
