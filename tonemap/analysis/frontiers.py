"""Analytic trade-off frontiers, feasible parameter intervals and the
dynamic-range infeasibility threshold.

Notation
--------
R        scene dynamic range x_max/x_min (stops = log2 R)
D        shadow-detail metric in [0,1] (higher = more shadow gradation)
C        contrast metric Q0.9/Q0.1 (higher = punchier image)
D*, C*   acceptance thresholds
G        maximum admissible shadow-curve gain (noise/halo budget)

All three families are endpoint-fixed monotone curves T(0)=0, T(1)=1 with a
toe slope <= 1.  Their shape metrics (D, C) depend only on the shape
parameter; R enters solely through the shadow-gain feasibility budget.
"""

from dataclasses import dataclass

import numpy as np

from metrics import (
    DARK_EDGE,
    analytic_C_log,
    analytic_C_pl,
    analytic_C_power,
    analytic_D_log,
    analytic_D_pl,
    analytic_D_power,
)


def bisect(f, lo, hi, tol=1e-12, max_iter=200):
    """Scalar bisection root of f on [lo, hi] with f(lo)*f(hi) <= 0."""
    flo, fhi = f(lo), f(hi)
    if flo * fhi > 0:
        raise ValueError("bisection bracket does not straddle zero")
    for _ in range(max_iter):
        mid = 0.5 * (lo + hi)
        fm = f(mid)
        if abs(fm) <= tol or 0.5 * (hi - lo) <= tol:
            return mid
        if flo * fm <= 0:
            hi, fhi = mid, fm
        else:
            lo, flo = mid, fm
    return 0.5 * (lo + hi)


def bisect_safe(f, lo, hi):
    """Return the root on [lo,hi], or None if the bracket never straddles."""
    try:
        return bisect(f, lo, hi)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Power family: y = u**gamma, convex regime gamma >= 1
#   D(g) = 5 * 0.2**g ;  C(g) = 9**g
# ---------------------------------------------------------------------------

POWER_DOMAIN = (1.0, np.inf)  # gamma; gamma=1 is the linear curve


def power_gamma_for_D(D_star: float) -> float:
    """Gamma at which D(gamma) = D* (D decreases with gamma)."""
    return np.log(5.0 / D_star) / np.log(5.0)


def power_gamma_for_C(C_star: float) -> float:
    """Smallest gamma that still clears C* (C increases with gamma)."""
    return np.log(C_star) / np.log(9.0)


def power_interval(D_star: float, C_star: float):
    """Feasible gamma interval clipped to the convex domain; None if empty."""
    g_lo = max(power_gamma_for_C(C_star), POWER_DOMAIN[0])
    g_hi = power_gamma_for_D(D_star)
    if g_lo > g_hi:
        return None
    return g_lo, g_hi


def power_C_of_D(D: float) -> np.ndarray:
    """Closed-form trade-off C(D) for the power family."""
    D = np.asarray(D, dtype=np.float64)
    gamma = np.log(5.0 / D) / np.log(5.0)
    return np.power(9.0, gamma)


# ---------------------------------------------------------------------------
# Piecewise-linear family: breakpoint (0.2, y_b), y_b in (0, 0.2]
#   D = 5 y_b
#   Q0.1 = 0.5 y_b (toe),  Q0.9 = 0.875 + 0.125 y_b (shoulder)
#   C = 1.75/y_b + 0.25 = 8.75/D + 0.25
# ---------------------------------------------------------------------------

PL_DOMAIN = (1e-9, DARK_EDGE)  # y_b; y_b=0.2 is the linear curve


def pl_yb_for_D(D_star: float) -> float:
    return DARK_EDGE * D_star  # smallest y_b clearing D* (D rises with y_b)


def pl_yb_for_C(C_star: float) -> float:
    # C falls with y_b; C(y_b) >= C*  <=>  y_b <= 1.75/(C* - 0.25)
    return 1.75 / (C_star - 0.25)  # largest y_b clearing C*


def pl_interval(D_star: float, C_star: float):
    y_lo = max(pl_yb_for_D(D_star), PL_DOMAIN[0])
    y_hi = min(pl_yb_for_C(C_star), PL_DOMAIN[1])
    if y_lo > y_hi:
        return None
    return y_lo, y_hi


def pl_C_of_D(D: float) -> np.ndarray:
    """Closed-form trade-off C(D) for the PL family."""
    D = np.asarray(D, dtype=np.float64)
    return 8.75 / np.maximum(D, 1e-12) + 0.25


# ---------------------------------------------------------------------------
# Convex log family: y = 1 - ln(1 + a(1-u)) / ln(1+a), alpha > 0
# Shape range: D in (0,1], C in [9, 21.85...).  alpha->0 is the linear curve.
# ---------------------------------------------------------------------------

LOG_DOMAIN = (1e-9, np.inf)
LOG_C_MIN = 9.0
LOG_C_MAX = analytic_C_log(1e12)  # supremum as alpha -> inf


def log_alpha_for_D(D_star: float):
    # D falls from 1 (alpha->0) towards 0 (alpha->inf)
    return bisect_safe(lambda a: analytic_D_log(a) - D_star, 1e-9, 1e9)


def log_alpha_for_C(C_star: float):
    # C rises from 9 (alpha->0) to a finite supremum ~21.85 (alpha->inf).
    # Below the minimum the contrast constraint never binds (return domain
    # lower edge); above the supremum no log curve can satisfy it (None).
    if C_star <= LOG_C_MIN:
        return LOG_DOMAIN[0]
    if C_star >= LOG_C_MAX:
        return None
    return bisect(lambda a: analytic_C_log(a) - C_star, 1e-9, 1e9)


def log_interval(D_star: float, C_star: float):
    a_lo = log_alpha_for_C(C_star)
    a_hi = log_alpha_for_D(D_star)
    if a_lo is None or a_hi is None or a_lo > a_hi:
        return None
    return a_lo, a_hi


# ---------------------------------------------------------------------------
# Frontiers and dominance
# ---------------------------------------------------------------------------

FAMILIES = ("power", "log", "piecewise_linear")


def frontier_grid(n: int = 400):
    """D-C frontiers sampled densely, plus the curve parameter at each point."""
    D = np.linspace(0.02, 1.0, n)
    out = {"D": D}
    out["power"] = power_C_of_D(D)
    # log: sweep alpha, read (D, C) off the analytic formulas.  Both run
    # monotonically as alpha grows; store them ordered by ascending D so the
    # arrays can feed np.interp directly.
    alpha = np.geomspace(1e-6, 1e6, n)
    d_log = np.array([analytic_D_log(a) for a in alpha])
    c_log = np.array([analytic_C_log(a) for a in alpha])
    order = np.argsort(d_log)
    out["log_D"] = d_log[order]
    out["log_C"] = c_log[order]
    out["piecewise_linear"] = pl_C_of_D(D)
    return out


def dominance_table(tol: float = 1e-9):
    """Pairwise dominance on the common D grid.

    A family dominates another if its contrast is no smaller at every shared
    shadow-detail level (and strictly greater somewhere).
    """
    grid = frontier_grid()
    D, c_power, c_pl = grid["D"], grid["power"], grid["piecewise_linear"]
    c_log = np.interp(D, grid["log_D"], grid["log_C"])
    curves = {"power": c_power, "log": c_log, "piecewise_linear": c_pl}
    rows = []
    for a in FAMILIES:
        for b in FAMILIES:
            if a == b:
                continue
            # Compare only where both curves have native support; require
            # dominance on the interior (endpoints may coincide).
            interior = slice(1, -1)
            no_worse = np.all(curves[a][interior] >= curves[b][interior] - tol)
            strictly = np.any(curves[a][interior] > curves[b][interior] + tol)
            if no_worse and strictly:
                rows.append((a, b))
    return rows, curves


# ---------------------------------------------------------------------------
# Dynamic-range infeasibility threshold
# ---------------------------------------------------------------------------


def shadow_gain(R: float, D: float) -> float:
    """Average linear-light gain over the shadow cohort.

    The shadow cohort spans u in [0,0.2], i.e. x in [R^-1, R^-0.8], and is
    spread over an output interval of width 0.2 D.
    """
    # Stable form of R**-0.8 - R**-1 (subtraction underflows for large R):
    # R**-1 * expm1(0.2 ln R)
    dx = np.exp(-np.log(R)) * np.expm1(0.2 * np.log(R))
    return DARK_EDGE * D / dx


def range_threshold(D_star: float, G: float):
    """Smallest R for which no endpoint-fixed curve can keep D >= D* while
    respecting the shadow-gain budget G.

    Feasibility requires  g(R, D*) <= G, i.e.
        R >= A (R^0.2 - 1),  A = 5 G / D*.
    R* is the larger root of R = A (R^0.2 - 1); beyond it the feasible set is
    empty.  Asymptotic closed form: R* ~ (5G/D*)^1.25.
    """
    A = 5.0 * G / D_star
    f = lambda R: R - A * (R ** 0.2 - 1.0)
    # f is positive at R=1, negative on a middle interval, and crosses zero a
    # second time near the asymptotic estimate A**1.25.  Find the negative
    # lobe by doubling, then bracket the second (large-R) root.
    lo = 1.0 + 1e-9
    while f(lo) >= 0:
        lo *= 2.0
    hi = A ** 1.25
    while f(hi) <= 0:
        hi *= 1.25
    R_star = bisect(f, lo, hi)
    asymptotic = A ** 1.25
    return R_star, asymptotic


@dataclass
class SceneVerdict:
    name: str
    R: float
    stops: float
    family: str
    interval: object  # tuple or None
    param_pick: float
    D_pred: float
    C_pred: float
    gain: float
    feasible: bool
    reason: str


def evaluate_scene(name: str, R: float, D_star: float, C_star: float, G: float):
    """Solve the parameter interval of every family on one scene tier and
    check the shadow-gain budget.  The operating point is the D=D* boundary
    (max contrast subject to the dark-detail rule), clipped to each family's
    domain: if that boundary lies outside the convex domain the family can
    only offer the linear curve, which over-satisfies D*.
    """
    stops = np.log2(R)
    verdicts = []
    solvers = {
        "power": (power_interval, power_gamma_for_D, analytic_D_power, analytic_C_power),
        "log": (log_interval, log_alpha_for_D, analytic_D_log, analytic_C_log),
        "piecewise_linear": (pl_interval, pl_yb_for_D, analytic_D_pl, analytic_C_pl),
    }
    for family, (solve, p_for_d, d_of, c_of) in solvers.items():
        interval = solve(D_star, C_star)
        if interval is None:
            verdicts.append(SceneVerdict(name, R, stops, family, None, float("nan"),
                                         float("nan"), float("nan"), float("nan"),
                                         False, "D/C constraint intersection empty"))
            continue
        lo, hi = interval
        # D* boundary parameter, kept inside the feasible interval.
        p_boundary = p_for_d(D_star)
        pick = min(max(p_boundary, lo), hi)
        D_pred = d_of(pick)
        C_pred = c_of(pick)
        gain = shadow_gain(R, D_pred)
        ok = gain <= G
        reason = "pass" if ok else f"DR exceeded: gain {gain:.1f} > budget {G:g}"
        verdicts.append(SceneVerdict(name, R, stops, family, interval, pick,
                                     D_pred, C_pred, gain, ok, reason))
    return verdicts
