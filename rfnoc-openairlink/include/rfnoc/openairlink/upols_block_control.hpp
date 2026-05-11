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

#ifndef INCLUDED_RFNOC_OPENAIRLINK_UPOLS_BLOCK_CONTROL_HPP
#define INCLUDED_RFNOC_OPENAIRLINK_UPOLS_BLOCK_CONTROL_HPP

#include <uhd/config.hpp>
#include <uhd/rfnoc/noc_block_base.hpp>
#include <complex>
#include <cstdint>
#include <vector>

namespace rfnoc { namespace openairlink {

/*! Block controller for the UPOLS (Uniformly-Partitioned Overlap-Save) FIR block.
 *
 *  Performs block-based frequency-domain convolution on a streaming sc16
 *  channel.  The filter length N, block size B, FFT size K = 2*B, and the
 *  partition count P = ceil(N/B) are compile-time parameters of the HDL
 *  and read back from the FPGA at construction.
 *
 *  The host is responsible for:
 *   1. Zero-padding the channel impulse response h to P*B taps
 *   2. Partitioning into P chunks of length B each
 *   3. Zero-padding each chunk to K, computing its K-point FFT
 *   4. Scaling the resulting spectra by 1/K (so the FPGA IFFT needs no
 *      post-scaling)
 *   5. Quantizing to signed Q1.15 and uploading via ctrlport
 *
 *  Steps 1-5 are encapsulated in set_channel_dense() and
 *  set_channel_sparse().
 *
 *  Register Map (see rfnoc_upols_regs.vh for authoritative source):
 *    0x00 COMPAT_NUM   (R)   {major[31:16], minor[15:0]}
 *    0x04 N            (R)   filter length
 *    0x08 B            (R)   block length
 *    0x0C K            (R)   FFT size
 *    0x10 P            (R)   partition count
 *    0x14 CTRL         (R/W) [0]=soft_reset (self-clearing)
 *    0x18 STATUS       (R)   [0]=h_loaded [1]=overflow [2]=underflow
 *    0x20 H_ADDR       (R/W) write index into H_parts BRAM
 *    0x24 H_DATA       (R/W) packed {im[15:0], re[15:0]} Q1.15,
 *                            auto-increments H_ADDR
 *
 *  H_parts layout (flat index = p*K + k, partition-major):
 *    H_parts[p][k] = (1/K) * DFT_K(h_padded[p*B .. (p+1)*B - 1] || zeros(B))[k]
 */
class UHD_API upols_block_control : public uhd::rfnoc::noc_block_base
{
public:
    RFNOC_DECLARE_BLOCK(upols_block_control)

    // Register addresses (must match rfnoc_upols_regs.vh)
    static const uint32_t REG_COMPAT_NUM;
    static const uint32_t REG_N;
    static const uint32_t REG_B;
    static const uint32_t REG_K;
    static const uint32_t REG_P;
    static const uint32_t REG_CTRL;
    static const uint32_t REG_STATUS;
    static const uint32_t REG_H_ADDR;
    static const uint32_t REG_H_DATA;

    // Bit positions within CTRL / STATUS
    static const uint32_t CTRL_SOFT_RESET_MASK;
    static const uint32_t STATUS_H_LOADED_MASK;
    static const uint32_t STATUS_OVERFLOW_MASK;
    static const uint32_t STATUS_UNDERFLOW_MASK;

    // -----------------------------------------------------------------
    // Compile-time parameter readback
    // -----------------------------------------------------------------
    /*! Number of filter taps supported by this block. */
    virtual uint32_t get_N() = 0;
    /*! Block length (number of input samples per FFT frame). */
    virtual uint32_t get_B() = 0;
    /*! FFT size (equal to 2*B). */
    virtual uint32_t get_K() = 0;
    /*! Number of sub-filter partitions (equal to ceil(N/B)). */
    virtual uint32_t get_P() = 0;

    // -----------------------------------------------------------------
    // Control / status
    // -----------------------------------------------------------------
    /*! Pulse the soft reset, clearing FDL, input_buf, and write_ptr.
     *  Also clears the h_loaded, overflow, and underflow status bits.
     *
     *  Should be called after any input discontinuity (e.g. RX overflow).
     */
    virtual void soft_reset() = 0;

    /*! True once a full H_parts upload (P*K words) has completed since the
     *  last soft reset.  The FPGA will produce a correct output only after
     *  this is true AND at least P-1 input blocks have been processed.
     */
    virtual bool get_h_loaded() = 0;

    /*! Sticky overflow flag — set if a block did not receive B samples.
     *  Cleared on soft_reset().  Treat as a hint to investigate upstream
     *  backpressure; the FPGA does not auto-resync.
     */
    virtual bool get_overflow() = 0;

    /*! Sticky underflow flag — set if downstream backpressure stalls a
     *  block output.  Cleared on soft_reset().
     */
    virtual bool get_underflow() = 0;

    // -----------------------------------------------------------------
    // Channel loading
    // -----------------------------------------------------------------
    /*! Upload a dense-format impulse response.
     *
     *  \param h  Channel impulse response, length <= N.  Shorter vectors
     *            are zero-padded on the right.  Values should lie in
     *            [-1, 1) (Q1.15 range); out-of-range values are saturated
     *            after the 1/K scaling.
     *
     *  Performs zero-padding, per-partition K-point FFT, scaling by 1/K,
     *  Q1.15 quantization, and ctrlport upload.  Blocks until upload
     *  completes.  Does not issue a soft reset — caller should decide
     *  whether glitches during the upload are acceptable.
     */
    virtual void set_channel_dense(
        const std::vector<std::complex<double>>& h) = 0;

    /*! Upload a sparse-format impulse response (CSV-style delay:coeff pairs).
     *
     *  \param delays  Vector of delays in samples; entries >= N are ignored.
     *  \param coeffs  Vector of complex coefficients, one per delay.  Must
     *                 have the same size as \p delays.
     *
     *  Expands to a dense length-N impulse response (summing contributions
     *  at duplicate delays), then forwards to set_channel_dense().
     */
    virtual void set_channel_sparse(
        const std::vector<uint32_t>& delays,
        const std::vector<std::complex<double>>& coeffs) = 0;
};

}} // namespace rfnoc::openairlink

#endif /* INCLUDED_RFNOC_OPENAIRLINK_UPOLS_BLOCK_CONTROL_HPP */
