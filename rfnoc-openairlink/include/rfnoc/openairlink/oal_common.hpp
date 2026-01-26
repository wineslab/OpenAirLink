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

/**
 * OpenAirLink Common Functions
 * 
 * Shared RFNoC/UHD functionality for the 4-channel bidirectional emulator.
 * Used by both oal_4chan.cpp (CSV interface) and oal_4chan_zmq.cpp (ZMQ/MT interface).
 */

#ifndef INCLUDED_RFNOC_OPENAIRLINK_OAL_COMMON_HPP
#define INCLUDED_RFNOC_OPENAIRLINK_OAL_COMMON_HPP

#include <uhd/rfnoc/block_id.hpp>
#include <uhd/rfnoc/mb_controller.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/rfnoc/fir_filter_block_control.hpp>
#include <uhd/rfnoc_graph.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/utils/graph_utils.hpp>
#include <uhd/utils/math.hpp>
#include <rfnoc/openairlink/shiftright_block_control.hpp>
#include <boost/format.hpp>
#include <vector>
#include <array>
#include <string>
#include <iostream>

namespace rfnoc { namespace openairlink {

//=============================================================================
// Constants
//=============================================================================

constexpr size_t NUM_DL_CHANNELS = 3;
constexpr size_t NUM_UL_CHANNELS = 3;
constexpr size_t NUM_TOTAL_CHANNELS = NUM_DL_CHANNELS + NUM_UL_CHANNELS;
constexpr size_t NUM_FIR_TAPS = 41;

//=============================================================================
// Block IDs
//=============================================================================

struct BlockIds {
    // Radio block IDs
    std::string radio0 = "0/Radio#0";   // gNB (port 0) + UE1 (port 1)
    std::string radio1 = "0/Radio#1";   // UE2 (port 0) + UE3 (port 1)
    
    // FIR block IDs
    std::string fir_dl[NUM_DL_CHANNELS] = {"0/FIR#0", "0/FIR#1", "0/FIR#2"};
    std::string fir_ul[NUM_UL_CHANNELS] = {"0/FIR#3", "0/FIR#4", "0/FIR#5"};
    
    // Shiftright block IDs
    std::string shift_dl[NUM_DL_CHANNELS] = {"0/Shiftright#0", "0/Shiftright#1", "0/Shiftright#2"};
    std::string shift_ul[NUM_UL_CHANNELS] = {"0/Shiftright#3", "0/Shiftright#4", "0/Shiftright#5"};
};

//=============================================================================
// Emulator Context - holds all block controls
//=============================================================================

struct EmulatorContext {
    // RFNoC graph
    uhd::rfnoc::rfnoc_graph::sptr graph;
    size_t mb_idx;
    
    // Radio controls
    uhd::rfnoc::radio_control::sptr radio0;
    uhd::rfnoc::radio_control::sptr radio1;
    
    // FIR filter controls
    std::array<uhd::rfnoc::fir_filter_block_control::sptr, NUM_DL_CHANNELS> fir_dl;
    std::array<uhd::rfnoc::fir_filter_block_control::sptr, NUM_UL_CHANNELS> fir_ul;
    
    // Shiftright block controls
    std::array<shiftright_block_control::sptr, NUM_DL_CHANNELS> shift_dl;
    std::array<shiftright_block_control::sptr, NUM_UL_CHANNELS> shift_ul;
    
    // Default FIR coefficients (passthrough)
    std::vector<int16_t> default_fir;
    
    // Constructor
    EmulatorContext() : default_fir(NUM_FIR_TAPS, 0) {
        default_fir[0] = 32767;  // Passthrough
    }
};

//=============================================================================
// RF Configuration
//=============================================================================

struct RFConfig {
    double gnb_freq = 3619.2e6;    // gNB center frequency (Hz)
    double ue_freq = 3619.2e6;     // UE center frequency (Hz)
    double rx_bw = 100e6;          // RX bandwidth (Hz)
    double tx_bw = 100e6;          // TX bandwidth (Hz)
    
    // Per-port gains (gNB, UE1, UE2, UE3)
    std::array<double, 4> rx_gains = {0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> tx_gains = {0.0, 0.0, 0.0, 0.0};
    
    size_t spp = 32;               // Samples per packet
    bool rx_timestamps = false;    // Enable RX timestamps
    double setup_time = 0.1;       // Setup time (seconds)
};

//=============================================================================
// Function Declarations
//=============================================================================

/**
 * Initialize the RFNoC graph and acquire all block controls
 * @param args UHD device arguments
 * @param ctx Emulator context to populate
 * @param block_ids Block IDs to use
 * @return true if successful, false otherwise
 */
bool init_rfnoc_graph(const std::string& args, EmulatorContext& ctx, const BlockIds& block_ids = BlockIds());

/**
 * Configure RF parameters on all radios
 * @param ctx Emulator context
 * @param config RF configuration
 */
void configure_rf_params(EmulatorContext& ctx, const RFConfig& config);

/**
 * Initialize FIR filters with default passthrough coefficients
 * @param ctx Emulator context
 */
void init_fir_filters(EmulatorContext& ctx);

/**
 * Initialize shiftright blocks with no attenuation (shift=0)
 * @param ctx Emulator context
 */
void init_shiftright_blocks(EmulatorContext& ctx);

/**
 * Start continuous streaming on all radios
 * @param ctx Emulator context
 * @param setup_time Time offset for stream start
 */
void start_streaming(EmulatorContext& ctx, double setup_time);

/**
 * Stop streaming on all radios
 * @param ctx Emulator context
 */
void stop_streaming(EmulatorContext& ctx);

/**
 * Print current channel status
 * @param ctx Emulator context
 */
void print_channel_status(const EmulatorContext& ctx);

/**
 * Set FIR coefficients for a specific channel
 * @param ctx Emulator context
 * @param channel_index Channel index (0-2 for DL, 3-5 for UL)
 * @param coeffs FIR coefficients
 */
void set_fir_coefficients(EmulatorContext& ctx, size_t channel_index, const std::vector<int16_t>& coeffs);

/**
 * Set shiftright value for a specific channel
 * @param ctx Emulator context
 * @param channel_index Channel index (0-2 for DL, 3-5 for UL)
 * @param shift Shiftright value
 */
void set_shiftright_value(EmulatorContext& ctx, size_t channel_index, uint32_t shift);

/**
 * Parse comma-separated gain values
 * @param input Comma-separated string (e.g., "0,10,15,20")
 * @param expected_count Expected number of values
 * @param default_val Default value for missing entries
 * @return Vector of gain values
 */
std::vector<double> parse_gains(const std::string& input, size_t expected_count, double default_val);

/**
 * Parse space-separated FIR coefficients from string
 * @param input Space-separated string of coefficients
 * @return Vector of int16_t coefficients
 */
std::vector<int16_t> parse_fir_coeffs(const std::string& input);

/**
 * Trim whitespace from string
 * @param str Input string
 * @return Trimmed string
 */
std::string trim_whitespace(const std::string& str);

}} // namespace rfnoc::openairlink

#endif /* INCLUDED_RFNOC_OPENAIRLINK_OAL_COMMON_HPP */

