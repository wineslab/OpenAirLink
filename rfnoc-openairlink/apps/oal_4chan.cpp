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
 * OpenAirLink 4-Channel Bidirectional Channel Emulator
 * 
 * Topology (1 gNB + 3 UEs):
 *   gNB  <---> radio0 port 0 (RX: gNB TX, TX: combined UL to gNB)
 *   UE1  <---> radio0 port 1 (RX: UE1 TX, TX: DL to UE1)
 *   UE2  <---> radio1 port 0 (RX: UE2 TX, TX: DL to UE2)
 *   UE3  <---> radio1 port 1 (RX: UE3 TX, TX: DL to UE3)
 *
 * Signal Flow:
 *   DOWNLINK: radio0_RX0 -> split -> FIR_DL -> shift_DL -> UE radios TX
 *   UPLINK:   UE radios RX -> FIR_UL -> shift_UL -> ADDER cascade -> radio0_TX0
 *
 * Channel Configuration (6 independent channels):
 *   DL0: gNB -> UE1   (fir_dl0, shift_dl0)
 *   DL1: gNB -> UE2   (fir_dl1, shift_dl1)
 *   DL2: gNB -> UE3   (fir_dl2, shift_dl2)
 *   UL0: UE1 -> gNB   (fir_ul0, shift_ul0)
 *   UL1: UE2 -> gNB   (fir_ul1, shift_ul1)
 *   UL2: UE3 -> gNB   (fir_ul2, shift_ul2)
 */

#include <uhd/rfnoc/block_id.hpp>
#include <uhd/rfnoc/mb_controller.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/rfnoc/fir_filter_block_control.hpp>
#include <uhd/rfnoc_graph.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/utils/graph_utils.hpp>
#include <uhd/utils/math.hpp>
#include <uhd/utils/safe_main.hpp>
#include <rfnoc/openairlink/shiftright_block_control.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <array>

namespace po = boost::program_options;
using uhd::rfnoc::radio_control;
using uhd::rfnoc::fir_filter_block_control;
using rfnoc::openairlink::shiftright_block_control;
using namespace std::chrono_literals;

// Number of channels (3 DL + 3 UL)
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
 * String to FIR Coeffs
 ***************************************************************************/
std::vector<int16_t> fir_parser(std::string input)
{
    std::istringstream iss(input);
    std::vector<int16_t> fir_coeffs;
    int temp;

    while (iss >> temp) {
        fir_coeffs.push_back(static_cast<int16_t>(temp));
    }

    return fir_coeffs;
}

/****************************************************************************
 * Utility function to trim whitespace from both ends of a string
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
 * Parse comma-separated gain values (e.g., "0,10,15,20" -> vector of 4 doubles)
 ***************************************************************************/
std::vector<double> parse_gains(const std::string& input, size_t expected_count, double default_val)
{
    std::vector<double> gains;
    if (input.empty()) {
        // Return vector filled with default value
        gains.assign(expected_count, default_val);
        return gains;
    }
    
    std::istringstream iss(input);
    std::string token;
    while (std::getline(iss, token, ',')) {
        gains.push_back(std::stod(space_trim(token)));
    }
    
    // Pad with default value if fewer values provided
    while (gains.size() < expected_count) {
        gains.push_back(default_val);
    }
    
    return gains;
}

/****************************************************************************
 * Verify The condition of CSV config file
 ***************************************************************************/
bool is_csv_valid(const std::string& path) {
    std::ifstream target_csv;
    bool valid;

    target_csv.open(path);
    valid = !target_csv.fail();
    target_csv.close();

    return valid;
}

/****************************************************************************
 * Print channel status
 ***************************************************************************/
void print_channel_status(
    const std::array<fir_filter_block_control::sptr, NUM_DL_CHANNELS>& fir_dl,
    const std::array<shiftright_block_control::sptr, NUM_DL_CHANNELS>& shift_dl,
    const std::array<fir_filter_block_control::sptr, NUM_UL_CHANNELS>& fir_ul,
    const std::array<shiftright_block_control::sptr, NUM_UL_CHANNELS>& shift_ul)
{
    std::vector<int16_t> coeffs;
    uint32_t shift_val;

    std::cout << "\n=== Downlink Channels (gNB -> UEs) ===" << std::endl;
    for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
        shift_val = shift_dl[i]->get_shiftright_value();
        coeffs = fir_dl[i]->get_coefficients();
        std::cout << boost::format("  DL%d (gNB->UE%d): shift=%2d, FIR=") % i % (i+1) % shift_val;
        for (size_t j = 0; j < std::min(coeffs.size(), size_t(5)); j++) {
            std::cout << coeffs[j] << " ";
        }
        if (coeffs.size() > 5) std::cout << "...";
        std::cout << std::endl;
    }

    std::cout << "=== Uplink Channels (UEs -> gNB) ===" << std::endl;
    for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
        shift_val = shift_ul[i]->get_shiftright_value();
        coeffs = fir_ul[i]->get_coefficients();
        std::cout << boost::format("  UL%d (UE%d->gNB): shift=%2d, FIR=") % i % (i+1) % shift_val;
        for (size_t j = 0; j < std::min(coeffs.size(), size_t(5)); j++) {
            std::cout << coeffs[j] << " ";
        }
        if (coeffs.size() > 5) std::cout << "...";
        std::cout << std::endl;
    }
}

/****************************************************************************
 * main
 ***************************************************************************/
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // variables to be set by po
    std::string args;
    double gnb_freq, ue_freq, rx_bw, tx_bw, update_t, print_t, scruni_t;
    std::string rx_gains_str, tx_gains_str;  // Comma-separated per-port gains
    double default_rx_gain = 0.0, default_tx_gain = 0.0;

    // Block IDs for the 4-channel configuration
    std::string radio0_id = "0/Radio#0";   // gNB (port 0) + UE1 (port 1)
    std::string radio1_id = "0/Radio#1";   // UE2 (port 0) + UE3 (port 1)

    // FIR and Shiftright block IDs (matching FPGA image core)
    std::string fir_dl0_id   = "0/FIR#0";       // DL to UE1
    std::string fir_dl1_id   = "0/FIR#1";       // DL to UE2
    std::string fir_dl2_id   = "0/FIR#2";       // DL to UE3
    std::string fir_ul0_id   = "0/FIR#3";       // UL from UE1
    std::string fir_ul1_id   = "0/FIR#4";       // UL from UE2
    std::string fir_ul2_id   = "0/FIR#5";       // UL from UE3

    std::string shift_dl0_id = "0/Shiftright#0";
    std::string shift_dl1_id = "0/Shiftright#1";
    std::string shift_dl2_id = "0/Shiftright#2";
    std::string shift_ul0_id = "0/Shiftright#3";
    std::string shift_ul1_id = "0/Shiftright#4";
    std::string shift_ul2_id = "0/Shiftright#5";

    double setup_time = 0.1;
    
    // Default FIR coefficients (passthrough)
    std::vector<int16_t> default_fir;
    default_fir.push_back(32767);

    size_t spp = 32; // Samples per packet (reduce for lower latency)
    bool rx_timestamps = false;
    bool use_script = false;

    // Config file paths
    std::string root = CMAKE_SOURCE_DIR;
    std::string config_path_manually = root + "/channel_control/chan_4chan_manually.csv";
    std::string config_path_script   = root + "/channel_control/chan_4chan_script.csv";
    std::ifstream config_in;

    // setup the program options
    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "UHD device address args")
        ("gnb-freq", po::value<double>(&gnb_freq)->default_value(3619.2e6), "gNB RF center frequency in Hz")
        ("ue-freq", po::value<double>(&ue_freq)->default_value(3619.2e6), "UE RF center frequency in Hz")
        ("rx-gains", po::value<std::string>(&rx_gains_str)->default_value(""), 
            "Per-port RX gains in dB, comma-separated: gNB,UE1,UE2,UE3 (e.g., '0,10,15,20')")
        ("tx-gains", po::value<std::string>(&tx_gains_str)->default_value(""), 
            "Per-port TX gains in dB, comma-separated: gNB,UE1,UE2,UE3 (e.g., '0,10,15,20')")
        ("rx-bw", po::value<double>(&rx_bw)->default_value(100e6), "RX analog frontend filter bandwidth in Hz")
        ("tx-bw", po::value<double>(&tx_bw)->default_value(100e6), "TX analog frontend filter bandwidth in Hz")
        ("udt", po::value<double>(&update_t)->default_value(1), "Time period to update emulator channel (s)")
        ("prt", po::value<double>(&print_t)->default_value(5), "Time period to print channel status (s)")
        ("script", "Use channel script config instead of manual")
        ("scr-t", po::value<double>(&scruni_t)->default_value(0.2), "Script time resolution (s)")
    ;
    // clang-format on
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // print the help message
    if (vm.count("help")) {
        std::cout << boost::format("OpenAirLink 4-Channel Bidirectional Emulator\n%s") % desc << std::endl;
        std::cout
            << std::endl
            << "This application runs a 1 gNB + 3 UE bidirectional channel emulator.\n"
            << "\nTopology:\n"
            << "  gNB  <---> radio0 port 0\n"
            << "  UE1  <---> radio0 port 1\n"
            << "  UE2  <---> radio1 port 0\n"
            << "  UE3  <---> radio1 port 1\n"
            << std::endl;
        return ~0;
    }

    /************************************************************************
     * Create device and block controls
     ***********************************************************************/
    std::cout << std::endl;
    std::cout << boost::format("Creating the RFNoC graph with args: %s...") % args << std::endl;
    uhd::rfnoc::rfnoc_graph::sptr graph = uhd::rfnoc::rfnoc_graph::make(args);

    // Create handles for radio objects
    uhd::rfnoc::block_id_t radio0_ctrl_id(radio0_id);
    uhd::rfnoc::block_id_t radio1_ctrl_id(radio1_id);

    uhd::rfnoc::radio_control::sptr radio0_ctrl =
        graph->get_block<uhd::rfnoc::radio_control>(radio0_ctrl_id);
    uhd::rfnoc::radio_control::sptr radio1_ctrl =
        graph->get_block<uhd::rfnoc::radio_control>(radio1_ctrl_id);

    std::cout << "Using radio0 " << radio0_ctrl_id << " (gNB port0, UE1 port1)" << std::endl;
    std::cout << "Using radio1 " << radio1_ctrl_id << " (UE2 port0, UE3 port1)" << std::endl;

    size_t mb_idx = radio0_ctrl_id.get_device_no();

    // Create FIR filter controls
    std::array<fir_filter_block_control::sptr, NUM_DL_CHANNELS> fir_dl_ctrl;
    std::array<fir_filter_block_control::sptr, NUM_UL_CHANNELS> fir_ul_ctrl;

    fir_dl_ctrl[0] = graph->get_block<fir_filter_block_control>(uhd::rfnoc::block_id_t(fir_dl0_id));
    fir_dl_ctrl[1] = graph->get_block<fir_filter_block_control>(uhd::rfnoc::block_id_t(fir_dl1_id));
    fir_dl_ctrl[2] = graph->get_block<fir_filter_block_control>(uhd::rfnoc::block_id_t(fir_dl2_id));
    fir_ul_ctrl[0] = graph->get_block<fir_filter_block_control>(uhd::rfnoc::block_id_t(fir_ul0_id));
    fir_ul_ctrl[1] = graph->get_block<fir_filter_block_control>(uhd::rfnoc::block_id_t(fir_ul1_id));
    fir_ul_ctrl[2] = graph->get_block<fir_filter_block_control>(uhd::rfnoc::block_id_t(fir_ul2_id));

    // Create Shiftright block controls
    std::array<shiftright_block_control::sptr, NUM_DL_CHANNELS> shift_dl_ctrl;
    std::array<shiftright_block_control::sptr, NUM_UL_CHANNELS> shift_ul_ctrl;

    shift_dl_ctrl[0] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_dl0_id));
    shift_dl_ctrl[1] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_dl1_id));
    shift_dl_ctrl[2] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_dl2_id));
    shift_ul_ctrl[0] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_ul0_id));
    shift_ul_ctrl[1] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_ul1_id));
    shift_ul_ctrl[2] = graph->get_block<shiftright_block_control>(uhd::rfnoc::block_id_t(shift_ul2_id));

    std::cout << "All RFNoC blocks acquired successfully." << std::endl;

    /************************************************************************
     * Set up static connections (already defined in FPGA image, just commit)
     * Note: Connections are hard-wired in the FPGA image core YAML.
     * We just need to commit the graph.
     ***********************************************************************/
    graph->commit();
    std::cout << "RFNoC graph committed." << std::endl;

    // Enable timestamps on RX if needed
    radio0_ctrl->enable_rx_timestamps(rx_timestamps, 0);
    radio0_ctrl->enable_rx_timestamps(rx_timestamps, 1);
    radio1_ctrl->enable_rx_timestamps(rx_timestamps, 0);
    radio1_ctrl->enable_rx_timestamps(rx_timestamps, 1);

    /************************************************************************
     * Initialize channel emulation blocks
     ***********************************************************************/
    // Set up FIR Filters (default passthrough)
    for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
        fir_dl_ctrl[i]->set_coefficients(default_fir, 0);
    }
    for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
        fir_ul_ctrl[i]->set_coefficients(default_fir, 0);
    }
    std::cout << boost::format("Max FIR taps supported: %d") 
              % fir_dl_ctrl[0]->get_max_num_coefficients() << std::endl;

    // Set up Shiftright blocks (default: no shift = 0 dB attenuation)
    for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
        shift_dl_ctrl[i]->set_shiftright_value(0);
    }
    for (size_t i = 0; i < NUM_UL_CHANNELS; i++) {
        shift_ul_ctrl[i]->set_shiftright_value(0);
    }

    /************************************************************************
     * Set up RF parameters
     ***********************************************************************/
    // Show sample rate
    double rate = radio0_ctrl->get_rate();
    std::cout << boost::format("Sample Rate: %f Msps") % (rate / 1e6) << std::endl;

    // Set center frequencies
    // gNB port (radio0 port 0)
    radio0_ctrl->set_rx_frequency(gnb_freq, 0);
    radio0_ctrl->set_tx_frequency(gnb_freq, 0);
    std::cout << boost::format("gNB (radio0 port0) Freq: %f MHz") 
              % (radio0_ctrl->get_rx_frequency(0) / 1e6) << std::endl;

    // UE1 port (radio0 port 1)
    radio0_ctrl->set_rx_frequency(ue_freq, 1);
    radio0_ctrl->set_tx_frequency(ue_freq, 1);
    std::cout << boost::format("UE1 (radio0 port1) Freq: %f MHz") 
              % (radio0_ctrl->get_rx_frequency(1) / 1e6) << std::endl;

    // UE2 port (radio1 port 0)
    radio1_ctrl->set_rx_frequency(ue_freq, 0);
    radio1_ctrl->set_tx_frequency(ue_freq, 0);
    std::cout << boost::format("UE2 (radio1 port0) Freq: %f MHz") 
              % (radio1_ctrl->get_rx_frequency(0) / 1e6) << std::endl;

    // UE3 port (radio1 port 1)
    radio1_ctrl->set_rx_frequency(ue_freq, 1);
    radio1_ctrl->set_tx_frequency(ue_freq, 1);
    std::cout << boost::format("UE3 (radio1 port1) Freq: %f MHz") 
              % (radio1_ctrl->get_rx_frequency(1) / 1e6) << std::endl;

    // Parse and set per-port RF gains
    // Port order: [0]=gNB, [1]=UE1, [2]=UE2, [3]=UE3
    std::vector<double> rx_gains = parse_gains(rx_gains_str, 4, default_rx_gain);
    std::vector<double> tx_gains = parse_gains(tx_gains_str, 4, default_tx_gain);

    radio0_ctrl->set_rx_gain(rx_gains[0], 0);  // gNB
    radio0_ctrl->set_rx_gain(rx_gains[1], 1);  // UE1
    radio1_ctrl->set_rx_gain(rx_gains[2], 0);  // UE2
    radio1_ctrl->set_rx_gain(rx_gains[3], 1);  // UE3
    std::cout << boost::format("RX Gains (gNB,UE1,UE2,UE3): %.1f, %.1f, %.1f, %.1f dB")
              % rx_gains[0] % rx_gains[1] % rx_gains[2] % rx_gains[3] << std::endl;

    radio0_ctrl->set_tx_gain(tx_gains[0], 0);  // gNB
    radio0_ctrl->set_tx_gain(tx_gains[1], 1);  // UE1
    radio1_ctrl->set_tx_gain(tx_gains[2], 0);  // UE2
    radio1_ctrl->set_tx_gain(tx_gains[3], 1);  // UE3
    std::cout << boost::format("TX Gains (gNB,UE1,UE2,UE3): %.1f, %.1f, %.1f, %.1f dB")
              % tx_gains[0] % tx_gains[1] % tx_gains[2] % tx_gains[3] << std::endl;

    // Set RF bandwidths
    radio0_ctrl->set_rx_bandwidth(rx_bw, 0);
    radio0_ctrl->set_rx_bandwidth(rx_bw, 1);
    radio1_ctrl->set_rx_bandwidth(rx_bw, 0);
    radio1_ctrl->set_rx_bandwidth(rx_bw, 1);
    std::cout << boost::format("RX Bandwidth: %f MHz") % (rx_bw / 1e6) << std::endl;

    radio0_ctrl->set_tx_bandwidth(tx_bw, 0);
    radio0_ctrl->set_tx_bandwidth(tx_bw, 1);
    radio1_ctrl->set_tx_bandwidth(tx_bw, 0);
    radio1_ctrl->set_tx_bandwidth(tx_bw, 1);
    std::cout << boost::format("TX Bandwidth: %f MHz") % (tx_bw / 1e6) << std::endl;

    // Set samples per packet
    radio0_ctrl->set_property<int>("spp", spp, 0);
    radio0_ctrl->set_property<int>("spp", spp, 1);
    radio1_ctrl->set_property<int>("spp", spp, 0);
    radio1_ctrl->set_property<int>("spp", spp, 1);
    spp = radio0_ctrl->get_property<int>("spp", 0);
    std::cout << "Samples per packet: " << spp << std::endl;

    /************************************************************************
     * Start streaming
     ***********************************************************************/
    // Allow for some setup time
    std::this_thread::sleep_for(1s * setup_time);

    // Arm SIGINT handler
    std::signal(SIGINT, &sig_int_handler);

    // Start streaming on all radio channels
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    stream_cmd.stream_now = false;
    stream_cmd.time_spec =
        graph->get_mb_controller(mb_idx)->get_timekeeper(mb_idx)->get_time_now()
        + setup_time;

    std::cout << "Issuing start stream cmd..." << std::endl;
    radio0_ctrl->issue_stream_cmd(stream_cmd, 0);  // gNB RX
    radio0_ctrl->issue_stream_cmd(stream_cmd, 1);  // UE1 RX
    radio1_ctrl->issue_stream_cmd(stream_cmd, 0);  // UE2 RX
    radio1_ctrl->issue_stream_cmd(stream_cmd, 1);  // UE3 RX

    std::cout << std::endl;
    std::cout << "**********************************************************" << std::endl;
    std::cout << "*  OpenAirLink 4-Channel Emulation is Now Running        *" << std::endl;
    std::cout << "*  1 gNB + 3 UE Bidirectional Channel Emulator           *" << std::endl;
    std::cout << "**********************************************************" << std::endl;

    // Print initial channel status
    print_channel_status(fir_dl_ctrl, shift_dl_ctrl, fir_ul_ctrl, shift_ul_ctrl);

    /************************************************************************
     * Channel update loop
     ***********************************************************************/
    std::string fir_str;
    std::string bit_str;
    std::vector<int16_t> fir_coeffs;
    uint32_t bit_shift;

    // Check if script mode
    if (vm.count("script")) {
        use_script = true;
        std::cout << "\nUsing Script Mode..." << std::endl;
    } else {
        std::cout << "\nUsing Manual Mode..." << std::endl;
    }

    double elapsed_time = 0.0;

    if (use_script && is_csv_valid(config_path_script)) {
        config_in.open(config_path_script);

        int step = 0;
        double curr_index;
        std::string index;

        std::getline(config_in, index, ',');
        curr_index = static_cast<double>(std::stod(index));

        std::cout << boost::format("Script starts at elapsed time: %.3fs") % curr_index << std::endl;
        std::cout << "Press Enter to start..." << std::endl;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        while (!stop_signal_called) {
            if (elapsed_time >= curr_index) {
                // Read 6 channels: DL0, DL1, DL2, UL0, UL1, UL2
                // CSV format per line: time, fir_dl0, shift_dl0, fir_dl1, shift_dl1, fir_dl2, shift_dl2, 
                //                             fir_ul0, shift_ul0, fir_ul1, shift_ul1, fir_ul2, shift_ul2

                // Downlink channels
                for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = fir_parser(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(space_trim(bit_str)));
                    fir_dl_ctrl[i]->set_coefficients(fir_coeffs, 0);
                    shift_dl_ctrl[i]->set_shiftright_value(bit_shift);
                }

                // Uplink channels (last one without trailing comma)
                for (size_t i = 0; i < NUM_UL_CHANNELS - 1; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = fir_parser(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(space_trim(bit_str)));
                    fir_ul_ctrl[i]->set_coefficients(fir_coeffs, 0);
                    shift_ul_ctrl[i]->set_shiftright_value(bit_shift);
                }
                // Last channel (newline terminated)
                std::getline(config_in, fir_str, ',');
                std::getline(config_in, bit_str);
                fir_coeffs = fir_parser(fir_str);
                bit_shift = static_cast<uint32_t>(std::stoi(space_trim(bit_str)));
                fir_ul_ctrl[NUM_UL_CHANNELS-1]->set_coefficients(fir_coeffs, 0);
                shift_ul_ctrl[NUM_UL_CHANNELS-1]->set_shiftright_value(bit_shift);

                step++;
                std::cout << std::endl;
                std::cout << boost::format("Script Step: %d   Running Time: %.3fs") % step % elapsed_time << std::endl;
                print_channel_status(fir_dl_ctrl, shift_dl_ctrl, fir_ul_ctrl, shift_ul_ctrl);

                // Get next config index
                std::getline(config_in, index, ',');
                index = space_trim(index);

                if (index.compare("eos") != 0) {
                    curr_index = static_cast<double>(std::stod(index));
                } else {
                    curr_index = std::numeric_limits<double>::infinity();
                    std::cout << "Reached end of Script, keeping current config..." << std::endl;
                }
            }

            std::this_thread::sleep_for(1000ms * scruni_t);
            elapsed_time += scruni_t;
            std::cout << '.' << std::flush;
        }
        config_in.close();
    }
    else {
        if (use_script) {
            std::cout << "Warning: Could not open script config at '" << config_path_script 
                      << "', using manual config instead." << std::endl;
        }

        while (!stop_signal_called) {
            if (is_csv_valid(config_path_manually)) {
                config_in.open(config_path_manually);

                // Read 6 channels from manual config
                // CSV format: fir_dl0, shift_dl0, fir_dl1, shift_dl1, fir_dl2, shift_dl2,
                //             fir_ul0, shift_ul0, fir_ul1, shift_ul1, fir_ul2, shift_ul2

                for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = fir_parser(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(space_trim(bit_str)));
                    fir_dl_ctrl[i]->set_coefficients(fir_coeffs, 0);
                    shift_dl_ctrl[i]->set_shiftright_value(bit_shift);
                }

                for (size_t i = 0; i < NUM_UL_CHANNELS - 1; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = fir_parser(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(space_trim(bit_str)));
                    fir_ul_ctrl[i]->set_coefficients(fir_coeffs, 0);
                    shift_ul_ctrl[i]->set_shiftright_value(bit_shift);
                }
                // Last channel
                std::getline(config_in, fir_str, ',');
                std::getline(config_in, bit_str);
                fir_coeffs = fir_parser(fir_str);
                bit_shift = static_cast<uint32_t>(std::stoi(space_trim(bit_str)));
                fir_ul_ctrl[NUM_UL_CHANNELS-1]->set_coefficients(fir_coeffs, 0);
                shift_ul_ctrl[NUM_UL_CHANNELS-1]->set_shiftright_value(bit_shift);

                config_in.close();
            } else {
                std::cout << "Warning: Could not open config at '" << config_path_manually 
                          << "', using default/previous config." << std::endl;
            }

            std::this_thread::sleep_for(1000ms * update_t);
            elapsed_time += update_t;
            std::cout << '.' << std::flush;

            // Print status periodically
            if (std::fmod(elapsed_time, print_t) == 0) {
                std::cout << std::endl;
                std::cout << boost::format("Running Time: %.1fs") % elapsed_time << std::endl;
                print_channel_status(fir_dl_ctrl, shift_dl_ctrl, fir_ul_ctrl, shift_ul_ctrl);
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
