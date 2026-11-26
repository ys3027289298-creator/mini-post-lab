#!/usr/bin/env python3
"""
audit.py - cross-machine pixel audit for the color-management incident.

Model of the incident
---------------------
Two machines share one source (linear scene radiance L) and one post-processing
op (the separable box blur from bloom/, which is just repeated weighted adds).

  * correct pipeline  : blur is applied to radiance,   out = blur(L)
  * buggy "mixed" one : L is gamma-encoded first, the encoded buffer is blurred
                        with linear arithmetic, then decoded with the machine's
                        own gamma:  out = (blur(L^(1/g)))^g

The deliverable is compared in ONE agreed encoding (reference gamma g0 = 2.2),
so display calibration is factored out: machine A and B outputs are both
re-encoded with g0 and quantized to 8-bit code values. A pixel pair fails when
any channel differs by more than one quantization step (1/255).

Run:  python3 -I tools/audit.py
Out:  results/audit_report.json, plus the human-readable table on stdout.
"""
import sys
import os
import json

# -I isolates site-packages; the interpreter is trusted, the repo data is not.
sys.path.insert(0, "/home/ys/.local/lib/python3.12/site-packages")
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")

G_REF = 2.2          # agreed deliverable transfer function
ONE_CODE = 1.0 / 255.0
W, H, R = 32, 32, 2  # frame size and blur radius (matches bloom.c box blur)
GAMMAS = (1.8, 2.2, 2.4)
PAIRS = ((1.8, 2.2), (2.2, 2.4), (1.8, 2.4))
MAX_VIOLATIONS_PRINTED = 12


# ---------------------------------------------------------------- transfer fns
def encode(L, g):
    return np.power(np.clip(L, 0.0, None), 1.0 / g)


def decode(x, g):
    return np.power(np.clip(x, 0.0, 1.0), g)


# ------------------------------------------------------------- bloom.c kernel
def box_blur_2d(buf, r):
    """Separable box blur with edge clamping, per RGB channel, float64."""
    tmp = np.empty_like(buf)
    out = np.empty_like(buf)
    k = 2 * r + 1
    # horizontal pass
    for x in range(buf.shape[1]):
        lo, hi = max(0, x - r), min(buf.shape[1], x + r + 1)
        pad_l = lo - (x - r)
        pad_r = (x + r + 1) - hi
        s = buf[:, lo:hi, :].sum(axis=1)
        s += buf[:, lo, :] * pad_l + buf[:, hi - 1, :] * pad_r
        tmp[:, x, :] = s / k
    # vertical pass
    for y in range(buf.shape[0]):
        lo, hi = max(0, y - r), min(buf.shape[0], y + r + 1)
        pad_l = lo - (y - r)
        pad_r = (y + r + 1) - hi
        s = tmp[lo:hi, :, :].sum(axis=0)
        s += tmp[lo, :, :] * pad_l + tmp[hi - 1, :, :] * pad_r
        out[y, :, :] = s / k
    return out


# ------------------------------------------------------------- test scene (L)
def make_frame():
    """Deterministic linear-light frame: ramps, checker, warm/cool patches."""
    L = np.zeros((H, W, 3))
    xs = np.linspace(0.0, 1.0, W)
    # top third: three channel ramps (R, G, B)
    L[0:11, :, 0] = xs[None, :]
    L[11:22, :, 1] = xs[None, :]
    # bottom third: checker in all channels plus a mid-gray band
    xx, yy = np.meshgrid(np.arange(W), np.arange(H - 22))
    checker = ((xx + yy) % 2).astype(float)
    L[22:, :, :] = checker[:, :, None] * 0.8
    L[22:25, :, :] = 0.5
    # warm patch (R-rich) and cool patch (B-rich), mid luminance
    L[4:8, 4:10, 0] = 0.9
    L[4:8, 4:10, 1] = 0.35
    L[4:8, 4:10, 2] = 0.15
    L[14:18, 20:28, 0] = 0.15
    L[14:18, 20:28, 1] = 0.35
    L[14:18, 20:28, 2] = 0.9
    return L


# ----------------------------------------------------------------- pipelines
def run_machine(L, g, linear_pipeline):
    if linear_pipeline:
        return box_blur_2d(L, R)
    return decode(box_blur_2d(encode(L, g), R), g)


def code_values(out_linear, g_ref=G_REF):
    return np.rint(encode(out_linear, g_ref) * 255.0).astype(int)


# ------------------------------------------------- gamma tolerance inversion
def max_capture_diff(g_nom, rel_delta, g_ref=G_REF):
    """Same linear scene, encoded on machines g_nom and g_nom*(1+rel_delta),
    compared in their own encoded spaces (capture direction)."""
    Ls = np.linspace(1e-4, 1.0, 200_000)
    return np.abs(encode(Ls, g_nom) - encode(Ls, g_nom * (1.0 + rel_delta))).max()


def max_display_diff(g_nom, rel_delta, g_ref=G_REF):
    """Same encoded framebuffer x, decoded on two displays, re-encoded at the
    reference gamma for comparison (display direction = the actual incident)."""
    xs = np.linspace(1e-4, 1.0, 200_000)
    g_b = g_nom * (1.0 + rel_delta)
    return np.abs(encode(decode(xs, g_nom), g_ref) -
                  encode(decode(xs, g_b), g_ref)).max()


def invert_tolerance(g_nom, diff_fn):
    """Bisection: largest |rel_delta| whose worst code diff stays <= 1."""
    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        if diff_fn(g_nom, mid) * 255.0 <= 1.0:
            lo = mid
        else:
            hi = mid
    return lo


# --------------------------------------------------------------------- audit
def main():
    os.makedirs(RESULTS, exist_ok=True)
    rng_state = np.random.get_state()
    np.random.seed(0)  # scene is deterministic anyway
    L = make_frame()

    report = {"criterion": "per-channel code-value diff > 1 (reference gamma %.2f)" % G_REF,
              "frame": [W, H], "blur_radius": R, "pairs": [], "tolerance": []}

    print("=" * 78)
    print("cross-machine pixel audit | reference encoding gamma %.2f | 1 code = %.6f"
          % (G_REF, ONE_CODE))
    print("=" * 78)

    for linear_pipeline in (True, False):
        mode = "linear (correct)" if linear_pipeline else "gamma-space (buggy)"
        print("\n### pipeline: %s" % mode)
        for gA, gB in PAIRS:
            outA = run_machine(L, gA, linear_pipeline)
            outB = run_machine(L, gB, linear_pipeline)
            cA, cB = code_values(outA), code_values(outB)
            diff = np.abs(encode(outA, G_REF) - encode(outB, G_REF)) * 255.0
            viol = np.argwhere(diff > 1.0)
            rec = {"pair": [gA, gB], "mode": mode,
                   "max_code_diff": round(float(diff.max()), 2),
                   "violations": int(len(viol)),
                   "pass": int(len(viol) == 0)}
            report["pairs"].append(rec)
            print("\nmachines gamma %.2f vs %.2f : max_diff=%.2f codes, violations=%d -> %s"
                  % (gA, gB, diff.max(), len(viol), "PASS" if len(viol) == 0 else "FAIL"))
            shown = 0
            for (y, x, ch) in viol:
                if shown >= MAX_VIOLATIONS_PRINTED:
                    print("  ... %d more violations" % (len(viol) - shown))
                    break
                print("  px(%2d,%2d) ch%d | gammas %.1f/%.1f | codes %3d vs %3d | diff %.2f"
                      % (x, y, ch, gA, gB, cA[y, x, ch], cB[y, x, ch], diff[y, x, ch]))
                shown += 1

    print("\n" + "=" * 78)
    print("gamma tolerance inversion (worst case over the full tonal range)")
    print("=" * 78)
    for g_nom in GAMMAS:
        t_cap = invert_tolerance(g_nom, max_capture_diff)
        t_disp = invert_tolerance(g_nom, max_display_diff)
        band_cap = (g_nom * (1 - t_cap), g_nom * (1 + t_cap))
        rec = {"gamma": g_nom,
               "rel_tol_capture": round(t_cap, 5),
               "rel_tol_display": round(t_disp, 5),
               "abs_band_capture": [round(band_cap[0], 4), round(band_cap[1], 4)]}
        report["tolerance"].append(rec)
        print("gamma %.2f: capture +-%.2f%% -> band [%.4f, %.4f] | display +-%.2f%%"
              % (g_nom, t_cap * 100, band_cap[0], band_cap[1], t_disp * 100))

    # blend-law discrepancy headline numbers (gamma 2.2, black/white 50/50)
    g = G_REF
    lin_bw, gam_bw = 0.5, decode(0.5, g)
    report["blend_law"] = {
        "case": "50/50 blend of black and white, gamma 2.2",
        "linear_result": lin_bw,
        "gamma_result": round(float(gam_bw), 4),
        "code_gap": round(float((encode(lin_bw, g) - encode(gam_bw, g)) * 255), 1),
    }
    print("\nblend-law check (black/white 50/50, g=2.2): linear=%.4f gamma=%.4f gap=%.1f codes"
          % (lin_bw, gam_bw, report["blend_law"]["code_gap"]))

    out_path = os.path.join(RESULTS, "audit_report.json")
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2)
    np.random.set_state(rng_state)
    print("\nreport written to %s" % out_path)


if __name__ == "__main__":
    main()
