/* gamut: wide-gamut (BT.2020) to narrow-gamut (BT.709) mapping, with the
 * clip-vs-compress tradeoff measured in CIEDE2000 and saturation.
 *
 * Pipeline: wide RGB -> narrow RGB through T = Mn^-1 * Mw.  Colours outside the
 * narrow cube are either per-channel clipped (the current behaviour) or pushed
 * inward along a constant-luminance ray (luminance-preserving compression), with
 * the compression strength solved from a colour-difference threshold. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define PI 3.14159265358979323846

#define NPIX 10000        /* 100x100 */
#define GRID 100

typedef double V3[3];

/* BT.2020 -> BT.709, derived from primaries + D65 (both share the white point,
 * so neutral grey survives unchanged). */
static const double T[3][3] = {
    {  1.660491002108, -0.587641138789, -0.072849863320 },
    { -0.124550474522,  1.132899897126, -0.008349422604 },
    { -0.018150763355, -0.100578898008,  1.118729661363 }
};
static const double MN[3][3] = {
    { 0.412453f, 0.357580f, 0.180423f },
    { 0.212671f, 0.715160f, 0.072169f },
    { 0.019334f, 0.119193f, 0.950227f },
};
static const double MW[3][3] = {
    { 0.636958048301, 0.144616903586, 0.168880975164 },
    { 0.262700212011, 0.677998071519, 0.059301716470 },
    { 0.000000000000, 0.028072693049, 1.060985057711 }
};
/* Lab reference white: column sums of MN (keeps Lab<->RGB exact at the cube faces). */
static double Xw, Yw, Zw;

static void mv(const double M[3][3], const V3 v, V3 out) {
    for (int i = 0; i < 3; i++) out[i] = M[i][0]*v[0] + M[i][1]*v[1] + M[i][2]*v[2];
}

static double lum_narrow(const V3 v) { return MN[1][0]*v[0] + MN[1][1]*v[1] + MN[1][2]*v[2]; }
static int in_gamut(const V3 v) { return v[0] >= 0 && v[0] <= 1 && v[1] >= 0 && v[1] <= 1 && v[2] >= 0 && v[2] <= 1; }

static void clipv(const V3 v, V3 out) {
    for (int i = 0; i < 3; i++) out[i] = v[i] < 0 ? 0 : (v[i] > 1 ? 1 : v[i]);
}

/* Distance outside the [0,1]^3 cube. */
static double excess(const V3 v) {
    double s = 0;
    for (int i = 0; i < 3; i++) {
        double d = (v[i] > 1) ? v[i] - 1 : (v[i] < 0 ? -v[i] : 0);
        s += d * d;
    }
    return sqrt(s);
}

/* Largest scale t with Y + t*(v-Y) still inside the cube; t>1 means v is inside.
 * Assumes Y in [0,1] and a non-zero chroma part. */
static double boundary_scale(const V3 v, double* Yout) {
    double Y = lum_narrow(v);
    if (Yout) *Yout = Y;
    double t = INFINITY;
    for (int i = 0; i < 3; i++) {
        double c = v[i] - Y;
        if (c > 1e-12) t = fmin(t, (1.0 - Y) / c);
        else if (c < -1e-12) t = fmin(t, (0.0 - Y) / c);
    }
    return t;
}

/* Per-pixel luminance-preserving compression. sigma 0 = leave alone (then clip),
 * sigma 1 = land exactly on the boundary. Intermediate values interpolate.
 * Y is preserved exactly (the chroma part has zero luminance). */
static void compress_pixel(const V3 v, double sigma, V3 out) {
    double Y, t = boundary_scale(v, &Y);
    if (t >= 1.0) { clipv(v, out); return; }
    double sb = t < 0 ? 0 : t;
    double s = 1.0 - sigma * (1.0 - sb);
    for (int i = 0; i < 3; i++) {
        double x = Y + s * (v[i] - Y);
        out[i] = x < 0 ? 0 : (x > 1 ? 1 : x);   /* residual only for sigma<1 */
    }
}

/* Global chroma squeeze: one factor g applied to every pixel. g<1 desaturates
 * the whole frame, which is the "evenly spread over the gamut" variant. */
static void compress_global(const V3 v, double g, V3 out) {
    double Y = lum_narrow(v);
    for (int i = 0; i < 3; i++) {
        double x = Y + g * (v[i] - Y);
        out[i] = x < 0 ? 0 : (x > 1 ? 1 : x);
    }
}

/* HSV-style saturation of a narrow RGB triple. */
static double sat_rgb(const V3 v) {
    double mx = fmax(v[0], fmax(v[1], v[2]));
    double mn = fmin(v[0], fmin(v[1], v[2]));
    return mx <= 1e-9 ? 0.0 : (mx - mn) / mx;
}

/* ---------------- CIELAB + CIEDE2000 ---------------- */
static double f_lab(double t) {
    return t > 0.0088564516 ? cbrt(t) : t / (3.0 * 0.1284185493 * 0.1284185493) + 4.0/29.0;
}
static void xyz2lab(const V3 xyz, double lab[3]) {
    double fx = f_lab(xyz[0]/Xw), fy = f_lab(xyz[1]/Yw), fz = f_lab(xyz[2]/Zw);
    lab[0] = 116*fy - 16;
    lab[1] = 500*(fx - fy);
    lab[2] = 200*(fy - fz);
}
static double de2000(const double a[3], const double b[3]) {
    double L1=a[0],a1=a[1],b1=a[2],L2=b[0],a2=b[1],b2=b[2];
    double C1=hypot(a1,b1), C2=hypot(a2,b2);
    double Cb=(C1+C2)/2;
    double Cb7=pow(Cb,7);
    double G=0.5*(1-sqrt(Cb7/(Cb7+pow(25.0,7))));
    double ap1=(1+G)*a1, ap2=(1+G)*a2;
    double Cp1=hypot(ap1,b1), Cp2=hypot(ap2,b2);
    double h1=atan2(b1,ap1)*180/PI; if (h1<0) h1+=360;
    double h2=atan2(b2,ap2)*180/PI; if (h2<0) h2+=360;
    double dL=L2-L1, dC=Cp2-Cp1, dh=h2-h1;
    if (dh>180) dh-=360; else if (dh<-180) dh+=360;
    double dH=2*sqrt(Cp1*Cp2)*sin(dh*PI/360);
    double Lb=(L1+L2)/2, Cs=(Cp1+Cp2)/2;
    double hb;
    if (fabs(h1-h2)<=180) hb=(h1+h2)/2;
    else if (h1+h2<360) hb=(h1+h2+360)/2;
    else hb=(h1+h2-360)/2;
    double Tt=1 - 0.17*cos((hb-30)*PI/180) + 0.24*cos(2*hb*PI/180)
             + 0.32*cos((3*hb+6)*PI/180) - 0.20*cos((4*hb-63)*PI/180);
    double SL=1+0.015*(Lb-50)*(Lb-50)/sqrt(20+(Lb-50)*(Lb-50));
    double SC=1+0.045*Cs, SH=1+0.015*Cs*Tt;
    double Cs7=pow(Cs,7);
    double RT=-sin(60*PI/180*exp(-pow((hb-275)/25,2)))*2*sqrt(Cs7/(Cs7+pow(25.0,7)));
    double x=dL/SL, y=dC/SC, z=dH/SH;
    return sqrt(x*x + y*y + z*z + RT*y*z);
}
static void lab_of_narrow(const V3 v, double lab[3]) { V3 x; mv(MN, v, x); xyz2lab(x, lab); }
static void lab_of_wide(const V3 v, double lab[3]) { V3 x; mv(MW, v, x); xyz2lab(x, lab); }

/* ---------------- deterministic synthetic content ----------------
 * The image is authored directly in the *narrow* space (so the out-of-gamut
 * fraction and excess are exact), then pushed back to wide RGB through T^-1 so
 * every source pixel is a legal, displayable-on-wide HDR colour. */
static V3 NPRIM[6];    /* narrow-space chromatic directions, zero narrow luminance */
static double NPINV[3][3];

static void inv3d(const double M[3][3], double out[3][3]) {
    double det = M[0][0]*(M[1][1]*M[2][2]-M[1][2]*M[2][1])
               - M[0][1]*(M[1][0]*M[2][2]-M[1][2]*M[2][0])
               + M[0][2]*(M[1][0]*M[2][1]-M[1][1]*M[2][0]);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double m[2][2]; int r = 0, c = 0;
            for (int rr = 0; rr < 3; rr++) if (rr != i) {
                for (int cc = 0; cc < 3; cc++) if (cc != j) { m[r][c] = M[rr][cc]; c++; }
                if (c == 2) { c = 0; r++; }
            }
            out[j][i] = ((i+j) & 1 ? -1.0 : 1.0) * (m[0][0]*m[1][1]-m[0][1]*m[1][0]) / det;
        }
}

static void build_narrow_hues(void) {
    V3 d;
    for (int k = 0; k < 3; k++) {
        V3 e = {0,0,0}; e[k] = 1;
        double Y = lum_narrow(e);
        for (int i = 0; i < 3; i++) d[i] = e[i] - Y;
        double n = sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        for (int i = 0; i < 3; i++) NPRIM[k][i] = d[i]/n;
    }
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) d[i] = NPRIM[k][i] + NPRIM[(k+1)%3][i];
        double Y = lum_narrow(d);
        for (int i = 0; i < 3; i++) d[i] -= Y;
        double n = sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        for (int i = 0; i < 3; i++) NPRIM[k+3][i] = d[i]/n;
    }
    inv3d(T, NPINV);
}

/* Scale along d that lands on the narrow cube boundary at luminance Y. */
static double boundary_scale_Y(double Y, const V3 d) {
    double s = INFINITY;
    for (int i = 0; i < 3; i++) {
        if (d[i] > 1e-12) s = fmin(s, (1.0 - Y)/d[i]);
        else if (d[i] < -1e-12) s = fmin(s, (0.0 - Y)/d[i]);
    }
    return s;
}

/* Narrow-space pixel placed at relative excess `e` beyond the boundary, then
 * mapped to a wide source colour. Falls back to the neutral of the same
 * luminance if the wide source would leave the wide cube (rare).
 *
 * Tile classes (so global compression has in-gamut colour to visibly dull):
 *   [0, frac)              out-of-gamut saturated
 *   [frac, frac+0.40)      in-gamut saturated (sits just inside the boundary)
 *   [frac+0.40, 1)         neutral ramp                                     */
#define IN_GAMUT_FRAC 0.40

static void make_wide_pixel(int x, int y, double frac, double e, V3 out) {
    int tx = x/4, ty = y/4;
    int h = (tx*31 + ty*57 + 7) % 211;
    double u = h/211.0;
    if (u < frac + IN_GAMUT_FRAC) {
        int hue = h % 6;
        double Y = 0.25 + 0.45 * ((h % 5) / 4.0);
        double rel = (u < frac) ? (1.0 + e) : 0.90;   /* overshoot vs just-inside */
        double s = boundary_scale_Y(Y, NPRIM[hue]) * rel;
        V3 vn, vw;
        for (int i = 0; i < 3; i++) vn[i] = Y + s * NPRIM[hue][i];
        mv(NPINV, vn, vw);
        int ok = 1;
        for (int i = 0; i < 3; i++) if (vw[i] < 0 || vw[i] > 1) ok = 0;
        for (int i = 0; i < 3; i++) out[i] = ok ? vw[i] : Y;
    } else {
        double g = 0.06 + 0.70 * (((x*3 + y*7) % 11) / 10.0);
        out[0] = out[1] = out[2] = g;
    }
}

/* ---------------- metrics ---------------- */
typedef struct {
    double oog_frac;
    double de_oog_mean, de_oog_p95, de_oog_max;
    double de_all_mean;
    double yloss_per_oog;       /* mean luminance loss per out-of-gamut pixel */
    double chroma_loss_all;     /* CIELAB C* loss over the whole frame (saturation) */
    double chroma_loss_nonog;   /* C* loss on in-gamut pixels: the "washed out" effect */
    double excess_mean;         /* mean excess per OOG pixel */
    double diversity;           /* distinct 8-bit outputs per OOG pixel */
} Metric;

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}
static double pctl(double* xs, int n, double p) {
    if (n <= 0) return 0.0;
    qsort(xs, n, sizeof(double), cmp_d);
    int k = (int)floor(p * (n-1) + 0.5);
    if (k < 0) k = 0;
    if (k >= n) k = n-1;
    return xs[k];
}

typedef int (*MapFn)(const V3, double, V3);   /* param is sigma or g */

/* Return by value is a struct; callers use `.de_oog_p95` on the result. */
static Metric measure(const V3* wide, int n, MapFn fn, double param, double src_lum_unused) {
    (void)src_lum_unused;
    double* deoog = malloc(sizeof(double) * n);
    double yloss = 0, cin_all = 0, cout_all = 0, cin_non = 0, cout_non = 0;
    double ex_sum = 0, de_all = 0;
    int oogc = 0, deoog_n = 0;
    int uniq = 0;
    /* 8-bit distinct outputs among OOG pixels: use a simple set of 24-bit keys */
    static unsigned char seen[1<<24];
    memset(seen, 0, sizeof(seen));
    for (int p = 0; p < n; p++) {
        V3 nv; mv(T, wide[p], nv);
        V3 o; fn(nv, param, o);
        double ls[3], lo[3];
        lab_of_wide(wide[p], ls);       /* source Lab (absolute, wide colour is real) */
        lab_of_narrow(o, lo);
        double de = de2000(ls, lo);
        de_all += de;
        double cin = hypot(ls[1], ls[2]);      /* source chroma */
        double cout = hypot(lo[1], lo[2]);     /* output chroma */
        if (cin > 2.0) { cin_all += cin; cout_all += cout; }
        if (!in_gamut(nv)) {
            oogc++;
            deoog[deoog_n++] = de;
            yloss += fabs(lum_narrow(nv) - lum_narrow(o));
            ex_sum += excess(nv);
            int key = (int)((o[0]*255+0.5)*65536 + (o[1]*255+0.5)*256 + (o[2]*255+0.5));
            if (!seen[key]) { seen[key] = 1; uniq++; }
        } else if (cin > 2.0) {
            cin_non += cin; cout_non += cout;      /* in-gamut colour affected by global squeeze */
        }
    }
    Metric m;
    m.oog_frac = (double)oogc / n;
    double s = 0; for (int i = 0; i < deoog_n; i++) s += deoog[i];
    m.de_oog_mean = deoog_n ? s/deoog_n : 0;
    m.de_oog_p95 = pctl(deoog, deoog_n, 0.95);
    m.de_oog_max = deoog_n ? pctl(deoog, deoog_n, 1.0) : 0;
    m.de_all_mean = de_all/n;
    m.yloss_per_oog = oogc ? yloss/oogc : 0;
    m.chroma_loss_all = cin_all > 0 ? 1 - cout_all/cin_all : 0;
    m.chroma_loss_nonog = cin_non > 0 ? 1 - cout_non/cin_non : 0;
    m.excess_mean = oogc ? ex_sum/oogc : 0;
    m.diversity = oogc ? (double)uniq/oogc : 0;
    free(deoog);
    return m;
}

/* wrappers so clip/compress share the MapFn signature */
static int map_clip(const V3 v, double unused, V3 out) { (void)unused; clipv(v, out); return 0; }
static int map_knee(const V3 v, double sigma, V3 out) { compress_pixel(v, sigma, out); return 0; }
static int map_global(const V3 v, double g, V3 out) { compress_global(v, g, out); return 0; }

/* Largest compression strength (sigma) whose OOG dE p95 stays within tau.
 * dE is not guaranteed monotone in sigma, so scan then refine. */
static double solve_sigma(const V3* wide, int n, double tau) {
    double best = 0, best_de = measure(wide, n, map_knee, 0.0, 0).de_oog_p95;
    for (int k = 0; k <= 100; k++) {
        double s = k / 100.0;
        double de = measure(wide, n, map_knee, s, 0).de_oog_p95;
        if (de <= tau && s > best) best = s;      /* prefer most compression under tau */
        if (de < best_de) best_de = de;
    }
    return best;
}
/* Smallest g (most global squeeze) whose dE p95 stays within tau. */
static double solve_global(const V3* wide, int n, double tau, double gmin) {
    double best = 1.0;
    for (int k = 0; k <= 100; k++) {
        double g = gmin + (1.0 - gmin) * k / 100.0;
        double de = measure(wide, n, map_global, g, 0).de_oog_p95;
        if (de <= tau) best = g;   /* scan upward; last one under tau = smallest g meeting it */
    }
    return best;
}

/* ---------------- self-checks ---------------- */
static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Xw = MN[0][0]+MN[0][1]+MN[0][2];
    Yw = MN[1][0]+MN[1][1]+MN[1][2];
    Zw = MN[2][0]+MN[2][1]+MN[2][2];
    build_narrow_hues();

    /* --- invariants --- */
    {
        V3 one = {1,1,1}, o; mv(T, one, o);
        check("neutral_preserved", fabs(o[0]-1)<1e-4 && fabs(o[1]-1)<1e-4 && fabs(o[2]-1)<1e-4);

        V3 wr = {1,0,0}, nr; mv(T, wr, nr);
        check("wide_red_oog", nr[0] > 1.0 && (nr[1] < 0 || nr[2] < 0));
        check("wide_red_excess", excess(nr) > 0.3);

        double lab[3]; lab_of_wide(one, lab);
        check("white_L100", fabs(lab[0]-100) < 1e-3);

        double a[3]={50,2.6772,-79.7751}, b[3]={50,0,-82.7485};
        check("de2000_reference", fabs(de2000(a,b) - 2.0425) < 1e-3);

        V3 grey = {0.5,0.5,0.5};
        check("grey_sat_zero", sat_rgb(grey) == 0.0);
        check("in_gamut_zero_excess", excess(grey) == 0.0);

        /* clipping loses luminance; compression (sigma=1) preserves it */
        double y_src = lum_narrow(nr);
        V3 c; clipv(nr, c);
        V3 pr; compress_pixel(nr, 1.0, pr);
        check("clip_loses_luma", lum_narrow(c) < y_src - 1e-6);
        check("compress_keeps_luma", fabs(lum_narrow(pr) - y_src) < 1e-6);
        check("compress_lands_in_gamut", in_gamut(pr) || excess(pr) < 1e-6);

        V3 k0; compress_pixel(nr, 0.0, k0);
        check("sigma0_is_clip", fabs(k0[0]-c[0])<1e-9 && fabs(k0[1]-c[1])<1e-9 && fabs(k0[2]-c[2])<1e-9);

        check("compress_less_sat", sat_rgb(pr) < sat_rgb(nr));
        check("clip_more_sat_than_compress", sat_rgb(c) > sat_rgb(pr));
    }

    /* --- three tiers: 5% / 15% / 30% of pixels out of gamut --- */
    static V3 wide[NPIX];
    double fracs[3] = { 0.05, 0.15, 0.30 };
    double excesses[3] = { 0.03, 0.05, 0.10 };   /* relative overshoot past the boundary */
    double tau = 2.0;                            /* acceptable CIEDE2000 */
    for (int t = 0; t < 3; t++) {
        int made = 0;
        for (int y = 0; y < GRID; y++)
            for (int x = 0; x < GRID; x++) {
                make_wide_pixel(x, y, fracs[t], excesses[t], wide[y*GRID + x]);
                V3 nv; mv(T, wide[y*GRID + x], nv);
                if (!in_gamut(nv)) made++;
            }

        Metric mc = measure(wide, NPIX, map_clip, 0.0, 0);
        double sig = solve_sigma(wide, NPIX, tau);
        Metric mk = measure(wide, NPIX, map_knee, sig, 0);
        double gfit = 1.0;
        for (int p = 0; p < NPIX; p++) {
            V3 nv; mv(T, wide[p], nv);
            double t2 = boundary_scale(nv, NULL);
            if (t2 < 1.0 && t2 < gfit) gfit = t2;
        }
        double g = solve_global(wide, NPIX, tau, gfit);
        Metric mg = measure(wide, NPIX, map_global, g, 0);

        printf("\n[tier %.0f%%] authored excess=%.2f  measured OOG=%.1f%% (%d px)  "
               "mean excess/OOG=%.3f  tau=%.1f dE00\n",
               fracs[t]*100, excesses[t], mc.oog_frac*100, made, mc.excess_mean, tau);
        printf("  %-12s %8s %8s %8s %9s %8s\n",
               "method", "dE mean", "dE p95", "dE max", "chroma-", "Yloss/OOG");
        printf("  %-12s %8.2f %8.2f %8.2f %8.1f%% %8.3f%%\n",
               "clip", mc.de_oog_mean, mc.de_oog_p95, mc.de_oog_max,
               mc.chroma_loss_all*100, mc.yloss_per_oog*100);
        printf("  %-12s %8.2f %8.2f %8.2f %8.1f%% %8.3f%%   sigma=%.2f\n",
               "compress", mk.de_oog_mean, mk.de_oog_p95, mk.de_oog_max,
               mk.chroma_loss_all*100, mk.yloss_per_oog*100, sig);
        printf("  %-12s %8.2f %8.2f %8.2f %8.1f%% %8.3f%%   g=%.3f\n",
               "global", mg.de_oog_mean, mg.de_oog_p95, mg.de_oog_max,
               mg.chroma_loss_all*100, mg.yloss_per_oog*100, g);
        printf("  verdict: dE<=tau clip=%d compress=%d global=%d | chroma<=10%% clip=%d compress=%d global=%d\n",
               mc.de_oog_mean<=tau, mk.de_oog_mean<=tau, mg.de_oog_mean<=tau,
               mc.chroma_loss_all<=0.10, mk.chroma_loss_all<=0.10, mg.chroma_loss_all<=0.10);
    }

    printf("\nall_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
