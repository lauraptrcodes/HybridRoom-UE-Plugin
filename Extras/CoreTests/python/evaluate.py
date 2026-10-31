"""
evaluate.py — Verifikation des FDTD-Kerns mit demselben C++-Code wie im Unreal-Plugin.

  1) Raummoden eines schallharten Quaders: FDTD-Spektrum vs. analytische Eigenfrequenzen
  2) Nachhallzeit vs. Wandabsorption: FDTD vs. Sabine und Eyring

Ausgabe (OUT_DIR): modes.csv, modes.png, t60.csv, t60.png
Voraussetzung: Modul hybridcore gebaut (cmake ... -DHAC_BUILD_PYTHON=ON), Pfad in MODULE_DIR.
"""

# ============================ CONFIG ============================
CONFIG = {
    "MODULE_DIR": "../build",            # Ordner mit hybridcore.*.so / .pyd
    "OUT_DIR": "results",

    # Raum (m) und Schallgeschwindigkeit (m/s)
    "ROOM": (5.0, 4.0, 3.0),
    "C": 343.0,

    # --- 1) Moden ---
    "MODES_FMAX": 343.0,                 # 343 Hz -> dx = 0.1 m teilt 5/4/3 m exakt
    "MODES_DURATION": 2.0,               # s, bestimmt die Frequenzauflösung
    "MODES_SOURCE": (0.15, 0.15, 0.15),  # Ecke -> alle Moden angeregt
    "MODES_RECEIVER": (4.85, 3.85, 2.85),
    "MODES_CHECK_MAX_HZ": 100.0,         # Moden bis hierhin auswerten
    "MODES_MIN_SPACING_HZ": 2.0,         # nur isolierte Moden (kein Nachbar näher)
    "MODES_SEARCH_HZ": 0.8,              # Suchfenster ± um die analytische Frequenz
    "MODES_PLOT_RANGE_HZ": (20.0, 110.0),

    # --- 2) Nachhallzeit ---
    "T60_FMAX": 300.0,
    "T60_ALPHAS": (0.05, 0.1, 0.2, 0.4, 0.7),
    "T60_SOURCE": (1.2, 1.0, 1.5),
    "T60_RECEIVER": (3.5, 2.8, 1.7),
    "T60_MIN_DURATION": 1.0,             # s; zusätzlich mind. 0.8 * Sabine-T60
}
# ================================================================

import csv
import math
import os
import sys
import time

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), CONFIG["MODULE_DIR"])))
import hybridcore as hc  # noqa: E402

# Farben (Referenzpalette, hell) — Kategorien in fester Reihenfolge, Text in Texttönen
SURFACE, TEXT, TEXT2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]   # blau, orange, aqua (validiert, 3 Slots)


def style_axes(ax):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=TEXT2, labelsize=9)
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.xaxis.label.set_color(TEXT2)
    ax.yaxis.label.set_color(TEXT2)


def sabine(L, a):
    V, S = L[0] * L[1] * L[2], 2 * (L[0] * L[1] + L[0] * L[2] + L[1] * L[2])
    return 0.161 * V / (a * S)


def eyring(L, a):
    V, S = L[0] * L[1] * L[2], 2 * (L[0] * L[1] + L[0] * L[2] + L[1] * L[2])
    return 0.161 * V / (-S * math.log(1 - a))


def analytic_modes(L, c, fmax):
    f = []
    for a in range(12):
        for b in range(12):
            for d in range(12):
                if a + b + d == 0:
                    continue
                v = 0.5 * c * math.sqrt((a / L[0]) ** 2 + (b / L[1]) ** 2 + (d / L[2]) ** 2)
                if v < fmax:
                    f.append((v, (a, b, d)))
    return sorted(f)


def peak_near(freqs, mag, f0, half):
    """Größtes Maximum in [f0-half, f0+half], parabolisch auf log-Betrag interpoliert."""
    k0, k1 = np.searchsorted(freqs, f0 - half), np.searchsorted(freqs, f0 + half)
    k = k0 + int(np.argmax(mag[k0:k1]))
    a, b, c = np.log(mag[k - 1:k + 2] + 1e-30)
    off = 0.5 * (a - c) / (a - 2 * b + c) if abs(a - 2 * b + c) > 1e-12 else 0.0
    return freqs[k] + off * (freqs[1] - freqs[0])


# ---------------------------------------------------------------- 1) Moden
def run_modes(out_dir):
    L, c = CONFIG["ROOM"], CONFIG["C"]
    cfg = hc.SimulationConfig(Lx=L[0], Ly=L[1], Lz=L[2], fmax=CONFIG["MODES_FMAX"], c=c)
    t0 = time.time()
    ir, fs, d = hc.simulate_point(cfg, CONFIG["MODES_SOURCE"], CONFIG["MODES_RECEIVER"],
                                  CONFIG["MODES_DURATION"], 0.0)
    print(f"[Moden] {d}  fs_sim={fs:.1f} Hz  {len(ir)} Schritte  ({time.time() - t0:.1f} s)")

    # Hann-Fenster gegen Nebenkeulen durch das Abschneiden der (unbedämpften) IR, Zero-Padding für feine Auflösung
    nfft = 1 << int(math.ceil(math.log2(len(ir) * 8)))
    mag = np.abs(np.fft.rfft(ir * np.hanning(len(ir)), nfft))
    freqs = np.fft.rfftfreq(nfft, 1.0 / fs)

    modes = analytic_modes(L, c, CONFIG["MODES_CHECK_MAX_HZ"])
    sp = CONFIG["MODES_MIN_SPACING_HZ"]
    rows = []
    for i, (f, n) in enumerate(modes):
        isolated = (i == 0 or f - modes[i - 1][0] > sp) and (i + 1 == len(modes) or modes[i + 1][0] - f > sp)
        if not isolated:
            continue
        fn = peak_near(freqs, mag, f, CONFIG["MODES_SEARCH_HZ"])
        rows.append({"mode": f"({n[0]},{n[1]},{n[2]})", "f_analytic_hz": round(f, 3),
                     "f_fdtd_hz": round(fn, 3), "delta_hz": round(fn - f, 3),
                     "delta_percent": round(100 * (fn - f) / f, 4)})

    with open(os.path.join(out_dir, "modes.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    print(f"{'Mode':>10} {'analytisch':>11} {'FDTD':>9} {'Δ %':>8}")
    for r in rows:
        print(f"{r['mode']:>10} {r['f_analytic_hz']:>11.2f} {r['f_fdtd_hz']:>9.2f} {r['delta_percent']:>8.3f}")
    worst = max(abs(r["delta_percent"]) for r in rows)
    print(f"  {len(rows)} isolierte Moden, max. |Δ| = {worst:.3f} %")

    # Plot: Spektrum (eine Serie) + analytische Eigenfrequenzen als Referenzlinien
    lo, hi = CONFIG["MODES_PLOT_RANGE_HZ"]
    sel = (freqs >= lo) & (freqs <= hi)
    db = 20 * np.log10(mag[sel] / mag[sel].max() + 1e-12)
    fig, ax = plt.subplots(figsize=(9, 4), dpi=150)
    fig.patch.set_facecolor(SURFACE)
    style_axes(ax)
    for j, (f, _) in enumerate(m for m in modes if lo <= m[0] <= hi):
        ax.axvline(f, color=TEXT2, linewidth=0.8, linestyle=(0, (3, 3)), alpha=0.6,
                   label="analytische Eigenfrequenz" if j == 0 else None)
    ax.plot(freqs[sel], db, color=SERIES[0], linewidth=1.5, label="FDTD-Spektrum")
    ax.set_xlim(lo, hi)
    ax.set_ylim(-60, 3)
    ax.set_xlabel("Frequenz (Hz)")
    ax.set_ylabel("Betrag (dB, normiert)")
    ax.set_title(f"Raummoden {L[0]:g} × {L[1]:g} × {L[2]:g} m, schallhart — max. Abweichung {worst:.2f} %",
                 color=TEXT, fontsize=11, loc="left")
    leg = ax.legend(loc="upper left", frameon=False, fontsize=9)
    for t in leg.get_texts():
        t.set_color(TEXT2)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "modes.png"), facecolor=SURFACE)
    plt.close(fig)
    return rows


# ---------------------------------------------------------------- 2) Nachhallzeit
def run_t60(out_dir):
    L, c = CONFIG["ROOM"], CONFIG["C"]
    cfg = hc.SimulationConfig(Lx=L[0], Ly=L[1], Lz=L[2], fmax=CONFIG["T60_FMAX"], c=c)
    rows = []
    for a in CONFIG["T60_ALPHAS"]:
        dur = max(CONFIG["T60_MIN_DURATION"], 0.8 * sabine(L, a))
        t0 = time.time()
        ir, fs, _ = hc.simulate_point(cfg, CONFIG["T60_SOURCE"], CONFIG["T60_RECEIVER"], dur, a)
        t60 = hc.estimate_t60(ir.astype(np.float32), int(round(fs)))
        rows.append({"alpha": a, "t60_fdtd_s": round(t60, 4), "t60_sabine_s": round(sabine(L, a), 4),
                     "t60_eyring_s": round(eyring(L, a), 4),
                     "fdtd_vs_sabine_percent": round(100 * (t60 / sabine(L, a) - 1), 2)})
        print(f"[T60] alpha={a:.2f}: FDTD {t60:.3f} s | Sabine {sabine(L, a):.3f} s | "
              f"Eyring {eyring(L, a):.3f} s  ({time.time() - t0:.1f} s)")

    with open(os.path.join(out_dir, "t60.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    # Plot: drei Serien, eine y-Achse (log), Marker + Direktbeschriftung zusätzlich zur Farbe
    alphas = [r["alpha"] for r in rows]
    series = [("FDTD", "t60_fdtd_s", "o"), ("Sabine", "t60_sabine_s", "s"), ("Eyring", "t60_eyring_s", "^")]
    fig, ax = plt.subplots(figsize=(7, 4.5), dpi=150)
    fig.patch.set_facecolor(SURFACE)
    style_axes(ax)
    for (name, key, marker), col in zip(series, SERIES):
        ys = [r[key] for r in rows]
        ax.plot(alphas, ys, color=col, linewidth=2, marker=marker, markersize=6,
                markeredgecolor=SURFACE, markeredgewidth=1.5, label=name)
        ax.annotate(name, (alphas[-1], ys[-1]), xytext=(8, 0), textcoords="offset points",
                    va="center", fontsize=9, color=TEXT2)
    ax.set_yscale("log")
    ticks = [t for t in (0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0)
             if min(min(r[k] for r in rows) for _, k, _ in series) * 0.8 <= t <= max(max(r[k] for r in rows) for _, k, _ in series) * 1.25]
    ax.set_yticks(ticks)
    ax.set_yticklabels([f"{t:g}" for t in ticks])
    ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    ax.set_xlim(0, max(alphas) * 1.18)
    ax.set_xlabel("Absorptionsgrad α (Diffusfeld)")
    ax.set_ylabel("T60 (s, log)")
    ax.set_title(f"Nachhallzeit vs. Wandabsorption — {L[0]:g} × {L[1]:g} × {L[2]:g} m, f ≤ {CONFIG['T60_FMAX']:g} Hz",
                 color=TEXT, fontsize=11, loc="left")
    leg = ax.legend(loc="upper right", frameon=False, fontsize=9)
    for t in leg.get_texts():
        t.set_color(TEXT2)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "t60.png"), facecolor=SURFACE)
    plt.close(fig)
    return rows


if __name__ == "__main__":
    out = os.path.abspath(os.path.join(os.path.dirname(__file__), CONFIG["OUT_DIR"]))
    os.makedirs(out, exist_ok=True)
    run_modes(out)
    run_t60(out)
    print(f"Ergebnisse in {out}")