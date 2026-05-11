/**
    This file is part of OpenAirLink.

    OpenAirLink is free software: you can redistribute it and/or modify it under the terms of
    the GNU General Public License as published by the Free Software Foundation, either
    version 3 of the License, or (at your option) any later version.

    OpenAirLink is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
    without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along with OpenAirLink.
    If not, see <https://www.gnu.org/licenses/>.
**/

// Header-only floating-point reference implementation of the UPOLS
// streaming algorithm (spec streaming_upols_spec_v2.md).  Used by the
// host-side unit test and by anyone who wants to cross-check the FPGA
// block's output against a ground-truth model.
//
// Convention:
//   FFT   — DFT_K unscaled          (X[k] = sum_n x[n] * exp(-j2pi kn/K))
//   IFFT  — DFT_K^-1 WITH 1/K scaling (true IDFT)
//   H_parts[p][k] = (1/K) * FFT( h_padded[p*B..(p+1)*B-1] || zeros(B) )[k]
//
// With these conventions the spec's "both unscaled FFTs + bake 1/K into
// H_parts" simplification and the "true FFT / true IFFT" simplification
// are identical — we use the latter here because it reads more naturally
// in C++.

#ifndef INCLUDED_RFNOC_OPENAIRLINK_UPOLS_REFERENCE_HPP
#define INCLUDED_RFNOC_OPENAIRLINK_UPOLS_REFERENCE_HPP

#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace rfnoc { namespace openairlink { namespace upols_ref {

using cdouble = std::complex<double>;

struct params
{
    std::size_t N;  // filter length
    std::size_t B;  // block length
    std::size_t K;  // FFT size, K = 2*B
    std::size_t P;  // partitions, P = ceil(N/B)

    static params from_N_B(std::size_t N_, std::size_t B_)
    {
        params q;
        q.N = N_;
        q.B = B_;
        q.K = 2 * B_;
        q.P = (N_ + B_ - 1) / B_;
        return q;
    }
};

// ---------------------------------------------------------------------------
// In-place radix-2 forward FFT (unscaled).
// ---------------------------------------------------------------------------
inline void fft_forward(std::vector<cdouble>& x)
{
    const std::size_t K = x.size();
    if (K == 0 || (K & (K - 1)) != 0) {
        throw std::invalid_argument("fft_forward: size must be a power of 2");
    }
    std::size_t j = 0;
    for (std::size_t i = 1; i < K; ++i) {
        std::size_t bit = K >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t m = 2; m <= K; m <<= 1) {
        const std::size_t m2 = m >> 1;
        const double theta   = -2.0 * M_PI / static_cast<double>(m);
        const cdouble w_m(std::cos(theta), std::sin(theta));
        for (std::size_t k = 0; k < K; k += m) {
            cdouble w(1.0, 0.0);
            for (std::size_t jj = 0; jj < m2; ++jj) {
                const cdouble t = w * x[k + jj + m2];
                const cdouble u = x[k + jj];
                x[k + jj]      = u + t;
                x[k + jj + m2] = u - t;
                w *= w_m;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// In-place inverse FFT with 1/K scaling (true IDFT).
// ---------------------------------------------------------------------------
inline void fft_inverse(std::vector<cdouble>& x)
{
    const std::size_t K = x.size();
    // IDFT(X) = (1/K) * conj( DFT(conj(X)) )
    for (auto& v : x) v = std::conj(v);
    fft_forward(x);
    const double inv_K = 1.0 / static_cast<double>(K);
    for (auto& v : x) v = std::conj(v) * inv_K;
}

// ---------------------------------------------------------------------------
// Phase 1: compute H_parts from the channel impulse response.
//   Returns a P-by-K matrix (row = partition, col = frequency bin).
//   Each row is pre-scaled by 1/K.
// ---------------------------------------------------------------------------
inline std::vector<std::vector<cdouble>> prep_h_parts(
    const params& p,
    const std::vector<cdouble>& h)
{
    if (h.size() > p.N) {
        throw std::invalid_argument(
            "prep_h_parts: h.size()=" + std::to_string(h.size())
            + " exceeds N=" + std::to_string(p.N));
    }
    std::vector<cdouble> h_padded(p.P * p.B, cdouble(0.0, 0.0));
    std::copy(h.begin(), h.end(), h_padded.begin());

    std::vector<std::vector<cdouble>> H(p.P, std::vector<cdouble>(p.K));
    std::vector<cdouble> scratch(p.K);
    const double inv_K = 1.0 / static_cast<double>(p.K);

    for (std::size_t pp = 0; pp < p.P; ++pp) {
        std::fill(scratch.begin(), scratch.end(), cdouble(0.0, 0.0));
        std::copy(h_padded.begin() + pp * p.B,
                  h_padded.begin() + (pp + 1) * p.B,
                  scratch.begin());
        fft_forward(scratch);
        for (std::size_t k = 0; k < p.K; ++k) {
            H[pp][k] = scratch[k] * inv_K;
        }
    }
    return H;
}

// ---------------------------------------------------------------------------
// Phase 2: streaming state.  Holds input_buf, FDL, write_ptr, and a
//   reference to H_parts.  process_block() consumes B samples and returns B.
// ---------------------------------------------------------------------------
class stream_state
{
public:
    stream_state(const params& p, const std::vector<std::vector<cdouble>>& H_parts)
        : _p(p)
        , _H(&H_parts)
        , _input_buf(p.K, cdouble(0.0, 0.0))
        , _fdl(p.P, std::vector<cdouble>(p.K, cdouble(0.0, 0.0)))
        , _write_ptr(0)
        , _block_count(0)
    {
        if (H_parts.size() != p.P) {
            throw std::invalid_argument("stream_state: H_parts outer size != P");
        }
        for (const auto& row : H_parts) {
            if (row.size() != p.K) {
                throw std::invalid_argument("stream_state: H_parts row size != K");
            }
        }
    }

    void reset()
    {
        std::fill(_input_buf.begin(), _input_buf.end(), cdouble(0.0, 0.0));
        for (auto& row : _fdl) {
            std::fill(row.begin(), row.end(), cdouble(0.0, 0.0));
        }
        _write_ptr   = 0;
        _block_count = 0;
    }

    std::vector<cdouble> process_block(const std::vector<cdouble>& x_new)
    {
        if (x_new.size() != _p.B) {
            throw std::invalid_argument(
                "process_block: x_new.size()=" + std::to_string(x_new.size())
                + " != B=" + std::to_string(_p.B));
        }

        // Step 1: slide input buffer.  [prev_right | x_new]
        std::copy(_input_buf.begin() + _p.B, _input_buf.end(),
                  _input_buf.begin());
        std::copy(x_new.begin(), x_new.end(), _input_buf.begin() + _p.B);

        // Step 2: forward FFT of the K-point window.
        std::vector<cdouble> X_curr = _input_buf;  // value-copy
        fft_forward(X_curr);

        // Step 3: advance FDL write_ptr (post-increment per spec).
        _write_ptr = (_write_ptr + 1) % _p.P;
        _fdl[_write_ptr] = X_curr;

        // Step 4: frequency-domain MAC across all P partitions.
        std::vector<cdouble> Y(_p.K, cdouble(0.0, 0.0));
        for (std::size_t pp = 0; pp < _p.P; ++pp) {
            const std::size_t idx = (_write_ptr + _p.P - pp) % _p.P;
            const auto& Hp = (*_H)[pp];
            const auto& Xp = _fdl[idx];
            for (std::size_t k = 0; k < _p.K; ++k) {
                Y[k] += Hp[k] * Xp[k];
            }
        }

        // Step 5: inverse FFT (true IDFT — 1/K already baked into H_parts,
        //   so we do NOT re-apply it here; use the unscaled inverse instead.
        //   Equivalently: use fft_inverse() with H_parts un-pre-scaled, or
        //   fft_inverse_unscaled() with H_parts pre-scaled.  We do the
        //   second.)
        fft_inverse_unscaled(Y);

        // Step 6: overlap-save extract — keep the last B samples.
        return std::vector<cdouble>(Y.begin() + _p.B, Y.end());
    }

    std::size_t block_count() const { return _block_count; }

private:
    // Unscaled inverse: output = sum_k X[k] exp(+j2pi kn/K)  (i.e. K * IDFT)
    static void fft_inverse_unscaled(std::vector<cdouble>& x)
    {
        for (auto& v : x) v = std::conj(v);
        fft_forward(x);
        for (auto& v : x) v = std::conj(v);
    }

    params _p;
    const std::vector<std::vector<cdouble>>* _H;
    std::vector<cdouble> _input_buf;
    std::vector<std::vector<cdouble>> _fdl;
    std::size_t _write_ptr;
    std::size_t _block_count;
};

// ---------------------------------------------------------------------------
// Direct time-domain convolution (O(N*L) — used by self-tests only).
//   y[n] = sum_{k=0..N-1} h[k] * x[n-k]   for n in 0..L-1
//   where x[n<0] = 0.
// ---------------------------------------------------------------------------
inline std::vector<cdouble> direct_convolution(
    const std::vector<cdouble>& x,
    const std::vector<cdouble>& h,
    std::size_t L)
{
    std::vector<cdouble> y(L, cdouble(0.0, 0.0));
    for (std::size_t n = 0; n < L; ++n) {
        const std::size_t kmax = std::min(h.size(), n + 1);
        for (std::size_t k = 0; k < kmax; ++k) {
            y[n] += h[k] * x[n - k];
        }
    }
    return y;
}

}}} // namespace rfnoc::openairlink::upols_ref

#endif /* INCLUDED_RFNOC_OPENAIRLINK_UPOLS_REFERENCE_HPP */
