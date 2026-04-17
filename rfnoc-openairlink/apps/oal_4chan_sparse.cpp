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
 * OpenAirLink 4-Channel Bidirectional Channel Emulator (Sparse FIR version)
 * 
 * Uses the sparse FIR filter block instead of the dense FIR filter.
 * Each sparse FIR block has NUM_TAPS (default 4) independently addressable
 * taps with programmable delays (0..MAX_DELAY-1 samples) and coefficients.
 *
 * This enables channel emulation over large delay spreads (e.g., 5+ us)
 * with minimal FPGA resources.
 *
 * Topology (1 gNB + 3 UEs):
 *   gNB  <---> radio0 port 0
 *   UE1  <---> radio0 port 1
 *   UE2  <---> radio1 port 0
 *   UE3  <---> radio1 port 1
 *
 * Signal Flow:
 *   DOWNLINK: radio0_RX0 -> split -> SparseFIR_DL -> shift_DL -> UE radios TX
 *   UPLINK:   UE radios RX -> SparseFIR_UL -> shift_UL -> ADDER -> radio0_TX0
 *
 * CSV Channel Config Format (per channel):
 *   delay0:coeff0 delay1:coeff1 ..., shift_value
 *
 * Coefficients can be real or complex:
 *   Real:    delay:coeff        (e.g. 0:32767)
 *   Complex: delay:re+imj       (e.g. 100:23170+23170j, 200:8000-4000j)
 *
 * Example CSV line (6 channels: DL0,DL1,DL2,UL0,UL1,UL2):
 *   0:23170+23170j 100:8000 0:0 0:0, 6, 0:32767 0:0 0:0 0:0, 6, ...
 */

#include <uhd/rfnoc/block_id.hpp>
#include <uhd/rfnoc/mb_controller.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/rfnoc_graph.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/utils/graph_utils.hpp>
#include <uhd/utils/math.hpp>
#include <uhd/utils/safe_main.hpp>
#include <rfnoc/openairlink/sparse_fir_block_control.hpp>
#include <rfnoc/openairlink/shiftright_block_control.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <array>

namespace po = boost::program_options;
using uhd::rfnoc::radio_control;
using rfnoc::openairlink::sparse_fir_block_control;
using rfnoc::openairlink::shiftright_block_control;
using namespace std::chrono_literals;

constexpr size_t NUM_DL_CHANNELS = 3;
constexpr size_t NUM_UL_CHANNELS = 3;
constexpr size_t NUM_TOTAL_CHANNELS = NUM_DL_CHANNELS + NUM_UL_CHANNELS;

/****************************************************************************
 * SIGINT handling
 ***************************************************************************/
static bool stop_signal_called = false;
void sig_int_handler(int)
{
    stop_signal_called = true;
}

/****************************************************************************
 * Utility: trim whitespace
 ***************************************************************************/
std::string space_trim(const std::string& str) {
    std::string out = str;
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    out.erase(std::find_if(out.rbegin(), out.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), out.end());
    return out;
}

/****************************************************************************
 * Parse sparse FIR tap config string (supports complex coefficients)
 *
 * Formats:
 *   "delay0:coeff0 delay1:coeff1 ..."              (real-only, backward compatible)
 *   "delay0:re0+im0j delay1:re1-im1j ..."          (complex coefficients)
 *   "delay0:re0+im0j delay1:coeff1 ..."             (mixed: some real, some complex)
 *
 * Examples:
 *   "0:32767 100:8000"                              -> h[0]=32767+0j, h[1]=8000+0j
 *   "0:23170+23170j 100:8000-4000j"                 -> h[0]=23170+23170j, h[1]=8000-4000j
 *
 * Returns tuple of vectors: {delays, coeffs_re, coeffs_im}
 ***************************************************************************/
struct sparse_tap_config {
    std::vector<uint32_t> delays;
    std::vector<int16_t> coeffs_re;
    std::vector<int16_t> coeffs_im;
};

sparse_tap_config parse_sparse_taps(const std::string& input, size_t num_taps)
{
    sparse_tap_config cfg;

    std::istringstream iss(space_trim(input));
    std::string token;

    while (iss >> token) {
        uint32_t delay;
        int16_t coeff_re = 0, coeff_im = 0;

        size_t colon_pos = token.find(':');
        if (colon_pos == std::string::npos) {
            // No colon: treat as real coefficient at delay=current_index
            delay = static_cast<uint32_t>(cfg.delays.size());
            coeff_re = static_cast<int16_t>(std::stoi(token));
        } else {
            delay = static_cast<uint32_t>(std::stoul(token.substr(0, colon_pos)));
            std::string coeff_str = token.substr(colon_pos + 1);

            // Check for complex notation: re+imj or re-imj
            size_t j_pos = coeff_str.find('j');
            if (j_pos != std::string::npos) {
                // Remove trailing 'j'
                std::string no_j = coeff_str.substr(0, j_pos);
                // Find the +/- separator (skip leading minus for negative re)
                size_t sep = std::string::npos;
                for (size_t i = 1; i < no_j.size(); i++) {
                    if (no_j[i] == '+' || no_j[i] == '-') {
                        sep = i;
                        break;
                    }
                }
                if (sep != std::string::npos) {
                    coeff_re = static_cast<int16_t>(std::stoi(no_j.substr(0, sep)));
                    coeff_im = static_cast<int16_t>(std::stoi(no_j.substr(sep)));
                } else {
                    // Pure imaginary: e.g. "100:5000j"
                    coeff_re = 0;
                    coeff_im = static_cast<int16_t>(std::stoi(no_j));
                }
            } else {
                // Real-only coefficient
                coeff_re = static_cast<int16_t>(std::stoi(coeff_str));
            }
        }

        cfg.delays.push_back(delay);
        cfg.coeffs_re.push_back(coeff_re);
        cfg.coeffs_im.push_back(coeff_im);
    }

    // Pad to num_taps with zeros
    while (cfg.delays.size() < num_taps) {
        cfg.delays.push_back(0);
        cfg.coeffs_re.push_back(0);
        cfg.coeffs_im.push_back(0);
    }

    return cfg;
}

/****************************************************************************
 * Doppler tap specification: per-tap delay, amplitude, Doppler shift, phase
 ***************************************************************************/
struct doppler_tap {
    uint32_t delay;
    double   amplitude;   // [0, 1]
    double   fd_hz;       // Doppler shift in Hz
    double   phi0;        // initial phase in radians
};

struct doppler_channel {
    std::vector<doppler_tap> taps;
    uint32_t shift_value = 0;
};

/****************************************************************************
 * Compute complex tap coefficients at time t for a Doppler channel
 ***************************************************************************/
sparse_tap_config compute_doppler_taps(const doppler_channel& ch, double t, size_t num_taps)
{
    sparse_tap_config cfg;
    for (const auto& tap : ch.taps) {
        double phase = 2.0 * M_PI * tap.fd_hz * t + tap.phi0;
        double re = tap.amplitude * std::cos(phase);
        double im = tap.amplitude * std::sin(phase);

        auto clamp16 = [](double v) -> int16_t {
            double scaled = v * 32767.0;
            if (scaled > 32767.0) scaled = 32767.0;
            if (scaled < -32768.0) scaled = -32768.0;
            return static_cast<int16_t>(std::round(scaled));
        };

        cfg.delays.push_back(tap.delay);
        cfg.coeffs_re.push_back(clamp16(re));
        cfg.coeffs_im.push_back(clamp16(im));
    }
    while (cfg.delays.size() < num_taps) {
        cfg.delays.push_back(0);
        cfg.coeffs_re.push_back(0);
        cfg.coeffs_im.push_back(0);
    }
    return cfg;
}

/****************************************************************************
 * Parse a Doppler channel spec string.
 * Format: "delay:amp:fd:phi0 delay:amp:fd:phi0 ..."
 *   delay  -- delay in samples
 *   amp    -- amplitude [0,1]
 *   fd     -- Doppler frequency in Hz
 *   phi0   -- initial phase in degrees
 *
 * Example: "0:0.9:194:0 20:0.5:-120:60 80:0.3:50:180"
 ***************************************************************************/
doppler_channel parse_doppler_channel(const std::string& input)
{
    doppler_channel ch;
    std::istringstream iss(space_trim(input));
    std::string token;

    while (iss >> token) {
        doppler_tap tap;
        std::vector<std::string> parts;
        std::istringstream ts(token);
        std::string part;
        while (std::getline(ts, part, ':')) {
            parts.push_back(part);
        }
        if (parts.size() < 4) {
            throw std::runtime_error(
                "Doppler tap requires 4 fields (delay:amp:fd:phi0), got: " + token);
        }
        tap.delay     = static_cast<uint32_t>(std::stoul(parts[0]));
        tap.amplitude = std::stod(parts[1]);
        tap.fd_hz     = std::stod(parts[2]);
        tap.phi0      = std::stod(parts[3]) * M_PI / 180.0;
        ch.taps.push_back(tap);
    }
    return ch;
}

/****************************************************************************
 * Pre-loaded script row: all 6 channels parsed into memory
 ***************************************************************************/
struct script_row {
    double time_index;
    std::array<sparse_tap_config, NUM_TOTAL_CHANNELS> taps;
    std::array<uint32_t, NUM_TOTAL_CHANNELS> shifts;
};

/****************************************************************************
 * Pre-load entire script CSV into memory for fast playback
 ***************************************************************************/
std::vector<script_row> preload_script_csv(const std::string& path, size_t num_taps)
{
    std::vector<script_row> rows;
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open script CSV: " + path);
    }

    std::string index_str;
    while (std::getline(f, index_str, ',')) {
        index_str = space_trim(index_str);
        if (index_str == "eos" || index_str.empty()) break;

        script_row row;
        row.time_index = std::stod(index_str);

        std::string taps_str, shift_str;

        for (size_t i = 0; i < NUM_TOTAL_CHANNELS - 1; i++) {
            std::getline(f, taps_str, ',');
            std::getline(f, shift_str, ',');
            row.taps[i] = parse_sparse_taps(taps_str, num_taps);
            row.shifts[i] = static_cast<uint32_t>(std::stoi(space_trim(shift_str)));
        }
        std::getline(f, taps_str, ',');
        std::getline(f, shift_str);
        row.taps[NUM_TOTAL_CHANNELS - 1] = parse_sparse_taps(taps_str, num_taps);
        row.shifts[NUM_TOTAL_CHANNELS - 1] =
            static_cast<uint32_t>(std::stoi(space_trim(shift_str)));

        rows.push_back(std::move(row));
    }
    return rows;
}

/****************************************************************************
 * Parse comma-separated gains
 ***************************************************************************/
std::vector<double> parse_gains(const std::string& input, size_t expected_count, double default_val)
{
    std::vector<double> gains;
    if (input.empty()) {
        gains.assign(expected_count, default_val);
        return gains;
    }

    std::istringstream iss(input);
    std::string token;
    while (std::getline(iss, token, ',')) {
        gains.push_back(std::stod(space_trim(token)));
    }

    while (gains.size() < expected_count) {
        gains.push_back(default_val);
    }

    return gains;
}

/****************************************************************************
 * Check CSV validity
 ***************************************************************************/
bool is_csv_valid(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

/****************************************************************************
 * Print channel status
 ***************************************************************************/
void print_channel_status(
    const std::array<sparse_fir_block_control::sptr, NUM_DL_CHANNELS>& sfir_dl,
    const std::array<shiftright_block_control::sptr, NUM_DL_CHANNELS>& shift_dl,
    const std::array<sparse_fir_block_control::sptr, NUM_UL_CHANNELS>& sfir_ul,
    const std::array<shiftright_block_control::sptr, NUM_UL_CHANNELS>& shift_ul)
{
    uint32_t num_taps = sfir_dl[0]->get_num_taps();
    uint32_t max_delay = sfir_dl[0]->get_max_delay();

    std::cout << "\n=== Sparse FIR Config (taps=" << num_taps 
              << ", max_delay=" << max_delay << " samples) ===" << std::endl;

    // Helper to format a tap as "delay:re" or "delay:re+imj"
    auto fmt_tap = [](uint32_t d, int16_t re, int16_t im) -> std::string {
        if (im == 0) {
            return (boost::format("%d:%d") % d % re).str();
        } else if (im > 0) {
            return (boost::format("%d:%d+%dj") % d % re % im).str();
        } else {
            return (boost::format("%d:%d%dj") % d % re % im).str();
        }
    };

    std::cout << "--- Downlink (gNB -> UEs) ---" << std::endl;
    for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
        uint32_t shift_val = shift_dl[i]->get_shiftright_value();
        std::cout << boost::format("  DL%d (gNB->UE%d): shift=%2d, taps=[")
                     % i % (i + 1) % shift_val;
        for (uint32_t t = 0; t < num_taps; t++) {
            uint32_t d = sfir_dl[i]->get_tap_delay(t);
            auto [re, im] = sfir_dl[i]->get_tap_coeff_complex(t);
            std::cout << fmt_tap(d, re, im);
            if (t < num_taps - 1) std::cout << " ";
        }
        std::cout << "]" << std::endl;
    }

    std::cout << "--- Uplink (UEs -> gNB) ---" << std::endl;
    for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
        uint32_t shift_val = shift_ul[i]->get_shiftright_value();
        std::cout << boost::format("  UL%d (UE%d->gNB): shift=%2d, taps=[")
                     % i % (i + 1) % shift_val;
        for (uint32_t t = 0; t < num_taps; t++) {
            uint32_t d = sfir_ul[i]->get_tap_delay(t);
            auto [re, im] = sfir_ul[i]->get_tap_coeff_complex(t);
            std::cout << fmt_tap(d, re, im);
            if (t < num_taps - 1) std::cout << " ";
        }
        std::cout << "]" << std::endl;
    }
}

/****************************************************************************
 * main
 ***************************************************************************/
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    std::string args;
    double gnb_freq, ue_freq, rx_bw, tx_bw, update_t, print_t, scruni_t;
    std::string rx_gains_str, tx_gains_str;
    double default_rx_gain = 0.0, default_tx_gain = 0.0;

    // Block IDs
    std::string radio0_id = "0/Radio#0";
    std::string radio1_id = "0/Radio#1";

    // SparseFIR block IDs (matching image core)
    std::string sfir_dl0_id = "0/SparseFIR#0";
    std::string sfir_dl1_id = "0/SparseFIR#1";
    std::string sfir_dl2_id = "0/SparseFIR#2";
    std::string sfir_ul0_id = "0/SparseFIR#3";
    std::string sfir_ul1_id = "0/SparseFIR#4";
    std::string sfir_ul2_id = "0/SparseFIR#5";

    std::string shift_dl0_id = "0/Shiftright#0";
    std::string shift_dl1_id = "0/Shiftright#1";
    std::string shift_dl2_id = "0/Shiftright#2";
    std::string shift_ul0_id = "0/Shiftright#3";
    std::string shift_ul1_id = "0/Shiftright#4";
    std::string shift_ul2_id = "0/Shiftright#5";

    double setup_time = 0.1;
    size_t spp = 32;
    bool rx_timestamps = false;
    bool use_script = false;
    bool use_doppler = false;
    double doppler_update_t = 0.002;  // 2 ms default (500 Hz)
    std::string doppler_spec_str;

    // Config file paths
    std::string root = CMAKE_SOURCE_DIR;
    std::string config_path_manually = root + "/channel_control/chan_4chan_sparse_manually.csv";
    std::string config_path_script   = root + "/channel_control/chan_4chan_sparse_script.csv";
    std::ifstream config_in;

    // Program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "UHD device address args")
        ("gnb-freq", po::value<double>(&gnb_freq)->default_value(3619.2e6), "gNB RF center frequency in Hz")
        ("ue-freq", po::value<double>(&ue_freq)->default_value(3619.2e6), "UE RF center frequency in Hz")
        ("rx-gains", po::value<std::string>(&rx_gains_str)->default_value(""),
            "Per-port RX gains: gNB,UE1,UE2,UE3")
        ("tx-gains", po::value<std::string>(&tx_gains_str)->default_value(""),
            "Per-port TX gains: gNB,UE1,UE2,UE3")
        ("rx-bw", po::value<double>(&rx_bw)->default_value(100e6), "RX bandwidth (Hz)")
        ("tx-bw", po::value<double>(&tx_bw)->default_value(100e6), "TX bandwidth (Hz)")
        ("udt", po::value<double>(&update_t)->default_value(1), "Channel update period (s)")
        ("prt", po::value<double>(&print_t)->default_value(5), "Status print period (s)")
        ("script", "Use script mode")
        ("fast-script", "Pre-load script CSV into memory for fast playback (ms-level updates)")
        ("scr-t", po::value<double>(&scruni_t)->default_value(0.2), "Script time resolution (s)")
        ("doppler", po::value<std::string>(&doppler_spec_str)->implicit_value(""),
            "Enable Doppler mode. Provide per-channel specs as comma-separated fields:\n"
            "  ch0_taps,ch0_shift,ch1_taps,ch1_shift,...\n"
            "Each tap: delay:amplitude:fd_hz:phi0_deg\n"
            "Example: '0:0.9:194:0 20:0.5:-120:60,4,...'")
        ("doppler-rate", po::value<double>(&doppler_update_t)->default_value(0.002),
            "Doppler tap update interval in seconds (default: 0.002 = 2 ms)")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << boost::format("OpenAirLink 4-Channel Sparse FIR Emulator\n%s") % desc
                  << std::endl;
        std::cout << "\nCSV format per channel: delay0:coeff0 delay1:coeff1 ... , shift_value\n"
                  << "Complex coefficients: delay:re+imj (e.g. 100:23170+23170j)\n"
                  << "Example: 0:32767 100:8000-4000j 200:4000 0:0, 6\n"
                  << "\nDoppler mode: --doppler 'taps_ch0,shift0,taps_ch1,shift1,...'\n"
                  << "Tap format: delay:amplitude:fd_hz:phi0_deg (space-separated)\n"
                  << "Example: --doppler '0:0.9:194:0 20:0.5:-120:60,4,0:1.0:0:0,0,...'\n"
                  << "Use --doppler-rate to set update interval (default 2 ms)\n"
                  << "\nFast script mode: --script --fast-script\n"
                  << "Pre-loads CSV into memory for ms-level update playback.\n"
                  << "Generate CSV with: channel_control/generate_doppler_script.py\n"
                  << std::endl;
        return ~0;
    }

    /************************************************************************
     * Create device and block controls
     ***********************************************************************/
    std::cout << std::endl;
    std::cout << boost::format("Creating the RFNoC graph with args: %s...") % args << std::endl;
    uhd::rfnoc::rfnoc_graph::sptr graph = uhd::rfnoc::rfnoc_graph::make(args);

    // Radio controls
    auto radio0_ctrl = graph->get_block<radio_control>(uhd::rfnoc::block_id_t(radio0_id));
    auto radio1_ctrl = graph->get_block<radio_control>(uhd::rfnoc::block_id_t(radio1_id));

    std::cout << "Using radio0 (gNB port0, UE1 port1)" << std::endl;
    std::cout << "Using radio1 (UE2 port0, UE3 port1)" << std::endl;

    size_t mb_idx = uhd::rfnoc::block_id_t(radio0_id).get_device_no();

    // SparseFIR controls
    std::array<sparse_fir_block_control::sptr, NUM_DL_CHANNELS> sfir_dl_ctrl;
    std::array<sparse_fir_block_control::sptr, NUM_UL_CHANNELS> sfir_ul_ctrl;

    sfir_dl_ctrl[0] = graph->get_block<sparse_fir_block_control>(uhd::rfnoc::block_id_t(sfir_dl0_id));
    sfir_dl_ctrl[1] = graph->get_block<sparse_fir_block_control>(uhd::rfnoc::block_id_t(sfir_dl1_id));
    sfir_dl_ctrl[2] = graph->get_block<sparse_fir_block_control>(uhd::rfnoc::block_id_t(sfir_dl2_id));
    sfir_ul_ctrl[0] = graph->get_block<sparse_fir_block_control>(uhd::rfnoc::block_id_t(sfir_ul0_id));
    sfir_ul_ctrl[1] = graph->get_block<sparse_fir_block_control>(uhd::rfnoc::block_id_t(sfir_ul1_id));
    sfir_ul_ctrl[2] = graph->get_block<sparse_fir_block_control>(uhd::rfnoc::block_id_t(sfir_ul2_id));

    // Shiftright controls
    std::array<shiftright_block_control::sptr, NUM_DL_CHANNELS> shift_dl_ctrl;
    std::array<shiftright_block_control::sptr, NUM_UL_CHANNELS> shift_ul_ctrl;

    shift_dl_ctrl[0] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_dl0_id));
    shift_dl_ctrl[1] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_dl1_id));
    shift_dl_ctrl[2] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_dl2_id));
    shift_ul_ctrl[0] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_ul0_id));
    shift_ul_ctrl[1] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_ul1_id));
    shift_ul_ctrl[2] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_ul2_id));

    std::cout << "All RFNoC blocks acquired successfully." << std::endl;

    uint32_t num_taps = sfir_dl_ctrl[0]->get_num_taps();
    uint32_t max_delay = sfir_dl_ctrl[0]->get_max_delay();
    std::cout << boost::format("Sparse FIR: %d taps, max delay = %d samples (%.2f us at 200MHz)")
                 % num_taps % max_delay % (max_delay * 5e-3) << std::endl;

    /************************************************************************
     * Commit graph (connections are hard-wired in FPGA image)
     ***********************************************************************/
    graph->commit();
    std::cout << "RFNoC graph committed." << std::endl;

    radio0_ctrl->enable_rx_timestamps(rx_timestamps, 0);
    radio0_ctrl->enable_rx_timestamps(rx_timestamps, 1);
    radio1_ctrl->enable_rx_timestamps(rx_timestamps, 0);
    radio1_ctrl->enable_rx_timestamps(rx_timestamps, 1);

    /************************************************************************
     * Initialize sparse FIR blocks (default: passthrough on tap 0)
     ***********************************************************************/
    for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
        sfir_dl_ctrl[i]->set_tap(0, 0, 32767);  // Tap 0: delay=0, max gain
        for (uint32_t t = 1; t < num_taps; t++) {
            sfir_dl_ctrl[i]->set_tap(t, 0, 0);  // Remaining taps: disabled
        }
        shift_dl_ctrl[i]->set_shiftright_value(0);
    }
    for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
        sfir_ul_ctrl[i]->set_tap(0, 0, 32767);
        for (uint32_t t = 1; t < num_taps; t++) {
            sfir_ul_ctrl[i]->set_tap(t, 0, 0);
        }
        shift_ul_ctrl[i]->set_shiftright_value(0);
    }

    /************************************************************************
     * Set up RF parameters
     ***********************************************************************/
    double rate = radio0_ctrl->get_rate();
    std::cout << boost::format("Sample Rate: %f Msps") % (rate / 1e6) << std::endl;

    radio0_ctrl->set_rx_frequency(gnb_freq, 0);
    radio0_ctrl->set_tx_frequency(gnb_freq, 0);
    std::cout << boost::format("gNB Freq: %f MHz") % (radio0_ctrl->get_rx_frequency(0) / 1e6) << std::endl;

    radio0_ctrl->set_rx_frequency(ue_freq, 1);
    radio0_ctrl->set_tx_frequency(ue_freq, 1);
    radio1_ctrl->set_rx_frequency(ue_freq, 0);
    radio1_ctrl->set_tx_frequency(ue_freq, 0);
    radio1_ctrl->set_rx_frequency(ue_freq, 1);
    radio1_ctrl->set_tx_frequency(ue_freq, 1);

    std::vector<double> rx_gains = parse_gains(rx_gains_str, 4, default_rx_gain);
    std::vector<double> tx_gains = parse_gains(tx_gains_str, 4, default_tx_gain);

    radio0_ctrl->set_rx_gain(rx_gains[0], 0);
    radio0_ctrl->set_rx_gain(rx_gains[1], 1);
    radio1_ctrl->set_rx_gain(rx_gains[2], 0);
    radio1_ctrl->set_rx_gain(rx_gains[3], 1);

    radio0_ctrl->set_tx_gain(tx_gains[0], 0);
    radio0_ctrl->set_tx_gain(tx_gains[1], 1);
    radio1_ctrl->set_tx_gain(tx_gains[2], 0);
    radio1_ctrl->set_tx_gain(tx_gains[3], 1);

    radio0_ctrl->set_rx_bandwidth(rx_bw, 0);
    radio0_ctrl->set_rx_bandwidth(rx_bw, 1);
    radio1_ctrl->set_rx_bandwidth(rx_bw, 0);
    radio1_ctrl->set_rx_bandwidth(rx_bw, 1);

    radio0_ctrl->set_tx_bandwidth(tx_bw, 0);
    radio0_ctrl->set_tx_bandwidth(tx_bw, 1);
    radio1_ctrl->set_tx_bandwidth(tx_bw, 0);
    radio1_ctrl->set_tx_bandwidth(tx_bw, 1);

    radio0_ctrl->set_property<int>("spp", spp, 0);
    radio0_ctrl->set_property<int>("spp", spp, 1);
    radio1_ctrl->set_property<int>("spp", spp, 0);
    radio1_ctrl->set_property<int>("spp", spp, 1);
    spp = radio0_ctrl->get_property<int>("spp", 0);
    std::cout << "Samples per packet: " << spp << std::endl;

    /************************************************************************
     * Start streaming
     ***********************************************************************/
    std::this_thread::sleep_for(1s * setup_time);
    std::signal(SIGINT, &sig_int_handler);

    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    stream_cmd.stream_now = false;
    stream_cmd.time_spec =
        graph->get_mb_controller(mb_idx)->get_timekeeper(mb_idx)->get_time_now()
        + setup_time;

    std::cout << "Issuing start stream cmd..." << std::endl;
    radio0_ctrl->issue_stream_cmd(stream_cmd, 0);
    radio0_ctrl->issue_stream_cmd(stream_cmd, 1);
    radio1_ctrl->issue_stream_cmd(stream_cmd, 0);
    radio1_ctrl->issue_stream_cmd(stream_cmd, 1);

    std::cout << std::endl;
    std::cout << "***********************************************************" << std::endl;
    std::cout << "*  OpenAirLink 4-Channel Sparse FIR Emulation Running     *" << std::endl;
    std::cout << "*  1 gNB + 3 UE, " << num_taps << " taps, max_delay=" << max_delay
              << " samples   *" << std::endl;
    if (use_doppler) {
        std::cout << "*  Mode: DOPPLER (update rate: "
                  << (1.0 / doppler_update_t) << " Hz)" << std::string(
                      std::max(0, 26 - static_cast<int>(
                          std::to_string(static_cast<int>(1.0 / doppler_update_t)).size())), ' ')
                  << "*" << std::endl;
    }
    std::cout << "***********************************************************" << std::endl;

    // Determine operating mode
    use_doppler = vm.count("doppler") > 0;
    if (vm.count("script")) {
        use_script = true;
    }

    print_channel_status(sfir_dl_ctrl, shift_dl_ctrl, sfir_ul_ctrl, shift_ul_ctrl);

    /************************************************************************
     * Channel update loop
     ***********************************************************************/
    std::string taps_str;
    std::string bit_str;

    double elapsed_time = 0.0;

    // Lambda to apply one channel config
    auto apply_channel = [&](sparse_fir_block_control::sptr sfir,
                             shiftright_block_control::sptr shift,
                             const std::string& taps_s,
                             const std::string& shift_s) {
        auto cfg = parse_sparse_taps(taps_s, num_taps);
        uint32_t bit_shift = static_cast<uint32_t>(std::stoi(space_trim(shift_s)));
        sfir->set_all_taps_complex(cfg.delays, cfg.coeffs_re, cfg.coeffs_im);
        shift->set_shiftright_value(bit_shift);
    };

    // Lambda to apply a pre-parsed row to all 6 channels
    auto apply_row = [&](const script_row& row) {
        for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
            sfir_dl_ctrl[i]->set_all_taps_complex(
                row.taps[i].delays, row.taps[i].coeffs_re, row.taps[i].coeffs_im);
            shift_dl_ctrl[i]->set_shiftright_value(row.shifts[i]);
        }
        for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
            size_t idx = NUM_DL_CHANNELS + i;
            sfir_ul_ctrl[i]->set_all_taps_complex(
                row.taps[idx].delays, row.taps[idx].coeffs_re, row.taps[idx].coeffs_im);
            shift_ul_ctrl[i]->set_shiftright_value(row.shifts[idx]);
        }
    };

    // ======================================================================
    // MODE 1: Doppler — real-time complex coefficient computation
    // ======================================================================
    if (use_doppler) {
        std::cout << "\nUsing Doppler Mode (update rate: "
                  << (1.0 / doppler_update_t) << " Hz)..." << std::endl;

        // Parse Doppler channel specs from the --doppler argument
        std::array<doppler_channel, NUM_TOTAL_CHANNELS> doppler_channels;
        std::array<uint32_t, NUM_TOTAL_CHANNELS> doppler_shifts;

        if (!doppler_spec_str.empty()) {
            // Parse comma-separated: taps0,shift0,taps1,shift1,...
            std::istringstream dss(doppler_spec_str);
            for (size_t i = 0; i < NUM_TOTAL_CHANNELS; i++) {
                std::string ch_taps, ch_shift;
                std::getline(dss, ch_taps, ',');
                std::getline(dss, ch_shift, ',');
                doppler_channels[i] = parse_doppler_channel(ch_taps);
                doppler_shifts[i] = static_cast<uint32_t>(std::stoi(space_trim(ch_shift)));
            }
        } else {
            // Default: single-tap passthrough with no Doppler
            std::cout << "No Doppler spec provided, using passthrough." << std::endl;
            for (size_t i = 0; i < NUM_TOTAL_CHANNELS; i++) {
                doppler_tap tap;
                tap.delay = 0;
                tap.amplitude = 1.0;
                tap.fd_hz = 0.0;
                tap.phi0 = 0.0;
                doppler_channels[i].taps.push_back(tap);
                doppler_shifts[i] = 0;
            }
        }

        // Set initial shift values (they don't change during Doppler)
        for (size_t i = 0; i < NUM_DL_CHANNELS; i++)
            shift_dl_ctrl[i]->set_shiftright_value(doppler_shifts[i]);
        for (size_t i = 0; i < NUM_UL_CHANNELS; i++)
            shift_ul_ctrl[i]->set_shiftright_value(doppler_shifts[NUM_DL_CHANNELS + i]);

        // Print Doppler config summary
        double max_fd = 0.0;
        for (size_t i = 0; i < NUM_TOTAL_CHANNELS; i++) {
            for (const auto& tap : doppler_channels[i].taps) {
                max_fd = std::max(max_fd, std::abs(tap.fd_hz));
            }
        }
        std::cout << boost::format("  Max Doppler shift: %.1f Hz") % max_fd << std::endl;
        double nyquist = 2.0 * max_fd;
        double rate_hz = 1.0 / doppler_update_t;
        if (rate_hz < nyquist) {
            std::cout << boost::format("  WARNING: Update rate %.0f Hz < Nyquist %.0f Hz!")
                         % rate_hz % nyquist << std::endl;
        }

        // Pre-initialize all taps: write delays once, zero all coefficients.
        // This avoids writing delays every update cycle (they don't change).
        std::cout << "Initializing tap delays and zeroing coefficients..." << std::endl;
        for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
            auto cfg = compute_doppler_taps(doppler_channels[i], 0.0, num_taps);
            sfir_dl_ctrl[i]->set_all_taps_complex(cfg.delays, cfg.coeffs_re, cfg.coeffs_im);
        }
        for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
            auto cfg = compute_doppler_taps(doppler_channels[NUM_DL_CHANNELS + i], 0.0, num_taps);
            sfir_ul_ctrl[i]->set_all_taps_complex(cfg.delays, cfg.coeffs_re, cfg.coeffs_im);
        }

        // Count active taps per channel for the status line
        size_t total_active_taps = 0;
        for (size_t i = 0; i < NUM_TOTAL_CHANNELS; i++)
            total_active_taps += doppler_channels[i].taps.size();
        size_t writes_per_update = total_active_taps; // 1 coeff poke32 per active tap
        std::cout << boost::format("  Active taps: %zu across %d channels (%zu poke32/update)")
                     % total_active_taps % NUM_TOTAL_CHANNELS % writes_per_update << std::endl;

        std::cout << "Press Enter to start Doppler emulation..." << std::endl;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        auto t_start = std::chrono::steady_clock::now();
        auto t_next = t_start;
        auto dt = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(doppler_update_t));
        size_t update_count = 0;
        auto t_last_print = t_start;

        auto clamp16 = [](double v) -> int16_t {
            double scaled = v * 32767.0;
            if (scaled > 32767.0) scaled = 32767.0;
            if (scaled < -32768.0) scaled = -32768.0;
            return static_cast<int16_t>(std::round(scaled));
        };

        while (!stop_signal_called) {
            t_next += dt;
            double t = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_start).count();

            // Only write coefficients for active taps (skip zeros, skip delays).
            // Delays were written once during initialization and don't change.
            for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
                const auto& ch = doppler_channels[i];
                for (size_t j = 0; j < ch.taps.size(); j++) {
                    const auto& tap = ch.taps[j];
                    double phase = 2.0 * M_PI * tap.fd_hz * t + tap.phi0;
                    sfir_dl_ctrl[i]->set_tap_coeff_complex(
                        static_cast<uint32_t>(j),
                        clamp16(tap.amplitude * std::cos(phase)),
                        clamp16(tap.amplitude * std::sin(phase)));
                }
            }
            for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
                const auto& ch = doppler_channels[NUM_DL_CHANNELS + i];
                for (size_t j = 0; j < ch.taps.size(); j++) {
                    const auto& tap = ch.taps[j];
                    double phase = 2.0 * M_PI * tap.fd_hz * t + tap.phi0;
                    sfir_ul_ctrl[i]->set_tap_coeff_complex(
                        static_cast<uint32_t>(j),
                        clamp16(tap.amplitude * std::cos(phase)),
                        clamp16(tap.amplitude * std::sin(phase)));
                }
            }

            update_count++;

            // Periodic status print
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_last_print).count() >= print_t) {
                double actual_rate = update_count /
                    std::chrono::duration<double>(now - t_start).count();
                std::cout << boost::format("\n[Doppler] t=%.3fs  updates=%zu  actual_rate=%.0f Hz")
                             % t % update_count % actual_rate << std::endl;
                print_channel_status(sfir_dl_ctrl, shift_dl_ctrl, sfir_ul_ctrl, shift_ul_ctrl);
                t_last_print = now;
            }

            // Sleep until next update
            std::this_thread::sleep_until(t_next);
        }
    }
    // ======================================================================
    // MODE 2: Fast Script — pre-loaded CSV with steady_clock timing
    // ======================================================================
    else if (use_script && vm.count("fast-script")) {
        std::cout << "\nUsing Fast Script Mode (pre-loaded)..." << std::endl;

        auto rows = preload_script_csv(config_path_script, num_taps);
        std::cout << boost::format("Loaded %zu script rows into memory.") % rows.size()
                  << std::endl;

        if (rows.empty()) {
            std::cout << "Error: Script CSV is empty." << std::endl;
            stop_signal_called = true;
        } else {
            std::cout << boost::format("Time range: %.6f -- %.6f s")
                         % rows.front().time_index % rows.back().time_index << std::endl;
            std::cout << "Press Enter to start..." << std::endl;
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

            auto t_start = std::chrono::steady_clock::now();
            size_t row_idx = 0;
            auto t_last_print = t_start;

            while (!stop_signal_called && row_idx < rows.size()) {
                double t = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t_start).count();

                if (t >= rows[row_idx].time_index) {
                    apply_row(rows[row_idx]);
                    row_idx++;

                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration<double>(now - t_last_print).count() >= print_t) {
                        std::cout << boost::format("\n[FastScript] step=%zu/%zu  t=%.3fs")
                                     % row_idx % rows.size() % t << std::endl;
                        print_channel_status(sfir_dl_ctrl, shift_dl_ctrl,
                                             sfir_ul_ctrl, shift_ul_ctrl);
                        t_last_print = now;
                    }
                } else {
                    // Busy-wait or short sleep for sub-ms resolution
                    double dt_remaining = rows[row_idx].time_index - t;
                    if (dt_remaining > 0.001) {
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(
                                static_cast<long>((dt_remaining - 0.0005) * 1e6)));
                    }
                }
            }

            if (row_idx >= rows.size()) {
                std::cout << "\nEnd of script, keeping current config." << std::endl;
                while (!stop_signal_called) {
                    std::this_thread::sleep_for(100ms);
                }
            }
        }
    }
    // ======================================================================
    // MODE 3: Original Script — streaming CSV with sleep-based timing
    // ======================================================================
    else if (use_script && is_csv_valid(config_path_script)) {
        std::cout << "\nUsing Script Mode..." << std::endl;
        config_in.open(config_path_script);

        int step = 0;
        double curr_index;
        std::string index;

        std::getline(config_in, index, ',');
        curr_index = std::stod(index);

        std::cout << boost::format("Script starts at: %.3fs") % curr_index << std::endl;
        std::cout << "Press Enter to start..." << std::endl;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        while (!stop_signal_called) {
            if (elapsed_time >= curr_index) {
                for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
                    std::getline(config_in, taps_str, ',');
                    std::getline(config_in, bit_str, ',');
                    apply_channel(sfir_dl_ctrl[i], shift_dl_ctrl[i], taps_str, bit_str);
                }
                for (size_t i = 0; i < NUM_UL_CHANNELS - 1; i++) {
                    std::getline(config_in, taps_str, ',');
                    std::getline(config_in, bit_str, ',');
                    apply_channel(sfir_ul_ctrl[i], shift_ul_ctrl[i], taps_str, bit_str);
                }
                std::getline(config_in, taps_str, ',');
                std::getline(config_in, bit_str);
                apply_channel(sfir_ul_ctrl[NUM_UL_CHANNELS - 1],
                              shift_ul_ctrl[NUM_UL_CHANNELS - 1], taps_str, bit_str);

                step++;
                std::cout << std::endl;
                std::cout << boost::format("Script Step: %d   Time: %.3fs") % step % elapsed_time
                          << std::endl;
                print_channel_status(sfir_dl_ctrl, shift_dl_ctrl, sfir_ul_ctrl, shift_ul_ctrl);

                std::getline(config_in, index, ',');
                index = space_trim(index);

                if (index != "eos") {
                    curr_index = std::stod(index);
                } else {
                    curr_index = std::numeric_limits<double>::infinity();
                    std::cout << "End of script, keeping current config..." << std::endl;
                }
            }

            std::this_thread::sleep_for(1000ms * scruni_t);
            elapsed_time += scruni_t;
            std::cout << '.' << std::flush;
        }
        config_in.close();
    }
    // ======================================================================
    // MODE 4: Manual — poll CSV file periodically
    // ======================================================================
    else {
        if (use_script) {
            std::cout << "Warning: Could not open script config, using manual mode." << std::endl;
        }
        std::cout << "\nUsing Manual Mode..." << std::endl;

        while (!stop_signal_called) {
            if (is_csv_valid(config_path_manually)) {
                config_in.open(config_path_manually);

                for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
                    std::getline(config_in, taps_str, ',');
                    std::getline(config_in, bit_str, ',');
                    apply_channel(sfir_dl_ctrl[i], shift_dl_ctrl[i], taps_str, bit_str);
                }
                for (size_t i = 0; i < NUM_UL_CHANNELS - 1; i++) {
                    std::getline(config_in, taps_str, ',');
                    std::getline(config_in, bit_str, ',');
                    apply_channel(sfir_ul_ctrl[i], shift_ul_ctrl[i], taps_str, bit_str);
                }
                std::getline(config_in, taps_str, ',');
                std::getline(config_in, bit_str);
                apply_channel(sfir_ul_ctrl[NUM_UL_CHANNELS - 1],
                              shift_ul_ctrl[NUM_UL_CHANNELS - 1], taps_str, bit_str);

                config_in.close();
            } else {
                std::cout << "Warning: Could not open config at '" << config_path_manually
                          << "'." << std::endl;
            }

            std::this_thread::sleep_for(1000ms * update_t);
            elapsed_time += update_t;
            std::cout << '.' << std::flush;

            if (std::fmod(elapsed_time, print_t) == 0) {
                std::cout << std::endl;
                std::cout << boost::format("Running Time: %.1fs") % elapsed_time << std::endl;
                print_channel_status(sfir_dl_ctrl, shift_dl_ctrl, sfir_ul_ctrl, shift_ul_ctrl);
            }
        }
    }

    /************************************************************************
     * Stop streaming
     ***********************************************************************/
    std::cout << std::endl;
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    std::cout << "Issuing stop stream cmd..." << std::endl;
    radio0_ctrl->issue_stream_cmd(stream_cmd, 0);
    radio0_ctrl->issue_stream_cmd(stream_cmd, 1);
    radio1_ctrl->issue_stream_cmd(stream_cmd, 0);
    radio1_ctrl->issue_stream_cmd(stream_cmd, 1);

    std::cout << "Done" << std::endl << std::endl;
    std::this_thread::sleep_for(100ms);

    return EXIT_SUCCESS;
}
