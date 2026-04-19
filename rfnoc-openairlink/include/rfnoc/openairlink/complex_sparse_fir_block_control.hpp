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

#ifndef INCLUDED_RFNOC_OPENAIRLINK_SPARSE_FIR_BLOCK_CONTROL_HPP
#define INCLUDED_RFNOC_OPENAIRLINK_SPARSE_FIR_BLOCK_CONTROL_HPP

#include <uhd/config.hpp>
#include <uhd/rfnoc/noc_block_base.hpp>
#include <cstdint>
#include <vector>

namespace rfnoc { namespace openairlink {

/*! Block controller for the sparse FIR filter block.
 *
 * This block implements a sparse FIR filter with NUM_TAPS independently
 * addressable taps, each with a programmable delay (in samples) and
 * complex coefficient. This enables large delay-spread channel emulation
 * (e.g. 5 us at 200 MHz) using only NUM_TAPS DSP48 slices.
 *
 * Register Map:
 *   0x00: COMPAT_NUM   (R)   - {major[31:16], minor[15:0]}
 *   0x04: NUM_TAPS     (R)   - Number of sparse taps
 *   0x08: MAX_DELAY    (R)   - Maximum delay depth in samples
 *   0x10+i*8: TAP_DELAY(R/W) - Delay for tap i (samples, 0..MAX_DELAY-1)
 *   0x14+i*8: TAP_COEFF(R/W) - Complex coeff: {im[31:16], re[15:0]}
 *
 * COMPAT_MAJOR >= 2 indicates complex coefficient support. Old host code
 * writing only 16-bit real coefficients is backward-compatible (im=0).
 */
class UHD_API sparse_fir_block_control : public uhd::rfnoc::noc_block_base
{
public:
    RFNOC_DECLARE_BLOCK(sparse_fir_block_control)

    // Register addresses (must match rfnoc_sparse_fir_regs.vh)
    static const uint32_t REG_COMPAT_NUM;
    static const uint32_t REG_NUM_TAPS;
    static const uint32_t REG_MAX_DELAY;
    static const uint32_t REG_TAP_BASE;
    static const uint32_t REG_TAP_STRIDE;

    /*! Get the number of taps supported by this block (compile-time constant).
     */
    virtual uint32_t get_num_taps() = 0;

    /*! Get the maximum delay depth in samples (compile-time constant).
     */
    virtual uint32_t get_max_delay() = 0;

    /*! Set the delay (in samples) for a specific tap.
     *
     * \param tap_index  Which tap to configure (0..NUM_TAPS-1)
     * \param delay      Delay in samples (0..MAX_DELAY-1)
     */
    virtual void set_tap_delay(uint32_t tap_index, uint32_t delay) = 0;

    /*! Get the current delay (in samples) for a specific tap.
     */
    virtual uint32_t get_tap_delay(uint32_t tap_index) = 0;

    /*! Set the real coefficient for a specific tap (imaginary set to 0).
     *
     * \param tap_index  Which tap to configure (0..NUM_TAPS-1)
     * \param coeff      Signed 16-bit coefficient (real part only)
     */
    virtual void set_tap_coeff(uint32_t tap_index, int16_t coeff) = 0;

    /*! Get the current real coefficient for a specific tap.
     */
    virtual int16_t get_tap_coeff(uint32_t tap_index) = 0;

    /*! Set complex coefficient for a specific tap.
     *
     * \param tap_index  Which tap to configure (0..NUM_TAPS-1)
     * \param coeff_re   Real part (signed 16-bit, Q1.15)
     * \param coeff_im   Imaginary part (signed 16-bit, Q1.15)
     */
    virtual void set_tap_coeff_complex(uint32_t tap_index, int16_t coeff_re, int16_t coeff_im) = 0;

    /*! Get the current complex coefficient for a specific tap.
     *
     * \return pair of {coeff_re, coeff_im}
     */
    virtual std::pair<int16_t, int16_t> get_tap_coeff_complex(uint32_t tap_index) = 0;

    /*! Configure a tap's delay and real coefficient in one call.
     *
     * \param tap_index  Which tap (0..NUM_TAPS-1)
     * \param delay      Delay in samples
     * \param coeff      Signed 16-bit coefficient (real only, im=0)
     */
    virtual void set_tap(uint32_t tap_index, uint32_t delay, int16_t coeff) = 0;

    /*! Configure a tap's delay and complex coefficient in one call.
     *
     * \param tap_index  Which tap (0..NUM_TAPS-1)
     * \param delay      Delay in samples
     * \param coeff_re   Real part of coefficient
     * \param coeff_im   Imaginary part of coefficient
     */
    virtual void set_tap_complex(uint32_t tap_index, uint32_t delay,
                                 int16_t coeff_re, int16_t coeff_im) = 0;

    /*! Set all taps at once from vectors (real coefficients only, im=0).
     *
     * All vectors must have exactly NUM_TAPS elements.
     *
     * \param delays  Vector of delays (one per tap)
     * \param coeffs  Vector of real coefficients (one per tap)
     */
    virtual void set_all_taps(
        const std::vector<uint32_t>& delays,
        const std::vector<int16_t>& coeffs) = 0;

    /*! Set all taps at once with complex coefficients.
     *
     * All vectors must have exactly NUM_TAPS elements.
     *
     * \param delays    Vector of delays (one per tap)
     * \param coeffs_re Vector of real coefficient parts
     * \param coeffs_im Vector of imaginary coefficient parts
     */
    virtual void set_all_taps_complex(
        const std::vector<uint32_t>& delays,
        const std::vector<int16_t>& coeffs_re,
        const std::vector<int16_t>& coeffs_im) = 0;
};

}} // namespace rfnoc::openairlink

#endif /* INCLUDED_RFNOC_OPENAIRLINK_SPARSE_FIR_BLOCK_CONTROL_HPP */
