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

#include <rfnoc/openairlink/complex_sparse_fir_block_control.hpp>

#include <uhd/rfnoc/defaults.hpp>
#include <uhd/rfnoc/registry.hpp>
#include <uhd/exception.hpp>
#include <uhd/utils/log.hpp>

using namespace rfnoc::openairlink;
using namespace uhd::rfnoc;

// Register addresses (must match rfnoc_complex_sparse_fir_regs.vh)
const uint32_t complex_sparse_fir_block_control::REG_COMPAT_NUM = 0x00;
const uint32_t complex_sparse_fir_block_control::REG_NUM_TAPS   = 0x04;
const uint32_t complex_sparse_fir_block_control::REG_MAX_DELAY  = 0x08;
const uint32_t complex_sparse_fir_block_control::REG_TAP_BASE   = 0x10;
const uint32_t complex_sparse_fir_block_control::REG_TAP_STRIDE = 0x08;

class complex_sparse_fir_block_control_impl : public complex_sparse_fir_block_control
{
public:
    RFNOC_BLOCK_CONSTRUCTOR(complex_sparse_fir_block_control)
    {
        // Cache compile-time constants from FPGA
        _num_taps  = regs().peek32(REG_NUM_TAPS);
        _max_delay = regs().peek32(REG_MAX_DELAY);

        // Expose tap count and max delay in the UHD property tree so they
        // appear under this block's path in `uhd_usrp_probe --tree`.
        auto tree_root = get_block_id().get_tree_root();
        get_tree()->create<uint32_t>(tree_root / "num_taps").set(_num_taps);
        get_tree()->create<uint32_t>(tree_root / "max_delay").set(_max_delay);

        UHD_LOGGER_INFO("ComplexSparseFIR")
            << get_unique_id() << ": Complex Sparse FIR Filter with "
            << _num_taps << " taps (max delay " << _max_delay << " samples)";
    }

    uint32_t get_num_taps() override
    {
        return _num_taps;
    }

    uint32_t get_max_delay() override
    {
        return _max_delay;
    }

    void set_tap_delay(uint32_t tap_index, uint32_t delay) override
    {
        _check_tap_index(tap_index);
        _check_delay(delay);
        regs().poke32(REG_TAP_BASE + tap_index * REG_TAP_STRIDE + 0x00, delay);
    }

    uint32_t get_tap_delay(uint32_t tap_index) override
    {
        _check_tap_index(tap_index);
        return regs().peek32(REG_TAP_BASE + tap_index * REG_TAP_STRIDE + 0x00);
    }

    void set_tap_coeff(uint32_t tap_index, int16_t coeff) override
    {
        // Real-only: set coeff_re = coeff, coeff_im = 0
        set_tap_coeff_complex(tap_index, coeff, 0);
    }

    int16_t get_tap_coeff(uint32_t tap_index) override
    {
        return get_tap_coeff_complex(tap_index).first;
    }

    void set_tap_coeff_complex(uint32_t tap_index, int16_t coeff_re, int16_t coeff_im) override
    {
        _check_tap_index(tap_index);
        // Pack {coeff_im[31:16], coeff_re[15:0]} into one 32-bit register
        uint32_t packed = (static_cast<uint32_t>(static_cast<uint16_t>(coeff_im)) << 16)
                        | static_cast<uint32_t>(static_cast<uint16_t>(coeff_re));
        regs().poke32(REG_TAP_BASE + tap_index * REG_TAP_STRIDE + 0x04, packed);
    }

    std::pair<int16_t, int16_t> get_tap_coeff_complex(uint32_t tap_index) override
    {
        _check_tap_index(tap_index);
        uint32_t raw = regs().peek32(REG_TAP_BASE + tap_index * REG_TAP_STRIDE + 0x04);
        int16_t coeff_re = static_cast<int16_t>(raw & 0xFFFF);
        int16_t coeff_im = static_cast<int16_t>((raw >> 16) & 0xFFFF);
        return {coeff_re, coeff_im};
    }

    void set_tap(uint32_t tap_index, uint32_t delay, int16_t coeff) override
    {
        set_tap_delay(tap_index, delay);
        set_tap_coeff(tap_index, coeff);
    }

    void set_tap_complex(uint32_t tap_index, uint32_t delay,
                         int16_t coeff_re, int16_t coeff_im) override
    {
        set_tap_delay(tap_index, delay);
        set_tap_coeff_complex(tap_index, coeff_re, coeff_im);
    }

    void set_all_taps(
        const std::vector<uint32_t>& delays,
        const std::vector<int16_t>& coeffs) override
    {
        if (delays.size() != _num_taps || coeffs.size() != _num_taps) {
            throw uhd::value_error(
                "set_all_taps: vectors must have exactly " +
                std::to_string(_num_taps) + " elements");
        }
        for (uint32_t i = 0; i < _num_taps; i++) {
            set_tap(i, delays[i], coeffs[i]);
        }
    }

    void set_all_taps_complex(
        const std::vector<uint32_t>& delays,
        const std::vector<int16_t>& coeffs_re,
        const std::vector<int16_t>& coeffs_im) override
    {
        if (delays.size() != _num_taps || coeffs_re.size() != _num_taps
            || coeffs_im.size() != _num_taps) {
            throw uhd::value_error(
                "set_all_taps_complex: vectors must have exactly " +
                std::to_string(_num_taps) + " elements");
        }
        for (uint32_t i = 0; i < _num_taps; i++) {
            set_tap_complex(i, delays[i], coeffs_re[i], coeffs_im[i]);
        }
    }

private:
    uint32_t _num_taps;
    uint32_t _max_delay;

    void _check_tap_index(uint32_t idx)
    {
        if (idx >= _num_taps) {
            throw uhd::value_error(
                "Tap index " + std::to_string(idx) +
                " out of range (0.." + std::to_string(_num_taps - 1) + ")");
        }
    }

    void _check_delay(uint32_t delay)
    {
        if (delay >= _max_delay) {
            throw uhd::value_error(
                "Delay " + std::to_string(delay) +
                " out of range (0.." + std::to_string(_max_delay - 1) + ")");
        }
    }
};

// NOC_ID must match the one in noc_shell_complex_sparse_fir.v (0x5F1A0004)
UHD_RFNOC_BLOCK_REGISTER_DIRECT(
    complex_sparse_fir_block_control, 0x5F1A0004, "ComplexSparseFIR", CLOCK_KEY_GRAPH, "bus_clk")
