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

#include <rfnoc/openairlink/oal_common.hpp>
#include <thread>
#include <sstream>
#include <algorithm>
#include <cctype>

using namespace std::chrono_literals;

namespace rfnoc
{
    namespace openairlink
    {

        //=============================================================================
        // RFNoC Graph Initialization
        //=============================================================================

        bool init_rfnoc_graph(const std::string &args, EmulatorContext &ctx, const BlockIds &block_ids)
        {
            std::cout << boost::format("Creating the RFNoC graph with args: %s...") % args << std::endl;

            try
            {
                // Create RFNoC graph
                ctx.graph = uhd::rfnoc::rfnoc_graph::make(args);

                // Create handles for radio objects
                uhd::rfnoc::block_id_t radio0_id(block_ids.radio0);
                uhd::rfnoc::block_id_t radio1_id(block_ids.radio1);

                ctx.radio0 = ctx.graph->get_block<uhd::rfnoc::radio_control>(radio0_id);
                ctx.radio1 = ctx.graph->get_block<uhd::rfnoc::radio_control>(radio1_id);

                std::cout << "Using radio0 " << radio0_id << " (gNB port0, UE1 port1)" << std::endl;
                std::cout << "Using radio1 " << radio1_id << " (UE2 port0, UE3 port1)" << std::endl;

                ctx.mb_idx = radio0_id.get_device_no();

                // Create FIR filter controls
                for (size_t i = 0; i < NUM_DL_CHANNELS; i++)
                {
                    ctx.fir_dl[i] = ctx.graph->get_block<uhd::rfnoc::fir_filter_block_control>(
                        uhd::rfnoc::block_id_t(block_ids.fir_dl[i]));
                }
                for (size_t i = 0; i < NUM_UL_CHANNELS; i++)
                {
                    ctx.fir_ul[i] = ctx.graph->get_block<uhd::rfnoc::fir_filter_block_control>(
                        uhd::rfnoc::block_id_t(block_ids.fir_ul[i]));
                }

                // Create Shiftright block controls
                for (size_t i = 0; i < NUM_DL_CHANNELS; i++)
                {
                    ctx.shift_dl[i] = ctx.graph->get_block<shiftright_block_control>(
                        uhd::rfnoc::block_id_t(block_ids.shift_dl[i]));
                }
                for (size_t i = 0; i < NUM_UL_CHANNELS; i++)
                {
                    ctx.shift_ul[i] = ctx.graph->get_block<shiftright_block_control>(
                        uhd::rfnoc::block_id_t(block_ids.shift_ul[i]));
                }

                std::cout << "All RFNoC blocks acquired successfully." << std::endl;

                // Commit graph
                ctx.graph->commit();
                std::cout << "RFNoC graph committed." << std::endl;

                return true;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error initializing RFNoC graph: " << e.what() << std::endl;
                return false;
            }
        }

        //=============================================================================
        // RF Configuration
        //=============================================================================

        void configure_rf_params(EmulatorContext &ctx, const RFConfig &config)
        {
            // Enable/disable timestamps
            ctx.radio0->enable_rx_timestamps(config.rx_timestamps, 0);
            ctx.radio0->enable_rx_timestamps(config.rx_timestamps, 1);
            ctx.radio1->enable_rx_timestamps(config.rx_timestamps, 0);
            ctx.radio1->enable_rx_timestamps(config.rx_timestamps, 1);

            // Show sample rate
            double rate = ctx.radio0->get_rate();
            std::cout << boost::format("Sample Rate: %f Msps") % (rate / 1e6) << std::endl;

            // Set center frequencies
            // gNB port (radio0 port 0)
            ctx.radio0->set_rx_frequency(config.gnb_freq, 0);
            ctx.radio0->set_tx_frequency(config.gnb_freq, 0);
            std::cout << boost::format("gNB (radio0 port0) Freq: %f MHz") % (ctx.radio0->get_rx_frequency(0) / 1e6) << std::endl;

            // UE1 port (radio0 port 1)
            ctx.radio0->set_rx_frequency(config.ue_freq, 1);
            ctx.radio0->set_tx_frequency(config.ue_freq, 1);
            std::cout << boost::format("UE1 (radio0 port1) Freq: %f MHz") % (ctx.radio0->get_rx_frequency(1) / 1e6) << std::endl;

            // UE2 port (radio1 port 0)
            ctx.radio1->set_rx_frequency(config.ue_freq, 0);
            ctx.radio1->set_tx_frequency(config.ue_freq, 0);
            std::cout << boost::format("UE2 (radio1 port0) Freq: %f MHz") % (ctx.radio1->get_rx_frequency(0) / 1e6) << std::endl;

            // UE3 port (radio1 port 1)
            ctx.radio1->set_rx_frequency(config.ue_freq, 1);
            ctx.radio1->set_tx_frequency(config.ue_freq, 1);
            std::cout << boost::format("UE3 (radio1 port1) Freq: %f MHz") % (ctx.radio1->get_rx_frequency(1) / 1e6) << std::endl;

            // Set per-port RF gains
            ctx.radio0->set_rx_gain(config.rx_gains[0], 0); // gNB
            ctx.radio0->set_rx_gain(config.rx_gains[1], 1); // UE1
            ctx.radio1->set_rx_gain(config.rx_gains[2], 0); // UE2
            ctx.radio1->set_rx_gain(config.rx_gains[3], 1); // UE3
            std::cout << boost::format("RX Gains (gNB,UE1,UE2,UE3): %.1f, %.1f, %.1f, %.1f dB") % config.rx_gains[0] % config.rx_gains[1] % config.rx_gains[2] % config.rx_gains[3] << std::endl;

            ctx.radio0->set_tx_gain(config.tx_gains[0], 0); // gNB
            ctx.radio0->set_tx_gain(config.tx_gains[1], 1); // UE1
            ctx.radio1->set_tx_gain(config.tx_gains[2], 0); // UE2
            ctx.radio1->set_tx_gain(config.tx_gains[3], 1); // UE3
            std::cout << boost::format("TX Gains (gNB,UE1,UE2,UE3): %.1f, %.1f, %.1f, %.1f dB") % config.tx_gains[0] % config.tx_gains[1] % config.tx_gains[2] % config.tx_gains[3] << std::endl;

            // Set RF bandwidths
            ctx.radio0->set_rx_bandwidth(config.rx_bw, 0);
            ctx.radio0->set_rx_bandwidth(config.rx_bw, 1);
            ctx.radio1->set_rx_bandwidth(config.rx_bw, 0);
            ctx.radio1->set_rx_bandwidth(config.rx_bw, 1);
            std::cout << boost::format("RX Bandwidth: %f MHz") % (config.rx_bw / 1e6) << std::endl;

            ctx.radio0->set_tx_bandwidth(config.tx_bw, 0);
            ctx.radio0->set_tx_bandwidth(config.tx_bw, 1);
            ctx.radio1->set_tx_bandwidth(config.tx_bw, 0);
            ctx.radio1->set_tx_bandwidth(config.tx_bw, 1);
            std::cout << boost::format("TX Bandwidth: %f MHz") % (config.tx_bw / 1e6) << std::endl;

            // Set samples per packet
            ctx.radio0->set_property<int>("spp", config.spp, 0);
            ctx.radio0->set_property<int>("spp", config.spp, 1);
            ctx.radio1->set_property<int>("spp", config.spp, 0);
            ctx.radio1->set_property<int>("spp", config.spp, 1);
            std::cout << "Samples per packet: " << ctx.radio0->get_property<int>("spp", 0) << std::endl;
        }

        //=============================================================================
        // Channel Emulation Initialization
        //=============================================================================

        void init_fir_filters(EmulatorContext &ctx)
        {
            // Set up FIR Filters with default passthrough
            for (size_t i = 0; i < NUM_DL_CHANNELS; i++)
            {
                ctx.fir_dl[i]->set_coefficients(ctx.default_fir, 0);
            }
            for (size_t i = 0; i < NUM_UL_CHANNELS; i++)
            {
                ctx.fir_ul[i]->set_coefficients(ctx.default_fir, 0);
            }

            std::cout << boost::format("FIR filters initialized: %d taps (5ns spacing, 200ns max delay)") % NUM_FIR_TAPS << std::endl;
            std::cout << boost::format("Max FIR taps supported: %d") % ctx.fir_dl[0]->get_max_num_coefficients() << std::endl;
        }

        void init_shiftright_blocks(EmulatorContext &ctx)
        {
            // Set up Shiftright blocks with no attenuation
            for (size_t i = 0; i < NUM_DL_CHANNELS; i++)
            {
                ctx.shift_dl[i]->set_shiftright_value(0);
            }
            for (size_t i = 0; i < NUM_UL_CHANNELS; i++)
            {
                ctx.shift_ul[i]->set_shiftright_value(0);
            }

            std::cout << "Shiftright blocks initialized (shift=0, no attenuation)" << std::endl;
        }

        //=============================================================================
        // Streaming Control
        //=============================================================================

        void start_streaming(EmulatorContext &ctx, double setup_time)
        {
            std::this_thread::sleep_for(1s * setup_time);

            uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
            stream_cmd.stream_now = false;
            stream_cmd.time_spec =
                ctx.graph->get_mb_controller(ctx.mb_idx)->get_timekeeper(ctx.mb_idx)->get_time_now() + setup_time;

            std::cout << "Issuing start stream cmd..." << std::endl;
            ctx.radio0->issue_stream_cmd(stream_cmd, 0); // gNB RX
            ctx.radio0->issue_stream_cmd(stream_cmd, 1); // UE1 RX
            ctx.radio1->issue_stream_cmd(stream_cmd, 0); // UE2 RX
            ctx.radio1->issue_stream_cmd(stream_cmd, 1); // UE3 RX
        }

        void stop_streaming(EmulatorContext &ctx)
        {
            uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);

            std::cout << "Issuing stop stream cmd..." << std::endl;
            ctx.radio0->issue_stream_cmd(stream_cmd, 0);
            ctx.radio0->issue_stream_cmd(stream_cmd, 1);
            ctx.radio1->issue_stream_cmd(stream_cmd, 0);
            ctx.radio1->issue_stream_cmd(stream_cmd, 1);

            std::this_thread::sleep_for(100ms);
        }

        //=============================================================================
        // Channel Control
        //=============================================================================

        void set_fir_coefficients(EmulatorContext &ctx, size_t channel_index, const std::vector<int16_t> &coeffs)
        {
            if (channel_index < NUM_DL_CHANNELS)
            {
                ctx.fir_dl[channel_index]->set_coefficients(coeffs, 0);
            }
            else if (channel_index < NUM_TOTAL_CHANNELS)
            {
                ctx.fir_ul[channel_index - NUM_DL_CHANNELS]->set_coefficients(coeffs, 0);
            }
        }

        void set_shiftright_value(EmulatorContext &ctx, size_t channel_index, uint32_t shift)
        {
            if (channel_index < NUM_DL_CHANNELS)
            {
                ctx.shift_dl[channel_index]->set_shiftright_value(shift);
            }
            else if (channel_index < NUM_TOTAL_CHANNELS)
            {
                ctx.shift_ul[channel_index - NUM_DL_CHANNELS]->set_shiftright_value(shift);
            }
        }

        //=============================================================================
        // Status Display
        //=============================================================================

        void print_channel_status(const EmulatorContext &ctx)
        {
            std::vector<int16_t> coeffs;
            uint32_t shift_val;

            std::cout << "\n=== Downlink Channels (gNB -> UEs) ===" << std::endl;
            for (size_t i = 0; i < NUM_DL_CHANNELS; i++)
            {
                shift_val = ctx.shift_dl[i]->get_shiftright_value();
                coeffs = ctx.fir_dl[i]->get_coefficients();
                std::cout << boost::format("  DL%d (gNB->UE%d): shift=%2d, FIR=") % i % (i + 1) % shift_val;
                for (size_t j = 0; j < std::min(coeffs.size(), size_t(5)); j++)
                {
                    std::cout << coeffs[j] << " ";
                }
                if (coeffs.size() > 5)
                    std::cout << "...";
                std::cout << std::endl;
            }

            std::cout << "=== Uplink Channels (UEs -> gNB) ===" << std::endl;
            for (size_t i = 0; i < NUM_UL_CHANNELS; i++)
            {
                shift_val = ctx.shift_ul[i]->get_shiftright_value();
                coeffs = ctx.fir_ul[i]->get_coefficients();
                std::cout << boost::format("  UL%d (UE%d->gNB): shift=%2d, FIR=") % i % (i + 1) % shift_val;
                for (size_t j = 0; j < std::min(coeffs.size(), size_t(5)); j++)
                {
                    std::cout << coeffs[j] << " ";
                }
                if (coeffs.size() > 5)
                    std::cout << "...";
                std::cout << std::endl;
            }
        }

        //=============================================================================
        // Utility Functions
        //=============================================================================

        std::vector<double> parse_gains(const std::string &input, size_t expected_count, double default_val)
        {
            std::vector<double> gains;
            if (input.empty())
            {
                gains.assign(expected_count, default_val);
                return gains;
            }

            std::istringstream iss(input);
            std::string token;
            while (std::getline(iss, token, ','))
            {
                gains.push_back(std::stod(trim_whitespace(token)));
            }

            // Pad with default value if fewer values provided
            while (gains.size() < expected_count)
            {
                gains.push_back(default_val);
            }

            return gains;
        }

        std::vector<int16_t> parse_fir_coeffs(const std::string &input)
        {
            std::istringstream iss(input);
            std::vector<int16_t> fir_coeffs;
            int temp;

            while (iss >> temp)
            {
                fir_coeffs.push_back(static_cast<int16_t>(temp));
            }

            return fir_coeffs;
        }

        std::string trim_whitespace(const std::string &str)
        {
            std::string out = str;
            out.erase(out.begin(), std::find_if(out.begin(), out.end(), [](unsigned char ch)
                                                { return !std::isspace(ch); }));
            out.erase(std::find_if(out.rbegin(), out.rend(), [](unsigned char ch)
                                   { return !std::isspace(ch); })
                          .base(),
                      out.end());
            return out;
        }

    }
} // namespace rfnoc::openairlink
