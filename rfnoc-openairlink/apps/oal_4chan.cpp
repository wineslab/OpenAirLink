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
 * OpenAirLink 4-Channel Bidirectional Channel Emulator (CSV Interface)
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

#include <rfnoc/openairlink/oal_common.hpp>
#include <uhd/utils/safe_main.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <fstream>
#include <thread>
#include <limits>
#include <cmath>

namespace po = boost::program_options;
using namespace rfnoc::openairlink;
using namespace std::chrono_literals;

/****************************************************************************
 * SIGINT handling
 ***************************************************************************/
static bool stop_signal_called = false;
void sig_int_handler(int)
{
    stop_signal_called = true;
}

/****************************************************************************
 * Check CSV file validity
 ***************************************************************************/
static bool is_csv_valid(const std::string& path) {
    std::ifstream target_csv(path);
    return !target_csv.fail();
}

/****************************************************************************
 * main
 ***************************************************************************/
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // Variables to be set by po
    std::string args;
    double gnb_freq, ue_freq, rx_bw, tx_bw, update_t, print_t, scruni_t;
    std::string rx_gains_str, tx_gains_str;
    double default_rx_gain = 0.0, default_tx_gain = 0.0;
    bool use_script = false;

    // Config file paths
    std::string root = CMAKE_SOURCE_DIR;
    std::string config_path_manually = root + "/channel_control/chan_4chan_manually.csv";
    std::string config_path_script   = root + "/channel_control/chan_4chan_script.csv";

    // Setup program options
    po::options_description desc("Allowed options");
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

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << boost::format("OpenAirLink 4-Channel Bidirectional Emulator (CSV Interface)\n%s") % desc << std::endl;
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
     * Initialize RFNoC graph and blocks
     ***********************************************************************/
    EmulatorContext ctx;
    
    if (!init_rfnoc_graph(args, ctx)) {
        std::cerr << "Failed to initialize RFNoC graph" << std::endl;
        return EXIT_FAILURE;
    }

    /************************************************************************
     * Configure RF parameters
     ***********************************************************************/
    RFConfig rf_config;
    rf_config.gnb_freq = gnb_freq;
    rf_config.ue_freq = ue_freq;
    rf_config.rx_bw = rx_bw;
    rf_config.tx_bw = tx_bw;
    
    // Parse gain strings
    auto rx_gains = parse_gains(rx_gains_str, 4, default_rx_gain);
    auto tx_gains = parse_gains(tx_gains_str, 4, default_tx_gain);
    for (size_t i = 0; i < 4; i++) {
        rf_config.rx_gains[i] = rx_gains[i];
        rf_config.tx_gains[i] = tx_gains[i];
    }
    
    configure_rf_params(ctx, rf_config);

    /************************************************************************
     * Initialize channel emulation blocks
     ***********************************************************************/
    init_fir_filters(ctx);
    init_shiftright_blocks(ctx);

    /************************************************************************
     * Start streaming
     ***********************************************************************/
    std::signal(SIGINT, &sig_int_handler);
    start_streaming(ctx, rf_config.setup_time);

    std::cout << std::endl;
    std::cout << "**********************************************************" << std::endl;
    std::cout << "*  OpenAirLink 4-Channel Emulation is Now Running        *" << std::endl;
    std::cout << "*  1 gNB + 3 UE Bidirectional Channel Emulator           *" << std::endl;
    std::cout << "**********************************************************" << std::endl;

    print_channel_status(ctx);

    /************************************************************************
     * Channel update loop (CSV-based)
     ***********************************************************************/
    std::ifstream config_in;
    std::string fir_str, bit_str;
    std::vector<int16_t> fir_coeffs;
    uint32_t bit_shift;

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
        curr_index = std::stod(index);

        std::cout << boost::format("Script starts at elapsed time: %.3fs") % curr_index << std::endl;
        std::cout << "Press Enter to start..." << std::endl;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        while (!stop_signal_called) {
            if (elapsed_time >= curr_index) {
                // Read 6 channels: DL0, DL1, DL2, UL0, UL1, UL2
                // Downlink channels
                for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = parse_fir_coeffs(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(trim_whitespace(bit_str)));
                    set_fir_coefficients(ctx, i, fir_coeffs);
                    set_shiftright_value(ctx, i, bit_shift);
                }

                // Uplink channels (last one without trailing comma)
                for (size_t i = 0; i < NUM_UL_CHANNELS - 1; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = parse_fir_coeffs(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(trim_whitespace(bit_str)));
                    set_fir_coefficients(ctx, NUM_DL_CHANNELS + i, fir_coeffs);
                    set_shiftright_value(ctx, NUM_DL_CHANNELS + i, bit_shift);
                }
                // Last channel (newline terminated)
                std::getline(config_in, fir_str, ',');
                std::getline(config_in, bit_str);
                fir_coeffs = parse_fir_coeffs(fir_str);
                bit_shift = static_cast<uint32_t>(std::stoi(trim_whitespace(bit_str)));
                set_fir_coefficients(ctx, NUM_TOTAL_CHANNELS - 1, fir_coeffs);
                set_shiftright_value(ctx, NUM_TOTAL_CHANNELS - 1, bit_shift);

                step++;
                std::cout << std::endl;
                std::cout << boost::format("Script Step: %d   Running Time: %.3fs") % step % elapsed_time << std::endl;
                print_channel_status(ctx);

                // Get next config index
                std::getline(config_in, index, ',');
                index = trim_whitespace(index);

                if (index != "eos") {
                    curr_index = std::stod(index);
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
                for (size_t i = 0; i < NUM_DL_CHANNELS; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = parse_fir_coeffs(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(trim_whitespace(bit_str)));
                    set_fir_coefficients(ctx, i, fir_coeffs);
                    set_shiftright_value(ctx, i, bit_shift);
                }

                for (size_t i = 0; i < NUM_UL_CHANNELS - 1; i++) {
                    std::getline(config_in, fir_str, ',');
                    std::getline(config_in, bit_str, ',');
                    fir_coeffs = parse_fir_coeffs(fir_str);
                    bit_shift = static_cast<uint32_t>(std::stoi(trim_whitespace(bit_str)));
                    set_fir_coefficients(ctx, NUM_DL_CHANNELS + i, fir_coeffs);
                    set_shiftright_value(ctx, NUM_DL_CHANNELS + i, bit_shift);
                }
                // Last channel
                std::getline(config_in, fir_str, ',');
                std::getline(config_in, bit_str);
                fir_coeffs = parse_fir_coeffs(fir_str);
                bit_shift = static_cast<uint32_t>(std::stoi(trim_whitespace(bit_str)));
                set_fir_coefficients(ctx, NUM_TOTAL_CHANNELS - 1, fir_coeffs);
                set_shiftright_value(ctx, NUM_TOTAL_CHANNELS - 1, bit_shift);

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
                print_channel_status(ctx);
            }
        }
    }

    /************************************************************************
     * Stop streaming
     ***********************************************************************/
    std::cout << std::endl;
    stop_streaming(ctx);
    std::cout << "Done" << std::endl << std::endl;

    return EXIT_SUCCESS;
}
