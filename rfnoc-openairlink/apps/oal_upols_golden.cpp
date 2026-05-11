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

// oal_upols_golden:
//   Self-test harness for the UPOLS reference model
//   (include/rfnoc/openairlink/upols_reference.hpp) and a generator of
//   binary test vectors for the FPGA testbench.
//
// Usage:
//   ./oal_upols_golden [--selftest] [--dump <dir>]
//
//   --selftest  Runs internal correctness checks (impulse, random channel
//               vs. direct conv, reset protocol) and prints PASS/FAIL.
//               This is the default action if no flag is given.
//   --dump DIR  After the self-tests, write three files into DIR:
//                 x_in.sc16      length N_BLOCKS*B, complex int16,
//                                I/Q packed as {I[31:16], Q[15:0]}
//                 h_parts.sc16   length P*K, H_parts flat in partition-
//                                major order, {im[31:16], re[15:0]}
//                 y_expected.sc16 length N_BLOCKS*B, same encoding as x_in
//               The testbench feeds x_in.sc16 + h_parts.sc16 into the
//               block and compares its output against y_expected.sc16
//               (with the documented quantization tolerance).

#include <rfnoc/openairlink/upols_reference.hpp>

#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using cdouble = std::complex<double>;
namespace up = rfnoc::openairlink::upols_ref;

// ---------------------------------------------------------------------------
// Test parameters — keep these small so the self-test runs in < 1s but
//   still exercises a non-trivial partition count.
// ---------------------------------------------------------------------------
static constexpr std::size_t SELFTEST_N        = 64;
static constexpr std::size_t SELFTEST_B        = 16;
static constexpr std::size_t SELFTEST_P        = 4;   // = ceil(N/B)
static constexpr std::size_t SELFTEST_K        = 32;  // = 2*B
static constexpr std::size_t SELFTEST_N_BLOCKS = 16;  // input blocks to stream

// ---------------------------------------------------------------------------
// Q1.15 quantization helper (must match host controller's quantize_q15).
// ---------------------------------------------------------------------------
static int16_t quantize_q15(double x)
{
    const double s = x * 32768.0;
    const double r = std::floor(s + (s >= 0.0 ? 0.5 : -0.5));
    if (r >  32767.0) return  32767;
    if (r < -32768.0) return -32768;
    return static_cast<int16_t>(r);
}

// ---------------------------------------------------------------------------
// Helpers for sc16 packing — I occupies the upper 16 bits, Q the lower.
// ---------------------------------------------------------------------------
static uint32_t pack_sc16(int16_t I, int16_t Q)
{
    return (static_cast<uint32_t>(static_cast<uint16_t>(I)) << 16)
         |  static_cast<uint32_t>(static_cast<uint16_t>(Q));
}

// Same packing as the ctrlport H_DATA register: {im[31:16], re[15:0]}.
static uint32_t pack_h(const cdouble& c)
{
    return (static_cast<uint32_t>(static_cast<uint16_t>(quantize_q15(c.imag()))) << 16)
         |  static_cast<uint32_t>(static_cast<uint16_t>(quantize_q15(c.real())));
}

static void write_u32_file(const std::string& path, const std::vector<uint32_t>& v)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "ERROR: failed to open " << path << " for writing\n";
        std::exit(2);
    }
    f.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(uint32_t)));
}

// ---------------------------------------------------------------------------
// TEST 1: impulse response recovery.
//   Input a single sample at n=0 and verify the filtered output across
//   N_BLOCKS matches h shifted by the OLS algorithmic delay (which is 0
//   for the UPOLS reference — block k's output represents conv samples
//   [k*B .. (k+1)*B - 1]).
// ---------------------------------------------------------------------------
static bool test_impulse()
{
    const auto p = up::params::from_N_B(SELFTEST_N, SELFTEST_B);

    // Construct a recognizable channel: h[n] = (n+1) * 0.01 * (1 + 0.5j)
    // to exercise both real and imaginary paths.
    std::vector<cdouble> h(p.N);
    for (std::size_t n = 0; n < p.N; ++n) {
        h[n] = cdouble(0.01 * (n + 1), 0.005 * (n + 1));
    }

    auto H_parts = up::prep_h_parts(p, h);
    up::stream_state s(p, H_parts);

    // Impulse input: x[0] = 1, rest zeros.
    std::vector<cdouble> x_full(SELFTEST_N_BLOCKS * p.B, cdouble(0.0, 0.0));
    x_full[0] = cdouble(1.0, 0.0);

    // Expected output = h zero-padded on the right.
    std::vector<cdouble> y_expected(SELFTEST_N_BLOCKS * p.B, cdouble(0.0, 0.0));
    for (std::size_t n = 0; n < p.N && n < y_expected.size(); ++n) {
        y_expected[n] = h[n];
    }

    double max_err = 0.0;
    for (std::size_t blk = 0; blk < SELFTEST_N_BLOCKS; ++blk) {
        std::vector<cdouble> x_blk(x_full.begin() + blk * p.B,
                                   x_full.begin() + (blk + 1) * p.B);
        auto y_blk = s.process_block(x_blk);
        for (std::size_t i = 0; i < p.B; ++i) {
            const double e = std::abs(y_blk[i] - y_expected[blk * p.B + i]);
            if (e > max_err) max_err = e;
        }
    }

    constexpr double tol = 1e-10;
    const bool ok = (max_err < tol);
    std::cout << "  [impulse]  max_err = " << max_err
              << (ok ? "  PASS" : "  FAIL") << "\n";
    return ok;
}

// ---------------------------------------------------------------------------
// TEST 2: random channel / random input vs. direct convolution.
// ---------------------------------------------------------------------------
static bool test_random_vs_direct()
{
    const auto p = up::params::from_N_B(SELFTEST_N, SELFTEST_B);

    std::mt19937 rng(0xC0FFEE);
    std::uniform_real_distribution<double> uni(-0.1, 0.1);

    std::vector<cdouble> h(p.N);
    for (auto& v : h) v = cdouble(uni(rng), uni(rng));

    std::vector<cdouble> x(SELFTEST_N_BLOCKS * p.B);
    for (auto& v : x) v = cdouble(uni(rng), uni(rng));

    auto H_parts = up::prep_h_parts(p, h);
    up::stream_state s(p, H_parts);

    std::vector<cdouble> y_ups;
    y_ups.reserve(x.size());
    for (std::size_t blk = 0; blk < SELFTEST_N_BLOCKS; ++blk) {
        std::vector<cdouble> x_blk(x.begin() + blk * p.B,
                                   x.begin() + (blk + 1) * p.B);
        auto y_blk = s.process_block(x_blk);
        y_ups.insert(y_ups.end(), y_blk.begin(), y_blk.end());
    }

    auto y_direct = up::direct_convolution(x, h, x.size());

    // Compare from sample (P-1)*B onward — the first (P-1) blocks are
    // the UPOLS transient region (FDL fill-up) per spec §3.3.  Before
    // that, UPOLS output is a partial convolution and will not match
    // direct conv exactly.
    const std::size_t skip = (p.P - 1) * p.B;
    double max_err = 0.0;
    for (std::size_t n = skip; n < y_ups.size(); ++n) {
        const double e = std::abs(y_ups[n] - y_direct[n]);
        if (e > max_err) max_err = e;
    }

    constexpr double tol = 1e-10;
    const bool ok = (max_err < tol);
    std::cout << "  [random ]  max_err = " << max_err
              << " (after " << skip << " startup samples)"
              << (ok ? "  PASS" : "  FAIL") << "\n";
    return ok;
}

// ---------------------------------------------------------------------------
// TEST 3: reset() should put the stream back in the pre-first-block state.
//   Process some blocks, reset, process the SAME blocks — outputs must
//   match byte-for-byte.
// ---------------------------------------------------------------------------
static bool test_reset()
{
    const auto p = up::params::from_N_B(SELFTEST_N, SELFTEST_B);

    std::mt19937 rng(0xBEEF);
    std::uniform_real_distribution<double> uni(-0.3, 0.3);

    std::vector<cdouble> h(p.N);
    for (auto& v : h) v = cdouble(uni(rng), uni(rng));

    std::vector<std::vector<cdouble>> blocks(SELFTEST_N_BLOCKS,
                                             std::vector<cdouble>(p.B));
    for (auto& b : blocks) for (auto& v : b) v = cdouble(uni(rng), uni(rng));

    auto H_parts = up::prep_h_parts(p, h);
    up::stream_state s(p, H_parts);

    std::vector<std::vector<cdouble>> y_first(SELFTEST_N_BLOCKS);
    for (std::size_t i = 0; i < SELFTEST_N_BLOCKS; ++i) {
        y_first[i] = s.process_block(blocks[i]);
    }

    s.reset();

    double max_err = 0.0;
    for (std::size_t i = 0; i < SELFTEST_N_BLOCKS; ++i) {
        auto y_new = s.process_block(blocks[i]);
        for (std::size_t j = 0; j < p.B; ++j) {
            const double e = std::abs(y_new[j] - y_first[i][j]);
            if (e > max_err) max_err = e;
        }
    }

    const bool ok = (max_err == 0.0);
    std::cout << "  [reset  ]  max_err = " << max_err
              << (ok ? "  PASS" : "  FAIL") << "\n";
    return ok;
}

// ---------------------------------------------------------------------------
// Generate binary test vectors for the SystemVerilog testbench.
// ---------------------------------------------------------------------------
static void dump_test_vectors(const std::string& dir)
{
    const auto p = up::params::from_N_B(SELFTEST_N, SELFTEST_B);

    std::mt19937 rng(0xDEADBEEF);
    std::uniform_real_distribution<double> uni(-0.5, 0.5);

    std::vector<cdouble> h(p.N);
    for (auto& v : h) v = cdouble(uni(rng), uni(rng));

    std::vector<cdouble> x(SELFTEST_N_BLOCKS * p.B);
    for (auto& v : x) v = cdouble(uni(rng), uni(rng));

    auto H_parts = up::prep_h_parts(p, h);
    up::stream_state s(p, H_parts);

    std::vector<cdouble> y;
    y.reserve(x.size());
    for (std::size_t blk = 0; blk < SELFTEST_N_BLOCKS; ++blk) {
        std::vector<cdouble> x_blk(x.begin() + blk * p.B,
                                   x.begin() + (blk + 1) * p.B);
        auto y_blk = s.process_block(x_blk);
        y.insert(y.end(), y_blk.begin(), y_blk.end());
    }

    // Input samples in sc16 (I high, Q low)
    std::vector<uint32_t> x_words(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        x_words[i] = pack_sc16(quantize_q15(x[i].real()),
                               quantize_q15(x[i].imag()));
    }

    // H_parts in flat partition-major layout, same packing as H_DATA
    std::vector<uint32_t> h_words(p.P * p.K);
    for (std::size_t pp = 0; pp < p.P; ++pp) {
        for (std::size_t k = 0; k < p.K; ++k) {
            h_words[pp * p.K + k] = pack_h(H_parts[pp][k]);
        }
    }

    // Expected output
    std::vector<uint32_t> y_words(y.size());
    for (std::size_t i = 0; i < y.size(); ++i) {
        y_words[i] = pack_sc16(quantize_q15(y[i].real()),
                               quantize_q15(y[i].imag()));
    }

    write_u32_file(dir + "/x_in.sc16", x_words);
    write_u32_file(dir + "/h_parts.sc16", h_words);
    write_u32_file(dir + "/y_expected.sc16", y_words);

    std::cout << "  dumped " << x_words.size() << " input samples, "
              << h_words.size() << " H_parts words, "
              << y_words.size() << " expected output samples into "
              << dir << "\n";
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    bool do_selftest = true;
    std::string dump_dir;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--selftest") {
            do_selftest = true;
        } else if (a == "--dump") {
            if (i + 1 >= argc) {
                std::cerr << "--dump requires a directory argument\n";
                return 2;
            }
            dump_dir = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--selftest] [--dump <dir>]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            return 2;
        }
    }

    bool all_ok = true;

    if (do_selftest) {
        std::cout << "UPOLS golden-model self-test"
                  << " (N=" << SELFTEST_N
                  << " B=" << SELFTEST_B
                  << " P=" << SELFTEST_P
                  << " K=" << SELFTEST_K
                  << " blocks=" << SELFTEST_N_BLOCKS << "):\n";
        all_ok &= test_impulse();
        all_ok &= test_random_vs_direct();
        all_ok &= test_reset();
        std::cout << (all_ok ? "SELFTEST PASSED\n" : "SELFTEST FAILED\n");
    }

    if (!dump_dir.empty()) {
        dump_test_vectors(dump_dir);
    }

    return all_ok ? 0 : 1;
}
