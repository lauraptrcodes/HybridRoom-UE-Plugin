// Convolver.cpp
#include "Convolver.h"
#include <algorithm>
#include <cmath>

namespace fdtd {

    // ---------------------------------------------------------------- FFT
    Fft::Fft(int n) : n_(n), rev_(n), tw_(n / 2) {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[i] = r;
        }
        const double kTwoPi = 6.283185307179586;
        for (int i = 0; i < n / 2; ++i)
            tw_[i] = cf((float)std::cos(kTwoPi * i / n), (float)-std::sin(kTwoPi * i / n));
    }

    void Fft::run(cf* d, bool inv) const {
        for (int i = 0; i < n_; ++i) if (i < rev_[i]) std::swap(d[i], d[rev_[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len / 2, step = n_ / len;
            for (int s = 0; s < n_; s += len)
                for (int k = 0; k < half; ++k) {
                    const cf w = inv ? std::conj(tw_[k * step]) : tw_[k * step];
                    const cf u = d[s + k], v = d[s + k + half] * w;
                    d[s + k] = u + v;
                    d[s + k + half] = u - v;
                }
        }
    }

    // ---------------------------------------------------------------- IR
    std::shared_ptr<const PartitionedIr> partitionIr(const float* ir, int length, int blockSize) {
        auto p = std::make_shared<PartitionedIr>();
        const int B = blockSize, N = 2 * B;
        p->blockSize = B;
        p->numParts = std::max(1, (length + B - 1) / B);
        p->spectra.assign((size_t)p->numParts * N, cf(0.f, 0.f));
        Fft fft(N);
        for (int k = 0; k < p->numParts; ++k) {
            cf* s = &p->spectra[(size_t)k * N];
            for (int i = 0; i < B; ++i) {
                const int idx = k * B + i;
                s[i] = cf(idx < length ? ir[idx] : 0.f, 0.f);   // zweite Hälfte bleibt 0
            }
            fft.forward(s);
        }
        return p;
    }

    // ---------------------------------------------------------------- Convolver
    Convolver::Convolver(int blockSize)
        : B_(blockSize), N_(2 * blockSize), fft_(2 * blockSize),
        prev_(blockSize, 0.f), inBlk_(blockSize, 0.f), outBlk_(blockSize, 0.f),
        work_(2 * blockSize), acc_(2 * blockSize), acc2_(2 * blockSize) {
    }

    void Convolver::ensureParts(int parts) {
        if (parts <= fdlParts_) return;
        // Selten (nur wenn eine längere IR kommt): Verlauf geht verloren, Ausgabe bleibt stabil
        fdl_.assign((size_t)parts * N_, cf(0.f, 0.f));
        fdlParts_ = parts;
        head_ = 0;
    }

    void Convolver::setIr(std::shared_ptr<const PartitionedIr> ir) {
        if (!ir || ir->blockSize != B_) return;
        ensureParts(ir->numParts);
        if (!cur_) cur_ = std::move(ir);      // erste IR: direkt übernehmen
        else pending_ = std::move(ir);        // sonst Crossfade im nächsten Block
    }

    void Convolver::reset() {
        std::fill(fdl_.begin(), fdl_.end(), cf(0.f, 0.f));
        std::fill(prev_.begin(), prev_.end(), 0.f);
        std::fill(inBlk_.begin(), inBlk_.end(), 0.f);
        std::fill(outBlk_.begin(), outBlk_.end(), 0.f);
        fill_ = 0;
        head_ = 0;
    }

    void Convolver::accumulate(const PartitionedIr& ir, std::vector<cf>& acc) const {
        std::fill(acc.begin(), acc.end(), cf(0.f, 0.f));
        const int P = std::min(ir.numParts, fdlParts_);
        for (int p = 0; p < P; ++p) {
            const int slot = (head_ - p + fdlParts_) % fdlParts_;
            const cf* X = &fdl_[(size_t)slot * N_];
            const cf* H = &ir.spectra[(size_t)p * N_];
            for (int i = 0; i < N_; ++i) acc[i] += X[i] * H[i];
        }
        fft_.inverse(acc.data());
    }

    void Convolver::processBlock() {
        // Overlap-Save: [vorheriger Block | aktueller Block]
        for (int i = 0; i < B_; ++i) { work_[i] = cf(prev_[i], 0.f); work_[B_ + i] = cf(inBlk_[i], 0.f); }
        std::copy(inBlk_.begin(), inBlk_.end(), prev_.begin());

        if (fdlParts_ == 0) { std::fill(outBlk_.begin(), outBlk_.end(), 0.f); return; }

        fft_.forward(work_.data());
        head_ = (head_ + 1) % fdlParts_;
        std::copy(work_.begin(), work_.end(), fdl_.begin() + (size_t)head_ * N_);

        const float scale = 1.f / (float)N_;
        if (cur_) accumulate(*cur_, acc_);
        else std::fill(acc_.begin(), acc_.end(), cf(0.f, 0.f));

        if (pending_) {
            accumulate(*pending_, acc2_);
            for (int i = 0; i < B_; ++i) {
                const float w = (i + 0.5f) / (float)B_;   // linearer Crossfade über einen Block
                outBlk_[i] = scale * ((1.f - w) * acc_[B_ + i].real() + w * acc2_[B_ + i].real());
            }
            cur_ = std::move(pending_);
            pending_.reset();
        }
        else {
            for (int i = 0; i < B_; ++i) outBlk_[i] = scale * acc_[B_ + i].real();
        }
    }

    void Convolver::process(const float* in, float* out, int n) {
        for (int i = 0; i < n; ++i) {
            const float x = in[i];
            out[i] = outBlk_[fill_];
            inBlk_[fill_] = x;
            if (++fill_ == B_) { processBlock(); fill_ = 0; }
        }
    }

}
