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

#include <rfnoc/openairlink/upols_block_control.hpp>

#include <uhd/exception.hpp>
#include <uhd/rfnoc/defaults.hpp>
#include <uhd/rfnoc/registry.hpp>
#include <uhd/utils/log.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace rfnoc::openairlink;
using namespace uhd::rfnoc;

// -------------------------------------------------------------------------
// Register addresses (must match rfnoc_upols_regs.vh)
// -------------------------------------------------------------------------
const uint32_t upols_block_control::REG_COMPAT_NUM = 0x00;
const uint32_t upols_block_control::REG_N          = 0x04;
const uint32_t upols_block_control::REG_B          = 0x08;
const uint32_t upols_block_control::REG_K          = 0x0C;
const uint32_t upols_block_control::REG_P          = 0x10;
const uint32_t upols_block_control::REG_CTRL       = 0x14;
const uint32_t upols_block_control::REG_STATUS     = 0x18;
const uint32_t upols_block_control::REG_H_ADDR     = 0x20;
const uint32_t upols_block_control::REG_H_DATA     = 0x24;

const uint32_t upols_block_control::CTRL_SOFT_RESET_MASK  = 0x1;
const uint32_t upols_block_control::STATUS_H_LOADED_MASK  = 0x1;
const uint32_t upols_block_control::STATUS_OVERFLOW_MASK  = 0x2;
const uint32_t upols_block_control::STATUS_UNDERFLOW_MASK = 0x4;

namespace {

// -------------------------------------------------------------------------
// Q1.15 quantization helper.
//   Maps a real value in [-1, 1) to a signed 16-bit integer with saturation.
//   Uses round-half-away-from-zero.  Value +1.0 saturates to +32767 (not
//   +32768) so the return always fits in int16_t.
// -------------------------------------------------------------------------
int16_t quantize_q15(double x)
{
    const double scaled  = x * 32768.0;
    const double rounded = std::floor(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
    if (rounded >  32767.0) return  32767;
    if (rounded < -32768.0) return -32768;
    return static_cast<int16_t>(rounded);
}

// -------------------------------------------------------------------------
// Iterative radix-2 decimation-in-time FFT (forward).
//   Input  : complex vector of length K, K a power of 2.
//   Output : in-place overwrite with DFT_K(x).
//   Convention: X[k] = sum_{n=0..K-1} x[n] * exp(-j*2*pi*k*n/K)   (unscaled).
// -------------------------------------------------------------------------
void fft_radix2_forward(std::vector<std::complex<double>>& x)
{
    const size_t K = x.size();
    if (K == 0 || (K & (K - 1)) != 0) {
        throw uhd::value_error("fft_radix2_forward: size must be a power of 2");
    }

    // Bit-reversal permutation (iterative).  For each i, advance j so that
    // j is bit-reversed index of i; swap when i < j to avoid double swaps.
    size_t j = 0;
    for (size_t i = 1; i < K; ++i) {
        size_t bit = K >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(x[i], x[j]);
        }
    }

    // Iterative Cooley-Tukey butterflies.  Stage size m doubles each pass.
    for (size_t m = 2; m <= K; m <<= 1) {
        const size_t m2    = m >> 1;
        const double theta = -2.0 * M_PI / static_cast<double>(m);
        const std::complex<double> w_m(std::cos(theta), std::sin(theta));
        for (size_t k = 0; k < K; k += m) {
            std::complex<double> w(1.0, 0.0);
            for (size_t jj = 0; jj < m2; ++jj) {
                const std::complex<double> t = w * x[k + jj + m2];
                const std::complex<double> u = x[k + jj];
                x[k + jj]      = u + t;
                x[k + jj + m2] = u - t;
                w *= w_m;
            }
        }
    }
}

} // namespace

// -------------------------------------------------------------------------
// upols_block_control_impl
// -------------------------------------------------------------------------
class upols_block_control_impl : public upols_block_control
{
public:
    RFNOC_BLOCK_CONSTRUCTOR(upols_block_control)
    {
        // Read compile-time parameters from FPGA
        _N = regs().peek32(REG_N);
        _B = regs().peek32(REG_B);
        _K = regs().peek32(REG_K);
        _P = regs().peek32(REG_P);

        // Cross-check the algebraic constraints (catches a mismatched
        // block YAML / HDL parameter set early).
        if (_K != 2 * _B) {
            throw uhd::runtime_error(
                "UPOLS block reports K=" + std::to_string(_K)
                + " but expected K=2*B=" + std::to_string(2 * _B));
        }
        const uint32_t expected_P = (_N + _B - 1) / _B;
        if (_P != expected_P) {
            throw uhd::runtime_error(
                "UPOLS block reports P=" + std::to_string(_P)
                + " but expected P=ceil(N/B)=" + std::to_string(expected_P));
        }
        if ((_K & (_K - 1)) != 0) {
            throw uhd::runtime_error(
                "UPOLS block reports non-power-of-2 K=" + std::to_string(_K));
        }

        // Expose in the property tree
        auto tree_root = get_block_id().get_tree_root();
        get_tree()->create<uint32_t>(tree_root / "N").set(_N);
        get_tree()->create<uint32_t>(tree_root / "B").set(_B);
        get_tree()->create<uint32_t>(tree_root / "K").set(_K);
        get_tree()->create<uint32_t>(tree_root / "P").set(_P);

        UHD_LOGGER_INFO("UPOLS")
            << get_unique_id() << ": UPOLS FIR, N=" << _N
            << " B=" << _B << " K=" << _K << " P=" << _P;

        // Start with state cleared
        soft_reset();
    }

    // ---- Compile-time parameter readback ----
    uint32_t get_N() override { return _N; }
    uint32_t get_B() override { return _B; }
    uint32_t get_K() override { return _K; }
    uint32_t get_P() override { return _P; }

    // ---- Control / status ----
    void soft_reset() override
    {
        // CTRL bit 0 is self-clearing: one-shot pulse in the FPGA.
        regs().poke32(REG_CTRL, CTRL_SOFT_RESET_MASK);
    }

    bool get_h_loaded() override
    {
        return (regs().peek32(REG_STATUS) & STATUS_H_LOADED_MASK) != 0;
    }

    bool get_overflow() override
    {
        return (regs().peek32(REG_STATUS) & STATUS_OVERFLOW_MASK) != 0;
    }

    bool get_underflow() override
    {
        return (regs().peek32(REG_STATUS) & STATUS_UNDERFLOW_MASK) != 0;
    }

    // ---- Channel loading ----
    void set_channel_dense(const std::vector<std::complex<double>>& h) override
    {
        if (h.size() > _N) {
            throw uhd::value_error(
                "set_channel_dense: impulse response length "
                + std::to_string(h.size())
                + " exceeds N=" + std::to_string(_N));
        }

        // Zero-pad to P*B (covers all P partitions fully).
        std::vector<std::complex<double>> h_padded(
            static_cast<size_t>(_P) * _B, std::complex<double>(0.0, 0.0));
        std::copy(h.begin(), h.end(), h_padded.begin());

        // Compose the full H_parts tensor (P * K complex doubles).
        const size_t PK = static_cast<size_t>(_P) * _K;
        std::vector<std::complex<double>> h_parts_flat(PK);

        std::vector<std::complex<double>> scratch(_K);
        const double inv_K = 1.0 / static_cast<double>(_K);

        for (uint32_t p = 0; p < _P; ++p) {
            // scratch = [ h_padded[p*B .. p*B+B-1], zeros(B) ]
            std::fill(scratch.begin(), scratch.end(),
                      std::complex<double>(0.0, 0.0));
            std::copy(h_padded.begin() + p * _B,
                      h_padded.begin() + (p + 1) * _B,
                      scratch.begin());

            fft_radix2_forward(scratch);

            // Scale by 1/K (bake in the IFFT normalization) and store in
            // partition-major flat layout: flat_idx = p*K + k.
            for (uint32_t k = 0; k < _K; ++k) {
                h_parts_flat[p * _K + k] = scratch[k] * inv_K;
            }
        }

        // Upload to FPGA.
        // 1. Rewind the H_parts write window.
        regs().poke32(REG_H_ADDR, 0);

        // 2. Stream P*K packed Q1.15 complex words via H_DATA (auto-increments).
        for (size_t i = 0; i < PK; ++i) {
            const int16_t re_q = quantize_q15(h_parts_flat[i].real());
            const int16_t im_q = quantize_q15(h_parts_flat[i].imag());
            const uint32_t packed =
                (static_cast<uint32_t>(static_cast<uint16_t>(im_q)) << 16)
                | static_cast<uint32_t>(static_cast<uint16_t>(re_q));
            regs().poke32(REG_H_DATA, packed);
        }

        // 3. Confirm the FPGA saw a full upload.
        if (!get_h_loaded()) {
            throw uhd::runtime_error(
                "UPOLS: H_parts upload completed but h_loaded status bit "
                "is not set (expected " + std::to_string(PK)
                + " words).  Check ctrlport integrity.");
        }

        UHD_LOGGER_INFO("UPOLS")
            << get_unique_id() << ": channel loaded (" << PK
            << " H_parts words, " << h.size() << " taps)";
    }

    void set_channel_sparse(
        const std::vector<uint32_t>& delays,
        const std::vector<std::complex<double>>& coeffs) override
    {
        if (delays.size() != coeffs.size()) {
            throw uhd::value_error(
                "set_channel_sparse: delays and coeffs size mismatch ("
                + std::to_string(delays.size()) + " vs "
                + std::to_string(coeffs.size()) + ")");
        }

        // Expand sparse -> dense.  Duplicate delays are summed (a physically
        // meaningful outcome: two rays at the same sample lag combine).
        std::vector<std::complex<double>> h(_N, std::complex<double>(0.0, 0.0));
        for (size_t i = 0; i < delays.size(); ++i) {
            if (delays[i] < _N) {
                h[delays[i]] += coeffs[i];
            }
            // Delays >= N fall outside the filter support and are dropped
            // silently — caller is responsible for matching channel length
            // to the block's N.
        }

        set_channel_dense(h);
    }

private:
    uint32_t _N = 0;
    uint32_t _B = 0;
    uint32_t _K = 0;
    uint32_t _P = 0;
};

// NOC_ID must match the one in noc_shell_upols.v (0x5F1A0005)
UHD_RFNOC_BLOCK_REGISTER_DIRECT(
    upols_block_control, 0x5F1A0005, "UPOLS", CLOCK_KEY_GRAPH, "bus_clk")
