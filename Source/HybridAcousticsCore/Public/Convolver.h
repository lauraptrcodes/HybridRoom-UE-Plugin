// Convolver.h — partitionierte Faltung (Uniform Partitioned Overlap-Save), reines C++
// - beliebige Blockgrößen am Eingang, feste Latenz = blockSize Samples
// - IR-Wechsel mit Crossfade über einen Block (kein Klicken)
#pragma once
#include <complex>
#include <memory>
#include <vector>
#include "Export.h"

namespace fdtd {

    using cf = std::complex<float>;

    // Radix-2-FFT (n = Zweierpotenz)
    class Fft {
    public:
        explicit Fft(int n);
        void forward(cf* d) const { run(d, false); }
        void inverse(cf* d) const { run(d, true); }   // unskaliert
        int size() const { return n_; }
    private:
        void run(cf* d, bool inv) const;
        int n_;
        std::vector<int> rev_;
        std::vector<cf> tw_;
    };

    // Unveränderliche, vorab transformierte IR. Wird zwischen Threads per shared_ptr geteilt.
    struct PartitionedIr {
        int blockSize = 0;
        int numParts = 0;
        std::vector<cf> spectra;   // numParts * (2 * blockSize)
    };

    // blockSize muss Zweierpotenz sein
    FDTD_API std::shared_ptr<const PartitionedIr> partitionIr(const float* ir, int length, int blockSize);

    class FDTD_API Convolver {
    public:
        explicit Convolver(int blockSize);

        // Neue IR setzen; Übergang per Crossfade im nächsten Block. Gleicher Thread wie process().
        void setIr(std::shared_ptr<const PartitionedIr> ir);
        // in/out dürfen gleich sein
        void process(const float* in, float* out, int n);
        void reset();

        int latency() const { return B_; }

    private:
        void processBlock();
        void accumulate(const PartitionedIr& ir, std::vector<cf>& acc) const;
        void ensureParts(int parts);

        int B_, N_;
        Fft fft_;
        std::shared_ptr<const PartitionedIr> cur_, pending_;
        std::vector<cf> fdl_;          // Frequency-domain delay line: fdlParts_ * N_
        int fdlParts_ = 0, head_ = 0;
        std::vector<float> prev_, inBlk_, outBlk_;
        std::vector<cf> work_, acc_, acc2_;
        int fill_ = 0;
    };

} // namespace fdtdlite
