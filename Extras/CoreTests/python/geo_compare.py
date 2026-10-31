"""geo_compare.py — FDTD (Tiefton) + Steam Audio (Hochton) im selben Raum: Bake, Hybrid-IR, Vergleich.

Beide Bakes nutzen dieselbe Szene (Quader + Innenwände) und dasselbe Empfängergitter,
d. h. ir[r] aus bake() und bake_geometric() gehören zur selben Position.
Ausgabe: Abbildung (IRs, Energieabklingkurven je Band, Spektrogramm), T60-Tabelle (CSV) und WAV zum Anhören.
"""
import csv
import os
import sys
import time
import wave

import numpy as np
import matplotlib.pyplot as plt
from scipy.signal import butter, sosfilt, spectrogram

# ============================ CONFIG ============================
BUILD_DIR       = os.path.join(os.path.dirname(__file__), "..", "build")
OUT_DIR         = os.path.join(os.path.dirname(__file__), "results")

ROOM            = (5.0, 4.0, 3.0)          # Lx, Ly, Lz (m)
SOURCE          = (1.2, 1.0, 1.5)          # m
RECEIVER        = (3.6, 2.9, 1.6)          # ausgewerteter Empfänger (nächster Gitterpunkt wird genommen)
WALL_ALPHA      = 0.10                     # Absorptionsgrad Wände (FDTD und Steam Audio)
WALLS           = []                       # Innenwände [((x0,y0,z0),(x1,y1,z1)), ...] in m
#WALLS          = [((1.9, 1.95, 0.0), (3.1, 2.05, 3.0))]

FC              = "auto"                   # Übergangsfrequenz (Hz) oder "auto" = Schroeder-Frequenz aus Steam-T60
FC_RANGE        = (80.0, 400.0)            # Grenzen für "auto" (FDTD-Rechenzeit ~ fmax^4)
T60_BAND        = (283.0, 566.0)           # Oktave um 400 Hz (unterstes Steam-Audio-Band) für die T60-Messung
FMAX_FACTOR     = 2.0                      # FDTD rechnet bis FMAX_FACTOR * FC
FDTD_ORDER      = 2                        # 2 oder 4 (Laplace-Stencil); 4 erlaubt ~6 statt 10 Punkte/Wellenlänge
POINTS_PER_LAM  = 10.0                     # Gitterpunkte pro Wellenlänge bei fmax
PRE_DELAY       = 0.005                    # s, Lage des Direktschalls in der IR (wie DirectPreDelayMs im Plugin)
IR_LENGTH       = 1.0                      # s
RECEIVER_SPACING = 1.0                     # m
FS              = 48000

STEAM_RAYS      = 16384
STEAM_SCATTERING = 0.05
STEAM_ABSORPTION = (-1.0, -1.0, -1.0)      # je Band (400 Hz, 2.5 kHz, 15 kHz); -1 = WALL_ALPHA
STEAM_THREADS   = 4

# Bake-Cache: gleiche Eingaben -> Datei laden statt neu backen. None = immer neu backen.
CACHE_FILE      = os.path.join(os.path.dirname(__file__), "results", "geo_compare.hacbake")
# Statt selbst zu backen eine Bake-Datei aus Unreal auswerten (Saved/HybridAcoustics/<Level>_<Actor>.hacbake).
# Raum, Quelle usw. kommen dann aus der Datei; die Werte oben werden ignoriert.
LOAD_FILE       = None

BANDS           = {"125 Hz": (88, 177), "250 Hz": (177, 355), "500 Hz": (355, 710),
                   "1 kHz": (710, 1420), "4 kHz": (2840, 5680)}
# ================================================================

sys.path.insert(0, os.path.abspath(BUILD_DIR))
import hybridcore as hc  # noqa: E402


def edc_db(x):
    e = np.cumsum((x.astype(np.float64) ** 2)[::-1])[::-1]
    return 10 * np.log10(e / e[0] + 1e-30)


def band(x, lo, hi, fs):
    return sosfilt(butter(4, [lo, hi], "bandpass", fs=fs, output="sos"), x).astype(np.float32)


def write_wav(path, x, fs):
    """x: (N,) mono oder (N, 2) stereo; gemeinsam normiert"""
    y = x / (np.max(np.abs(x)) + 1e-12) * 0.9
    with wave.open(path, "wb") as w:
        w.setnchannels(1 if y.ndim == 1 else y.shape[1]); w.setsampwidth(2); w.setframerate(fs)
        w.writeframes((y * 32767).astype(np.int16).tobytes())


def main():
    if not hc.has_steam_audio:
        sys.exit("hybridcore wurde ohne Steam Audio gebaut (cmake ... -DSTEAMAUDIO_DIR=<Pfad>/steamaudio)")
    os.makedirs(OUT_DIR, exist_ok=True)

    global ROOM, SOURCE, WALL_ALPHA, FS
    t = time.time()
    if LOAD_FILE:
        res = hc.load_bake(LOAD_FILE)
        c = res["cfg"]; ROOM = (c.Lx, c.Ly, c.Lz); SOURCE = res["source"]
        WALL_ALPHA = res["wall_alpha"]; FS = res["sample_rate"]
        print(f"geladen: {LOAD_FILE} (Raum {ROOM}, Quelle {SOURCE})")
    else:
        bs = hc.BakeSettings(source=SOURCE, ir_length=IR_LENGTH, receiver_spacing=RECEIVER_SPACING,
                             out_rate=FS, wall_alpha=WALL_ALPHA, walls=WALLS)
        cfg = hc.SimulationConfig(Lx=ROOM[0], Ly=ROOM[1], Lz=ROOM[2], points_per_lambda=POINTS_PER_LAM,
                                  order=FDTD_ORDER)
        auto = FC == "auto"
        res = hc.hybrid_bake(cfg, bs, use_geo=True, absorption=STEAM_ABSORPTION, scattering=STEAM_SCATTERING,
                             num_rays=STEAM_RAYS, num_threads=STEAM_THREADS, auto_fc=auto,
                             fixed_fc=200.0 if auto else float(FC), fc_min=FC_RANGE[0], fc_max=FC_RANGE[1],
                             fmax_factor=FMAX_FACTOR, cache_path=CACHE_FILE or "")
    V = ROOM[0] * ROOM[1] * ROOM[2]
    fc, fmax = res["fc"], res["fmax"]
    if res["geo_ir"] is None:
        sys.exit("Bake enthält keinen Steam-Audio-Anteil")
    print(f"{'aus Cache geladen' if res['from_cache'] else 'neu gebacken'} in {time.time() - t:.1f} s; "
          f"T60 = {res['t60']:.2f} s ({res['t60_source']}) -> f_S = 2000*sqrt({res['t60']:.2f}/{V:.0f}) = "
          f"{res['fs']:.0f} Hz -> Übergang {fc:.0f} Hz, FDTD bis {fmax:.0f} Hz")
    low = {"ir": res["ir"], "positions": res["positions"]}
    geo = {"ir": res["geo_ir"]}

    # Empfänger wählen
    pos = low["positions"]
    r = int(np.argmin(np.sum((pos - np.array(RECEIVER)) ** 2, axis=1)))
    dist = float(np.linalg.norm(pos[r] - np.array(SOURCE)))
    h_low = low["ir"][r]
    h_geo = np.zeros_like(h_low)                       # Steam-IR auf FDTD-Länge bringen (Länge hängt von dt ab)
    n = min(len(h_low), len(geo["ir"][r])); h_geo[:n] = geo["ir"][r][:n]
    # Wet-IR genau wie im Plugin: Direktschall (Tiefton, aus FDTD) = Einheitsimpuls bei PRE_DELAY + Reflexionen
    h_hyb, g = res["bake"].receiver_ir(r, PRE_DELAY)
    # Gesamt-IR wie gehört: + Dry-Pfad = Einheitsimpuls bei PRE_DELAY durch LR4-Hochpass bei fc
    dry = np.zeros_like(h_hyb); dry[int(round(PRE_DELAY * FS))] = 1.0
    sos_hp = np.vstack([butter(2, fc, "high", fs=FS, output="sos")] * 2)
    h_tot = h_hyb + sosfilt(sos_hp, dry).astype(np.float32)
    n_rev = int((PRE_DELAY + 0.003) * FS)
    dr = 10 * np.log10(1.0 / np.sum(h_hyb[n_rev:].astype(np.float64) ** 2))
    rc = 0.057 * np.sqrt(V / res["t60"])
    print(f"Empfänger {r} bei {pos[r]}, Abstand {dist:.2f} m, Pegelfaktor Steam g = {g:.3e}, "
          f"D/R = {dr:+.1f} dB (diffuses Feld (r_c/r)^2: {20 * np.log10(rc / dist):+.1f} dB, r_c = {rc:.2f} m)")

    # T60 je Band
    S = 2 * (ROOM[0] * ROOM[1] + ROOM[0] * ROOM[2] + ROOM[1] * ROOM[2])
    sab = 0.161 * V / (WALL_ALPHA * S) if WALL_ALPHA > 0 else float("inf")
    eyr = 0.161 * V / (-S * np.log(1 - WALL_ALPHA)) if WALL_ALPHA > 0 else float("inf")
    rows = []
    for name, (lo, hi) in BANDS.items():
        def t60(x):
            v = hc.estimate_t60(band(x, lo, hi, FS), FS)
            return v if v > 0 else float("nan")
        rows.append((name, t60(h_low) if hi <= fmax else float("nan"), t60(g * h_geo), t60(h_hyb)))
    print(f"\n{'Band':>8} {'FDTD':>7} {'Steam':>7} {'Hybrid':>7}   (Sabine {sab:.2f} s, Eyring {eyr:.2f} s)")
    for name, a, b, c in rows:
        print(f"{name:>8} {a:7.2f} {b:7.2f} {c:7.2f}")
    with open(os.path.join(OUT_DIR, "geo_t60.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(["band", "t60_fdtd", "t60_steam", "t60_hybrid", "sabine", "eyring"])
        for row in rows: w.writerow([*row, sab, eyr])

    # Abbildung
    tt = np.arange(len(h_hyb)) / FS * 1e3
    fig, ax = plt.subplots(2, 2, figsize=(12, 7))
    sos_lp = np.vstack([butter(2, fc, "low", fs=FS, output="sos")] * 2)
    ax[0, 0].plot(tt, sosfilt(sos_lp, h_tot), lw=0.8, label=f"< {fc:.0f} Hz (FDTD: Direktschall + Reflexionen)")
    ax[0, 0].plot(tt, sosfilt(sos_hp, h_tot), lw=0.6, alpha=0.7, label=f"> {fc:.0f} Hz (Dry-Direktschall + Steam)")
    ax[0, 0].set_xlim(0, 60); ax[0, 0].set_xlabel("t (ms)"); ax[0, 0].legend(fontsize=8)
    ax[0, 0].set_title("Gesamt-IR wie gehört, Anfang (Direktschall = 1)")
    ax[0, 1].plot(tt, h_hyb, lw=0.6, color="k")
    ax[0, 1].set_xlim(0, 150); ax[0, 1].set_ylim(-0.1, 0.1); ax[0, 1].set_xlabel("t (ms)")
    ax[0, 1].set_title(f"Wet-IR (Übergang {fc:.0f} Hz), Ausschnitt ±0.1")
    for name, (lo, hi) in BANDS.items():
        ax[1, 0].plot(tt, edc_db(band(h_hyb, lo, hi, FS)), label=name)
    ax[1, 0].set_ylim(-60, 2); ax[1, 0].set_xlabel("t (ms)"); ax[1, 0].set_ylabel("dB")
    ax[1, 0].set_title("Energieabklingkurven der Hybrid-IR je Band"); ax[1, 0].legend()
    f, ts, Sxx = spectrogram(h_hyb, FS, nperseg=1024, noverlap=896)
    ax[1, 1].pcolormesh(ts * 1e3, f, 10 * np.log10(Sxx + 1e-20), shading="auto", vmin=10 * np.log10(Sxx.max()) - 70)
    ax[1, 1].axhline(fc, color="w", ls="--", lw=0.8)
    ax[1, 1].set_yscale("symlog", linthresh=100); ax[1, 1].set_ylim(30, FS / 2)
    ax[1, 1].set_xlabel("t (ms)"); ax[1, 1].set_ylabel("f (Hz)"); ax[1, 1].set_title("Spektrogramm Hybrid-IR")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "geo_compare.png"), dpi=150)

    write_wav(os.path.join(OUT_DIR, "ir_hybrid.wav"), h_tot, FS)

    # Hörvergleich Dekorrelation: Mono-Nachhall auf beiden Ohren vs. unabhängiger Hochton-Nachhall je Ohr
    # (Direktschall hier mittig ohne HRTF, damit nur der Nachhall verglichen wird)
    low, hf_l, hf_r, _ = res["bake"].receiver_stereo_ir(r, PRE_DELAY)
    dry_hp = sosfilt(sos_hp, dry).astype(np.float32)
    stereo = np.stack([low + dry_hp + hf_l, low + dry_hp + hf_r], axis=1)
    mono2 = np.stack([h_tot, h_tot], axis=1)
    write_wav(os.path.join(OUT_DIR, "ir_reverb_mono.wav"), mono2, FS)
    write_wav(os.path.join(OUT_DIR, "ir_reverb_decorrelated.wav"), stereo, FS)
    write_wav(os.path.join(OUT_DIR, "ir_steam.wav"), h_geo, FS)
    print("\ngeschrieben: geo_compare.png, geo_t60.csv, ir_hybrid.wav, ir_steam.wav,\n"
          "             ir_reverb_mono.wav, ir_reverb_decorrelated.wav (Kopfhörer-Vergleich) in", OUT_DIR)


if __name__ == "__main__":
    main()
