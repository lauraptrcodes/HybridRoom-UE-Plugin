#pragma once
#include "Export.h"
#include <vector>

namespace fdtd {

	FDTD_API float estimateT60(const float* ir, int length, int fs);
	FDTD_API double schroederFrequency(double T60, double volume);

	// Hybrid-IR: LR4-Tiefpass(FDTD-IR) + LR4-Hochpass(synthetischer Nachhall), Übergang bei fc.
	// directDelaySec = Laufzeit des Direktschalls (Abstand / c); ab dort beginnt der Rauschnachhall.
	FDTD_API std::vector<float> makeHybrid(const float* lowIr, int length, int fs, float fc, float T60,
		float directDelaySec, float tailGain = 1.0f, unsigned seed = 1);

	FDTD_API void applyDecay(float* ir, int length, int fs, float T60);
	FDTD_API std::vector<float> makeHybridGeo(const float* lowIr, int lowLength, const float* highIr, int highLength,
		int fs, float fc, float fmaxLow, float directDelaySec,
		float* gainOut = nullptr);

	// T60 = 24 ln10 V / (c S (-ln(1 - alpha)))   (c = 343 m/s -> 0.161 V / (-S ln(1 - alpha))); <= 0 bei alpha <= 0
	FDTD_API double eyringT60(double Lx, double Ly, double Lz, double alpha, double c = 343.0);
	// T60 im Band [fLo, fHi] (LR4-Hochpass bei fLo, LR4-Tiefpass bei fHi), Median über numIrs IRs.
	// irs liegt wie IrGrid::data: IR r beginnt bei irs + r * length. Rückgabe <= 0, wenn keine IR messbar ist.
	//FDTD_API float medianBandT60(const float* irs, int numIrs, int length, int fs, float fLo, float fHi);
	FDTD_API float medianBandT60(const float* irs, int numIrs, int length, int fs, float fLo, float fHi, int stride = 0);
	FDTD_API std::vector<float> fdtdToUnitIr(const float* ir, int length, int fs, double fmax, double dx, double dt,
		double c, double peak, double dist, double preDelaySec, double fLow = 20.0);
}
