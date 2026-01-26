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
 * OpenAirLink 4-Channel Bidirectional Channel Emulator (ZMQ/MT Protocol Interface)
 *
 * This application receives MT protocol messages over ZMQ to control the channel emulator,
 * enabling integration with DynScen/Colosseum scenario conductors.
 *
 * Topology (1 gNB + 3 UEs):
 *   gNB  <---> radio0 port 0 (RX: gNB TX, TX: combined UL to gNB)
 *   UE1  <---> radio0 port 1 (RX: UE1 TX, TX: DL to UE1)
 *   UE2  <---> radio1 port 0 (RX: UE2 TX, TX: DL to UE2)
 *   UE3  <---> radio1 port 1 (RX: UE3 TX, TX: DL to UE3)
 *
 * MT Protocol Support:
 *   Receive: MT001 (Radio Config), MT010 (PDP/Coefficient Matrix)
 *   Send:    MT134 (Config Response), MT250 (Status)
 *
 * Channel Mapping (fixed):
 *   Channel 0 = gNB, Channel 2 = UE1, Channel 4 = UE2, Channel 6 = UE3
 *   DL0: gNB(0) -> UE1(2), DL1: gNB(0) -> UE2(4), DL2: gNB(0) -> UE3(6)
 *   UL0: UE1(2) -> gNB(0), UL1: UE2(4) -> gNB(0), UL2: UE3(6) -> gNB(0)
 */

#include <rfnoc/openairlink/oal_common.hpp>
#include <rfnoc/openairlink/mt_protocol.hpp>
#include <uhd/utils/safe_main.hpp>
#include <boost/program_options.hpp>
#include <zmq.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>

namespace po = boost::program_options;
using namespace rfnoc::openairlink;
using namespace rfnoc::openairlink::mt_protocol;
using namespace std::chrono_literals;

/****************************************************************************
 * Global State
 ***************************************************************************/
static std::atomic<bool> stop_signal_called(false);
static std::atomic<uint64_t> total_pdps_received(0);
static std::atomic<uint64_t> scenario_set_count(0);
static std::atomic<uint64_t> msg_counter(0);
static std::mutex ctx_mutex;

// Global emulator context for handlers
static EmulatorContext *g_ctx = nullptr;

/****************************************************************************
 * SIGINT handling
 ***************************************************************************/
void sig_int_handler(int)
{
    stop_signal_called = true;
}

/****************************************************************************
 * Print channel status with ZMQ stats
 ***************************************************************************/
void print_channel_status_zmq(const EmulatorContext &ctx)
{
    print_channel_status(ctx);
    std::cout << boost::format("Total PDPs received: %lu, Scenario set count: %lu") % total_pdps_received.load() % scenario_set_count.load() << std::endl;
}

/****************************************************************************
 * MT001 Handler - Radio Configuration
 ***************************************************************************/
bool handle_mt001(const MT001_Message &msg, void *zmq_mt134_socket)
{
    std::cout << boost::format("[MT001] Radio config received: %d radios") % static_cast<int>(msg.body.num_radios) << std::endl;

    std::lock_guard<std::mutex> lock(ctx_mutex);

    // Process radio configurations
    for (const auto &radio : msg.radios)
    {
        std::cout << boost::format("  Radio %d: RX freq=%.1f MHz, TX freq=%.1f MHz, RX gain=%d, TX gain=%d") % static_cast<int>(radio.radio_id) % radio.rx_center_freq % radio.tx_center_freq % static_cast<int>(radio.rx_channel_gain) % static_cast<int>(radio.tx_channel_gain)
                  << std::endl;

        // Map radio ID to port and apply configuration
        // Channel IDs: 0=gNB (radio0 port0), 2=UE1 (radio0 port1),
        //              4=UE2 (radio1 port0), 6=UE3 (radio1 port1)
        double rx_freq = radio.rx_center_freq * 1e6; // Convert MHz to Hz
        double tx_freq = radio.tx_center_freq * 1e6;
        double rx_gain = radio.rx_channel_gain / 2.0; // Convert from 0.5dB increments
        double tx_gain = radio.tx_channel_gain / 2.0;

        switch (radio.radio_id)
        {
        case 0: // gNB -> radio0 port 0
            g_ctx->radio0->set_rx_frequency(rx_freq, 0);
            g_ctx->radio0->set_tx_frequency(tx_freq, 0);
            g_ctx->radio0->set_rx_gain(rx_gain, 0);
            g_ctx->radio0->set_tx_gain(tx_gain, 0);
            break;
        case 2: // UE1 -> radio0 port 1
            g_ctx->radio0->set_rx_frequency(rx_freq, 1);
            g_ctx->radio0->set_tx_frequency(tx_freq, 1);
            g_ctx->radio0->set_rx_gain(rx_gain, 1);
            g_ctx->radio0->set_tx_gain(tx_gain, 1);
            break;
        case 4: // UE2 -> radio1 port 0
            g_ctx->radio1->set_rx_frequency(rx_freq, 0);
            g_ctx->radio1->set_tx_frequency(tx_freq, 0);
            g_ctx->radio1->set_rx_gain(rx_gain, 0);
            g_ctx->radio1->set_tx_gain(tx_gain, 0);
            break;
        case 6: // UE3 -> radio1 port 1
            g_ctx->radio1->set_rx_frequency(rx_freq, 1);
            g_ctx->radio1->set_tx_frequency(tx_freq, 1);
            g_ctx->radio1->set_rx_gain(rx_gain, 1);
            g_ctx->radio1->set_tx_gain(tx_gain, 1);
            break;
        default:
            std::cout << "  Warning: Unknown radio ID " << static_cast<int>(radio.radio_id) << std::endl;
            break;
        }
    }

    // Send MT134 response
    MT134_Message response;
    response.init(msg.body.num_radios, true, 1, 0, msg_counter++);
    response.radios = msg.radios;

    std::vector<uint8_t> response_data = response.encode();
    zmq_send(zmq_mt134_socket, response_data.data(), response_data.size(), 0);

    std::cout << "[MT134] Config response sent (WILCO)" << std::endl;
    return true;
}

/****************************************************************************
 * MT010 Handler - PDP/Coefficient Matrix
 ***************************************************************************/
bool handle_mt010(const MT010_Message &msg)
{
    // Update counters
    total_pdps_received += msg.body.num_channels_per_packet;
    scenario_set_count = msg.body.scenario_set_count;

    std::lock_guard<std::mutex> lock(ctx_mutex);

    // Process each PDP in the message
    for (const auto &pdp : msg.pdps)
    {
        // Get FIR block index from channel mapping
        int fir_index = channel_to_fir_index(pdp.src_chan, pdp.dst_chan);

        if (fir_index < 0)
        {
            // Skip PDPs that don't match our topology
            continue;
        }

        // Convert PDP to FIR coefficients (41 taps, 5ns spacing)
        std::vector<int16_t> fir_coeffs;
        pdp_to_fir_coeffs(pdp, fir_coeffs);

        // Apply to appropriate FIR block
        set_fir_coefficients(*g_ctx, fir_index, fir_coeffs);
    }

    return true;
}

/****************************************************************************
 * Status Thread - Periodic MT250 broadcast
 ***************************************************************************/
void status_thread_func(void *zmq_mt250_socket, int interval_ms)
{
    MT250_Message status;
    status.init(1, 0, 0); // src=1 (OpenAirLink), dst=0 (DynScen)

    while (!stop_signal_called)
    {
        // Update status
        status.update(total_pdps_received.load(), scenario_set_count.load());
        status.header.message_counter = msg_counter++;

        // Encode and send
        std::vector<uint8_t> status_data = status.encode();
        zmq_send(zmq_mt250_socket, status_data.data(), status_data.size(), 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

/****************************************************************************
 * main
 ***************************************************************************/
int UHD_SAFE_MAIN(int argc, char *argv[])
{
    // Variables to be set by po
    std::string args;
    double gnb_freq, ue_freq, rx_bw, tx_bw;
    std::string zmq_mt001_addr, zmq_mt010_addr, zmq_mt134_addr, zmq_mt250_addr;
    int status_interval_ms;
    double print_interval;

    // Setup program options
    po::options_description desc("Allowed options");
    desc.add_options()("help", "help message")("args", po::value<std::string>(&args)->default_value(""), "UHD device address args")("gnb-freq", po::value<double>(&gnb_freq)->default_value(3619.2e6), "gNB RF center frequency in Hz")("ue-freq", po::value<double>(&ue_freq)->default_value(3619.2e6), "UE RF center frequency in Hz")("rx-bw", po::value<double>(&rx_bw)->default_value(100e6), "RX analog frontend filter bandwidth in Hz")("tx-bw", po::value<double>(&tx_bw)->default_value(100e6), "TX analog frontend filter bandwidth in Hz")("zmq-mt001", po::value<std::string>(&zmq_mt001_addr)->default_value("tcp://0.0.0.0:5001"),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       "ZMQ PULL address for MT001 messages")("zmq-mt010", po::value<std::string>(&zmq_mt010_addr)->default_value("tcp://0.0.0.0:5002"),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              "ZMQ PULL address for MT010 messages")("zmq-mt134", po::value<std::string>(&zmq_mt134_addr)->default_value("tcp://0.0.0.0:6004"),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     "ZMQ PUSH address for MT134 responses")("zmq-mt250", po::value<std::string>(&zmq_mt250_addr)->default_value("tcp://0.0.0.0:6005"),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             "ZMQ PUSH address for MT250 status")("status-interval", po::value<int>(&status_interval_ms)->default_value(1000),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  "MT250 status broadcast interval in ms")("print-interval", po::value<double>(&print_interval)->default_value(5.0),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           "Channel status print interval in seconds");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        std::cout << boost::format("OpenAirLink 4-Channel ZMQ/MT Protocol Emulator\n%s") % desc << std::endl;
        std::cout
            << std::endl
            << "This application runs a 1 gNB + 3 UE bidirectional channel emulator\n"
            << "controlled via ZMQ using the MT protocol (MT001, MT010, MT134, MT250).\n"
            << "\nTopology:\n"
            << "  gNB  <---> radio0 port 0  (Channel ID: 0)\n"
            << "  UE1  <---> radio0 port 1  (Channel ID: 2)\n"
            << "  UE2  <---> radio1 port 0  (Channel ID: 4)\n"
            << "  UE3  <---> radio1 port 1  (Channel ID: 6)\n"
            << "\nChannel Mapping:\n"
            << "  DL0: gNB(0) -> UE1(2)  -> FIR#0\n"
            << "  DL1: gNB(0) -> UE2(4)  -> FIR#1\n"
            << "  DL2: gNB(0) -> UE3(6)  -> FIR#2\n"
            << "  UL0: UE1(2) -> gNB(0)  -> FIR#3\n"
            << "  UL1: UE2(4) -> gNB(0)  -> FIR#4\n"
            << "  UL2: UE3(6) -> gNB(0)  -> FIR#5\n"
            << std::endl;
        return ~0;
    }

    /************************************************************************
     * Initialize ZMQ
     ***********************************************************************/
    std::cout << "Initializing ZMQ sockets..." << std::endl;

    void *zmq_context = zmq_ctx_new();

    // MT001 PULL socket (radio config)
    void *zmq_mt001_socket = zmq_socket(zmq_context, ZMQ_PULL);
    if (zmq_bind(zmq_mt001_socket, zmq_mt001_addr.c_str()) != 0)
    {
        std::cerr << "Failed to bind MT001 socket to " << zmq_mt001_addr << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "  MT001 (Radio Config) listening on: " << zmq_mt001_addr << std::endl;

    // MT010 PULL socket (PDP)
    void *zmq_mt010_socket = zmq_socket(zmq_context, ZMQ_PULL);
    if (zmq_bind(zmq_mt010_socket, zmq_mt010_addr.c_str()) != 0)
    {
        std::cerr << "Failed to bind MT010 socket to " << zmq_mt010_addr << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "  MT010 (PDP) listening on: " << zmq_mt010_addr << std::endl;

    // MT134 PUSH socket (config response)
    void *zmq_mt134_socket = zmq_socket(zmq_context, ZMQ_PUSH);
    if (zmq_bind(zmq_mt134_socket, zmq_mt134_addr.c_str()) != 0)
    {
        std::cerr << "Failed to bind MT134 socket to " << zmq_mt134_addr << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "  MT134 (Config Response) sending on: " << zmq_mt134_addr << std::endl;

    // MT250 PUSH socket (status)
    void *zmq_mt250_socket = zmq_socket(zmq_context, ZMQ_PUSH);
    if (zmq_bind(zmq_mt250_socket, zmq_mt250_addr.c_str()) != 0)
    {
        std::cerr << "Failed to bind MT250 socket to " << zmq_mt250_addr << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "  MT250 (Status) sending on: " << zmq_mt250_addr << std::endl;

    /************************************************************************
     * Initialize RFNoC graph and blocks
     ***********************************************************************/
    EmulatorContext ctx;

    if (!init_rfnoc_graph(args, ctx))
    {
        std::cerr << "Failed to initialize RFNoC graph" << std::endl;
        return EXIT_FAILURE;
    }

    // Set global context pointer for handlers
    g_ctx = &ctx;

    /************************************************************************
     * Configure RF parameters
     ***********************************************************************/
    RFConfig rf_config;
    rf_config.gnb_freq = gnb_freq;
    rf_config.ue_freq = ue_freq;
    rf_config.rx_bw = rx_bw;
    rf_config.tx_bw = tx_bw;

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
    std::cout << "*  OpenAirLink 4-Channel ZMQ/MT Emulator Running         *" << std::endl;
    std::cout << "*  Waiting for MT001/MT010 messages...                   *" << std::endl;
    std::cout << "**********************************************************" << std::endl;

    print_channel_status_zmq(ctx);

    /************************************************************************
     * Start status thread
     ***********************************************************************/
    std::thread status_thread(status_thread_func, zmq_mt250_socket, status_interval_ms);

    /************************************************************************
     * Main ZMQ receiver loop
     ***********************************************************************/
    std::vector<uint8_t> recv_buffer(MAX_PACKET_SIZE);
    zmq_pollitem_t poll_items[] = {
        {zmq_mt001_socket, 0, ZMQ_POLLIN, 0},
        {zmq_mt010_socket, 0, ZMQ_POLLIN, 0}};

    auto last_print_time = std::chrono::steady_clock::now();
    uint64_t mt010_count = 0;

    while (!stop_signal_called)
    {
        // Poll for messages with 100ms timeout
        int rc = zmq_poll(poll_items, 2, 100);

        if (rc < 0)
        {
            if (errno == EINTR)
                continue; // Interrupted by signal
            std::cerr << "ZMQ poll error: " << zmq_strerror(errno) << std::endl;
            break;
        }

        // Check MT001 socket
        if (poll_items[0].revents & ZMQ_POLLIN)
        {
            int nbytes = zmq_recv(zmq_mt001_socket, recv_buffer.data(), recv_buffer.size(), 0);
            if (nbytes > 0)
            {
                MT001_Message mt001;
                if (mt001.decode(recv_buffer.data(), nbytes))
                {
                    handle_mt001(mt001, zmq_mt134_socket);
                }
                else
                {
                    std::cerr << "Failed to decode MT001 message" << std::endl;
                }
            }
        }

        // Check MT010 socket
        if (poll_items[1].revents & ZMQ_POLLIN)
        {
            int nbytes = zmq_recv(zmq_mt010_socket, recv_buffer.data(), recv_buffer.size(), 0);
            if (nbytes > 0)
            {
                MT010_Message mt010;
                if (mt010.decode(recv_buffer.data(), nbytes))
                {
                    handle_mt010(mt010);
                    mt010_count++;
                }
                else
                {
                    std::cerr << "Failed to decode MT010 message" << std::endl;
                }
            }
        }

        // Periodic status print
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_print_time).count() >= print_interval)
        {
            std::cout << boost::format("\n[Status] MT010 messages processed: %lu") % mt010_count << std::endl;
            print_channel_status_zmq(ctx);
            last_print_time = now;
        }
    }

    /************************************************************************
     * Cleanup
     ***********************************************************************/
    std::cout << std::endl
              << "Shutting down..." << std::endl;

    // Wait for status thread
    status_thread.join();

    // Stop streaming
    stop_streaming(ctx);

    // Cleanup ZMQ
    zmq_close(zmq_mt001_socket);
    zmq_close(zmq_mt010_socket);
    zmq_close(zmq_mt134_socket);
    zmq_close(zmq_mt250_socket);
    zmq_ctx_destroy(zmq_context);

    std::cout << "Done" << std::endl
              << std::endl;

    return EXIT_SUCCESS;
}
