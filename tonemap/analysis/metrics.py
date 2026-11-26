"""Dark-detail / contrast acceptance metrics and the three candidate curves.

Scene model
-----------
A scene is a luminance distribution spanning a finite dynamic range
``R = x_max / x_min``.  We normalize to u in [0, 1] with a *log-uniform*
density (equal mass per stop), which is the standard worst-case model for
tonemapping: every stop of the input carries the same amount of content.

    x(u) = R ** (u - 1),   u ~ Uniform(0, 1),
    x in [1/R, 1],   stop span = log2(R).

A tone curve T maps u -> y in [0, 1].  All three candidate families are
strictly increasing, map u=0 -> y=0 and u=1 -> y=1, and are controlled by a
single shape parameter.

Metrics (executable form of the acceptance sentence
"dark areas must not be crushed, details must stay visible")
----------------------------------------------------------------
D -- shadow-detail metric, the non-empty-bin fraction inside the lowest two
     deciles.  The bins are equal-width output bins covering [0, 1], and the
     population whose histogram is taken is the *shadow cohort* -- the input
     mass below the first input quintile (the lowest two deciles of scene
     luminance).  In the continuous limit non-empty-bin fraction equals
     occupied-width fraction:

         D = width(T([0, 0.2]) ∩ [0, 0.2]) / 0.2          in [0, 1]

     Rationale and caveat on the literal reading are documented in
     ACCEPTANCE.md, section 2.

C -- global contrast metric, ratio of the 90th to the 10th output percentile:

         C = Q_0.9(T) / Q_0.1(T).
"""

from dataclasses import dataclass

import numpy as np

# ---------------------------------------------------------------------------
# Scene model
# ---------------------------------------------------------------------------


def scene_luminance(u: np.ndarray, R: float) -> np.ndarray:
    """Normalized scene luminance for log-uniform input position u in [0,1]."""
    return np.power(R, u - 1.0)


def sample_scene(R: float, n: int, rng: np.random.Generator) -> np.ndarray:
    """Draw n log-uniform scene luminances spanning dynamic range R."""
    u = rng.uniform(0.0, 1.0, size=n)
    return scene_luminance(u, R)


# ---------------------------------------------------------------------------
# Candidate curves.  Each maps input position u in [0,1] to output y in [0,1].
# ---------------------------------------------------------------------------


def power_curve(u, gamma: float):
    """Power / gamma curve y = u**gamma.  gamma > 1 lifts the toe."""
    u = np.clip(u, 0.0, 1.0)
    return np.power(u, gamma)


def log_curve(u, alpha: float, R: float = 1.0):
    """Convex log curve y = 1 - ln(1 + alpha(1-u)) / ln(1 + alpha).

    Output-side logarithm with fixed endpoints y(0)=0, y(1)=1 and toe slope
    1/ln(1+alpha) <= 1: the same compression regime as the power family
    (alpha -> 0 recovers the linear curve).  R is unused but kept in the
    signature so all three curves share a calling convention.
    """
    u = np.clip(u, 0.0, 1.0)
    return 1.0 - np.log1p(alpha * (1.0 - u)) / np.log1p(alpha)


def piecewise_linear_curve(u, y_b: float):
    """Two-segment PL curve with breakpoint fixed at the input quintile.

    Breakpoint is at (u_b=0.2, y_b); endpoints (0,0) and (1,1).
    y_b in [0, 0.2] makes the toe slope y_b/0.2 <= 1 and the shoulder slope
    (1-y_b)/0.8 >= 1: shadows are *compressed* relative to a linear curve
    (which is the normal tonemapping regime).  y_b=0.2 is the linear curve;
    y_b->0 hard-clips the toe.
    """
    u = np.clip(u, 0.0, 1.0)
    u_b = 0.2
    return np.where(u <= u_b, y_b / u_b * u, y_b + (1.0 - y_b) / (1.0 - u_b) * (u - u_b))


# ---------------------------------------------------------------------------
# Continuous (population-level, analytic) metrics
# ---------------------------------------------------------------------------

DARK_EDGE = 0.2  # lowest two deciles, input and output share the [0,1] frame


def analytic_D_power(gamma: float) -> float:
    """D for y = u**gamma: shadow cohort occupies [0, 0.2**gamma]."""
    hi = DARK_EDGE ** gamma
    return float(min(hi, DARK_EDGE) / DARK_EDGE)


def analytic_D_log(alpha: float, R: float = 1.0) -> float:
    """D for the convex log curve: cohort output spans [0, T(0.2; alpha)]."""
    hi = log_curve(DARK_EDGE, alpha)
    return float(min(hi, DARK_EDGE) / DARK_EDGE)


def analytic_D_pl(y_b: float) -> float:
    """D for the PL curve: cohort output spans [0, y_b] (breakpoint at 0.2)."""
    return float(min(y_b, DARK_EDGE) / DARK_EDGE)


def analytic_C_power(gamma: float, R: float = 1.0) -> float:
    """Q0.9/Q0.1 for y=u**gamma under log-uniform input (percentiles of u)."""
    return float(power_curve(0.9, gamma) / power_curve(0.1, gamma))


def analytic_C_log(alpha: float, R: float = 1.0) -> float:
    return float(log_curve(0.9, alpha) / log_curve(0.1, alpha))


def analytic_C_pl(y_b: float, R: float = 1.0) -> float:
    """Q0.1 lies on the toe, Q0.9 on the shoulder (verified for R>1)."""
    q10 = y_b / 0.2 * 0.1
    q90 = y_b + (1.0 - y_b) / 0.8 * (0.9 - 0.2)
    return float(q90 / q10)


# ---------------------------------------------------------------------------
# Empirical (finite-image, histogram-based) metrics -- the executable form
# ---------------------------------------------------------------------------


@dataclass
class MetricResult:
    D: float
    C: float
    n_shadow: int
    n_filled: int
    n_dark_bins: int


def empirical_metrics(lum: np.ndarray, R: float, curve, n_bins: int = 100) -> MetricResult:
    """Measure D and C from a finite rendered image (array of luminances).

    Parameters
    ----------
    lum : scene luminances (any positive scale; normalized internally).
    R   : scene dynamic range, used only for the log curve's parameterization.
    curve : callable mapping normalized input position u -> output y.
    n_bins : total equal-width bins over [0,1]; the dark zone is the lowest
             20 %, i.e. the first n_bins//5 bins.
    """
    lum = np.asarray(lum, dtype=np.float64)
    # Normalize scene to u in [0,1] via the known range R (x = R**(u-1)).
    u = 1.0 + np.log(np.maximum(lum, 1e-300)) / np.log(R)
    u = np.clip(u, 0.0, 1.0)
    y = curve(u)

    # Shadow cohort: input mass in the lowest two deciles of scene light.
    shadow = y[u <= DARK_EDGE]
    n_dark_bins = n_bins // 5  # bins covering output [0, 0.2]
    counts, _ = np.histogram(shadow, bins=n_dark_bins, range=(0.0, DARK_EDGE))
    n_filled = int(np.count_nonzero(counts))
    D = n_filled / n_dark_bins

    q10, q90 = np.quantile(y, (0.1, 0.9))
    C = float(q90 / max(q10, 1e-12))
    return MetricResult(D=D, C=C, n_shadow=shadow.size, n_filled=n_filled,
                        n_dark_bins=n_dark_bins)


def literal_fullhist_D(lum: np.ndarray, R: float, curve, n_bins: int = 100) -> float:
    """Diagnostic: D computed on the *whole-image* output histogram.

    This is the literal reading of "non-empty-bin fraction of the lowest two
    deciles of the luminance histogram".  It is kept only to demonstrate why
    that reading cannot detect crushed blacks on continuous content
    (ACCEPTANCE.md section 2): a monotone curve always fills the lowest bin,
    so empty bins in [0,0.2] only arise from finite-pixel sparsity.
    """
    lum = np.asarray(lum, dtype=np.float64)
    u = 1.0 + np.log(np.maximum(lum, 1e-300)) / np.log(R)
    u = np.clip(u, 0.0, 1.0)
    y = curve(u)
    n_dark_bins = n_bins // 5
    counts, _ = np.histogram(y, bins=n_dark_bins, range=(0.0, DARK_EDGE))
    return float(np.count_nonzero(counts) / n_dark_bins)
