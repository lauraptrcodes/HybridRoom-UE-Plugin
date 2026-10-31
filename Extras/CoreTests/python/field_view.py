"""field_view.py — Draufsicht auf die Schallausbreitung (FDTD-Teil der hybriden Simulation).

Gleicher C++-Kern und gleiche Farbskala wie der Unreal-Visualisierer (UHybridFieldViewComponent).
Erzeugt eine Bildfolge (PNG) und eine Animation (GIF) für die Thesis.
"""
import os
import sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib import animation

# ============================ CONFIG ============================
BUILD_DIR      = os.path.join(os.path.dirname(__file__), "..", "build")  # Ordner mit hybridcore.*.pyd/.so
OUT_DIR        = os.path.join(os.path.dirname(__file__), "results")

ROOM           = (5.0, 4.0, 3.0)        # Lx, Ly, Lz (m)
FMAX           = 600.0                  # obere Frequenz der Visualisierung (Hz), bestimmt dx
POINTS_PER_LAM = 10.0
WALL_ALPHA     = 0.10                   # Diffusfeld-Absorptionsgrad der Außenwände
SOURCE         = (2.5, 2.9, 1.5)        # Quelle (m)
SLICE_Z        = 1.5                    # Schnitthöhe (m); = Quellhöhe -> Quelle liegt im Bild
# Innenwände als Quader (min, max) in m; Beispiel: Schirm wie im Referenzbild
WALLS          = [((1.0, 0.0, 0.0), (1.5, 3.5, 3.0)),((1.5, 2.0, 0.0),(4.0,2.5,3.0))]

EXCITATION     = "pulse"                # "pulse" oder "sine"
SINE_FREQ      = 120.0                  # Hz (nur "sine")
PULSE_INTERVAL = 0.0                    # s, 0 = Einzelpuls

DURATION       = 0.030                  # simulierte Zeit (s)
FRAMES         = 90
GAMMA          = 0.5                    # < 1 hebt leise Anteile (Reflexionen) an
RMS_FACTOR     = 3.0                    # Skala je Bild = RMS_FACTOR * RMS(Bild), wie die Auto-Verstärkung in Unreal
STILLS_MS      = [3, 8, 13, 20]         # Einzelbilder (ms)
# ================================================================

sys.path.insert(0, os.path.abspath(BUILD_DIR))
import hybridcore as hc  # noqa: E402


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    cfg = hc.SimulationConfig(Lx=ROOM[0], Ly=ROOM[1], Lz=ROOM[2], fmax=FMAX, points_per_lambda=POINTS_PER_LAM)

    # dt hängt nur von dx ab -> vorab bestimmen, um Schritte pro Bild festzulegen
    dx = cfg.c / (FMAX * POINTS_PER_LAM)
    dt = 0.99 * dx / (cfg.c * np.sqrt(3.0))
    steps_per_frame = max(1, int(round(DURATION / FRAMES / dt)))

    r = hc.simulate_field(cfg, SOURCE, SLICE_Z, FRAMES, steps_per_frame, WALL_ALPHA, WALLS,
                          EXCITATION, SINE_FREQ, PULSE_INTERVAL)
    field, fdt = r["field"], r["frame_dt"]
    ny, nx = field.shape[1:]
    print(f"Gitter {r['cfg']}, Schnitt {nx}x{ny}, {steps_per_frame} Schritte/Bild, Bildabstand {fdt*1e3:.3f} ms")
    print(f"Mittelwert im Schnitt: max |mean| = {np.nanmax(np.abs(r['slice_mean'])):.3e} (Drift-Kontrolle)")

    extent = (0, nx * r["dx"], 0, ny * r["dx"])

    def rgba(f):
        # RMS statt Spitzenwert: das Nahfeld an der Quelle dominiert sonst die Skala
        scale = RMS_FACTOR * np.sqrt(np.nanmean(field[f] ** 2))
        return hc.colorize(field[f], float(scale), GAMMA)

    # --- Einzelbilder ---
    idx = [min(FRAMES - 1, int(round(ms * 1e-3 / fdt)) - 1) for ms in STILLS_MS]
    fig, axs = plt.subplots(1, len(idx), figsize=(4 * len(idx), 3.4))
    for ax, f in zip(np.atleast_1d(axs), idx):
        ax.imshow(rgba(f), origin="lower", extent=extent, interpolation="bilinear")
        ax.plot(SOURCE[0], SOURCE[1], "k.", ms=4)
        ax.set_title(f"t = {(f + 1) * fdt * 1e3:.1f} ms")
        ax.set_xlabel("x (m)")
    np.atleast_1d(axs)[0].set_ylabel("y (m)")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "field_stills.png"), dpi=150)

    # --- Animation ---
    fig2, ax2 = plt.subplots(figsize=(5, 4.2))
    im = ax2.imshow(rgba(0), origin="lower", extent=extent, interpolation="bilinear")
    ax2.plot(SOURCE[0], SOURCE[1], "k.", ms=4)
    title = ax2.set_title("")
    ax2.set_xlabel("x (m)"); ax2.set_ylabel("y (m)")

    def update(f):
        im.set_data(rgba(f))
        title.set_text(f"t = {(f + 1) * fdt * 1e3:.1f} ms")
        return im, title

    anim = animation.FuncAnimation(fig2, update, frames=FRAMES, interval=60, blit=False)
    anim.save(os.path.join(OUT_DIR, "field.gif"), writer=animation.PillowWriter(fps=16))
    print("geschrieben:", os.path.join(OUT_DIR, "field_stills.png"), "und field.gif")


if __name__ == "__main__":
    main()
