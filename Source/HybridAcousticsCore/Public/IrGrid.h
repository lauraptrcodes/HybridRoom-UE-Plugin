#pragma once
#include "Export.h"
#include <cstddef>
#include <vector>

namespace fdtd {

    // Ergebnis eines Bakes: eine Impulsantwort pro Empfänger auf einem regelmäßigen Gitter.
    // Koordinaten in Metern, Ursprung in einer Raumecke, Z = Höhe.
    struct FDTD_API IrGrid {
        int nx = 0, ny = 0, nz = 0;   // Anzahl Empfänger je Achse
        float spacing = 0.f;          // Abstand der Empfänger (m)
        int length = 0;               // Samples pro IR
        int sampleRate = 0;           // Hz
        int channels = 1;             // 1 = omni; 4 = Ambisonics 1. Ordnung (ACN/N3D: W, Y, Z, X)
        std::vector<float> data;      // [(receiverIndex * channels + ch) * length + n]

        int numReceivers() const { return nx * ny * nz; }
        int index(int ix, int iy, int iz) const { return (iz * ny + iy) * nx + ix; }
        // Kanal 0 (omni bzw. W) des Empfängers idx
        const float* ir(int idx) const { return data.data() + (std::size_t)idx * channels * length; }
        const float* ir(int idx, int ch) const { return data.data() + ((std::size_t)idx * channels + ch) * length; }

        // Mittelpunkt des Empfängers idx (m)
        void position(int idx, float& x, float& y, float& z) const;

        // Index des nächsten Empfängers; Positionen außerhalb werden auf den Rand geklemmt -> nie leer
        int nearest(float x, float y, float z) const;
    };

} // namespace fdtd
