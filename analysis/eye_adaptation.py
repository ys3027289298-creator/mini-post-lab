#!/usr/bin/env python3
"""Eye-adaptation auto-exposure: white-flash analysis.

Model (standard game eye adaptation, all control in log10-luminance space):

  scene log-luminance  L_t = log10(Y_t)
  smoothed target      M_t = mean of L over a boxcar window of W frames
                       (W = 0 -> instantaneous target, the buggy baseline)
  target exposure      E*_t = log10(K) - M_t                  (K = 0.18)
  adaptation loop      E_t = E_{t-1} + a (E*_t - E_{t-1}),    a = 1 - exp(-dt/tau)
  displayed value      D_t = reinhard(Y_t * 10^E_t)

The cascade (boxcar -> EMA) is LTI in the log domain, so the step response
and the one-frame-spike response superpose exactly; the steady-state working
point on the tonemap is always R(K), independent of scene level.

Metrics
  exposure-space overshoot : max |delta E| in log10 (what the loop does)
  display-space overshoot  : max |log10(D/D_inf)| on non-spike frames
                             (what the player sees; Reinhard gain g(K) ~ 0.85)
  direct flash             : the spike frame itself, unfixable by exposure
  1 JND                    : 0.10 log10
"""
import math
import csv
import os

FPS = 60.0
DT = 1.0 / FPS
K = 0.18
D_INF = K / (1.0 + K)
G_K = 1.0 / (1.0 + K)                  # d log10 R / d log10 v at v=K
JND = 0.10
TAU = 0.25
A = 1.0 - math.exp(-DT / TAU)
TAU_FRAMES = -1.0 / math.log(1.0 - A)
T_BUDGET = 1.0
SIM_FRAMES = 900
SETTLE_EPS = JND

JUMPS = [1.0, 2.0, 3.0]               # log10 step sizes
NAMES = {1.0: "10x", 2.0: "100x", 3.0: "1000x"}


def reinhard(x):
    return x / (1.0 + x)


def run_step(jump_log, w, tau=TAU):
    """Luminance step of size jump_log at t=0; exposure starts settled."""
    a = 1.0 - math.exp(-DT / tau)
    e = math.log10(K)
    hist = [0.0] * max(w, 0)
    out = []
    for t in range(SIM_FRAMES):
        L = jump_log
        if w > 0:
            hist[t % w] = L
            M = sum(hist) / w
        else:
            M = L
        e = e + a * (math.log10(K) - M - e)
        D = reinhard(10.0 ** L * 10.0 ** e)
        out.append((t * DT, e, math.log10(D / D_INF)))
    return out


def run_spike(bg_log, w, spike_log, spike_at=60, tau=TAU):
    """Settled constant scene, one highlight frame at spike_at."""
    a = 1.0 - math.exp(-DT / tau)
    e0 = math.log10(K) - bg_log
    e = e0
    hist = [bg_log] * max(w, 0)
    out = []
    for t in range(SIM_FRAMES):
        L = bg_log + (spike_log if t == spike_at else 0.0)
        if w > 0:
            hist[t % w] = L
            M = sum(hist) / w
        else:
            M = L
        e = e + a * (math.log10(K) - M - e)
        D = reinhard(10.0 ** L * 10.0 ** e)
        out.append((t * DT, L, e, math.log10(D / D_INF)))
    return out, e0


def settle_time(step_trace, jump_log, eps=SETTLE_EPS):
    """First t after which |E - E_final| stays within eps."""
    e_final = math.log10(K) - jump_log
    for i, (_, e, _) in enumerate(step_trace):
        if abs(e - e_final) <= eps and \
               all(abs(e2 - e_final) <= eps for _, e2, _ in step_trace[i:]):
            return step_trace[i][0]
    return float("inf")


def spike_overshoot(spike_trace, e0, spike_at=60):
    """Loop-induced artifact: exclude the direct-flash frame itself."""
    tail = spike_trace[spike_at + 1:]
    pk_E = max(abs(e - e0) for _, _, e, _ in tail)
    pk_D = max(abs(od) for _, _, _, od in tail)
    direct = spike_trace[spike_at][3]      # display deviation on flash frame
    return pk_E, pk_D, direct


def pulse_peak_formula(ds, w):
    """Analytic peak |dE| of boxcar(W) -> EMA(a) hit by a unit-frame pulse.

    W = 0   : a * ds                    (bare EMA impulse)
    W >= 1  : (ds/W) * (1 - (1-a)^W)    (pulse of height ds/W, duration W)
    Asymptote W >> tau_frames: ds/W -> W_min = ds/JND (the simple heuristic).
    """
    if w == 0:
        return A * ds
    return (ds / w) * (1.0 - (1.0 - A) ** w)


print("=" * 88)
print(f"a = {A:.5f}/frame   tau = {TAU}s = {TAU_FRAMES:.1f} frames   "
      f"K = {K}   D_inf = {D_INF:.4f}   Reinhard gain g(K) = {G_K:.3f}")
print(f"1 JND = {JND} log10   adaptation budget = {T_BUDGET} s   "
      f"settle band = +/-{SETTLE_EPS} log10")
print("=" * 88)

# ---- [1] pure step: white flash and adaptation time ----------------------
print("\n[1] pure luminance step (indoor->outdoor), no glint")
print(f"{'jump':>6} {'W':>4} {'frame0 flash (JND)':>20} {'t_adapt (s)':>13}")
for j in JUMPS:
    for w in (0, 8, 15, 30):
        tr = run_step(j, w)
        flash = tr[0][2] / JND
        ta = settle_time(tr, j)
        print(f"{NAMES[j]:>6} {w:>4} {flash:>20.2f} {ta:>13.3f}")

# ---- [2] impulse formula vs simulation -----------------------------------
print("\n[2] boxcar->EMA pulse response: analytic formula vs simulation "
      "(spike ds = 1.0 log10)")
print(f"{'W':>4} {'formula (JND)':>15} {'sim dE (JND)':>14} "
      f"{'sim disp (JND)':>16} {'direct flash (JND)':>20}")
for w in (0, 1, 2, 4, 8, 15, 30, 60):
    tr, e0 = run_spike(2.0, w, 1.0)
    pk_E, pk_D, direct = spike_overshoot(tr, e0)
    print(f"{w:>4} {pulse_peak_formula(1.0, w) / JND:>15.2f} "
          f"{pk_E / JND:>14.2f} {pk_D / JND:>16.2f} {direct / JND:>20.2f}")

# ---- [3] window inversion for two glint sizes ----------------------------
print("\n[3] minimum window for loop overshoot <= 1 JND "
      "(heuristic W_min = ds/JND is the W >> tau asymptote)")
for ds in (1.0, 2.0):
    print(f"  glint ds = {ds:.1f} log10 ({10**ds:.0f}x), heuristic W_min = "
          f"{math.ceil(ds / JND)} frames")
    print(f"  {'bg':>6} {'W_min dE':>9} {'W_min disp':>12}")
    for j in JUMPS:
        w_e = w_d = None
        for w in range(400):
            tr, e0 = run_spike(j, w, ds)
            pk_E, pk_D, _ = spike_overshoot(tr, e0)
            if w_e is None and pk_E <= JND:
                w_e = w
            if w_d is None and pk_D <= JND:
                w_d = w
            if w_e is not None and w_d is not None:
                break
        print(f"  {NAMES[j]:>6} {w_e:>9} {w_d:>12}")

# ---- [4] tradeoff sweep: the two curves ----------------------------------
print("\n[4] tradeoff curves @100x-level scene")
print(f"{'W':>4} {'W(ms)':>7} {'ds=1 dE':>9} {'ds=1 disp':>11} "
      f"{'ds=2 dE':>9} {'ds=2 disp':>11} {'t_adapt 10x':>13} "
      f"{'100x':>8} {'1000x':>8}")
sweep = []
for w in (0, 1, 2, 4, 8, 15, 30, 45, 60, 90):
    row = [w, w / FPS * 1000]
    for ds in (1.0, 2.0):
        tr, e0 = run_spike(2.0, w, ds)
        pk_E, pk_D, _ = spike_overshoot(tr, e0)
        row += [pk_E / JND, pk_D / JND]
    tas = [settle_time(run_step(j, w), j) for j in JUMPS]
    row += tas
    sweep.append(row)
    print(f"{w:>4} {row[1]:>7.0f} {row[2]:>9.2f} {row[3]:>11.2f} "
          f"{row[4]:>9.2f} {row[5]:>11.2f} {row[6]:>13.3f} "
          f"{row[7]:>8.3f} {row[8]:>8.3f}")

# ---- [5] combined verdict per jump ---------------------------------------
print("\n[5] verdict per jump: overshoot <= 1 JND AND t_adapt <= 1.0 s")
for ds in (1.0, 2.0):
    print(f"  glint ds = {ds:.1f} log10:")
    for j in JUMPS:
        w_min = None
        for w in range(400):
            tr, e0 = run_spike(j, w, ds)
            pk_E, pk_D, _ = spike_overshoot(tr, e0)
            if pk_E <= JND and pk_D <= JND:
                w_min = w
                break
        ta = settle_time(run_step(j, w_min), j)
        if ta <= T_BUDGET:
            verdict = (f"PASS  W>={w_min} ({w_min / FPS:.3f}s), "
                       f"t_adapt={ta:.3f}s")
        else:
            w_time = max(w for w in range(0, 120)
                         if settle_time(run_step(j, w), j) <= T_BUDGET)
            tr, e0 = run_spike(j, w_time, ds)
            pk_E, pk_D, _ = spike_overshoot(tr, e0)
            verdict = (f"FAIL  W>={w_min} needed for JND but t_adapt={ta:.3f}s; "
                       f"at time-limit W={w_time} overshoot stays "
                       f"{pk_E / JND:.2f}/{pk_D / JND:.2f} JND (dE/disp) -> "
                       "adaptation time sacrificed")
        print(f"    {NAMES[j]:>6}: {verdict}")

# ---- [6] tau: the other lever --------------------------------------------
print("\n[6] tau sweep, W = 0, glint ds = 1.0 on 100x scene")
print(f"{'tau(s)':>8} {'a':>8} {'spike dE(JND)':>15} {'spike disp(JND)':>17} "
      f"{'t_adapt 100x(s)':>17}")
for tau in (0.10, 0.25, 0.50, 1.00):
    tr, e0 = run_spike(2.0, 0, 1.0, tau=tau)
    pk_E, pk_D, _ = spike_overshoot(tr, e0)
    ta = settle_time(run_step(2.0, 0, tau=tau), 2.0)
    print(f"{tau:>8.2f} {1 - math.exp(-DT / tau):>8.4f} "
          f"{pk_E / JND:>15.2f} {pk_D / JND:>17.2f} {ta:>17.3f}")

out = os.path.join(os.path.dirname(__file__), "sweep.csv")
with open(out, "w", newline="") as f:
    wr = csv.writer(f)
    wr.writerow(["W_frames", "W_ms",
                 "ds1_dE_JND", "ds1_disp_JND", "ds2_dE_JND", "ds2_disp_JND",
                 "t_adapt_10x_s", "t_adapt_100x_s", "t_adapt_1000x_s"])
    for row in sweep:
        wr.writerow([row[0], f"{row[1]:.1f}"] + [f"{v:.3f}" for v in row[2:]])
print("\nwrote", out)
