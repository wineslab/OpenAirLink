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

#include <rfnoc/openairlink/mt_protocol.hpp>
#include <algorithm>
#include <chrono>
#include <boost/log/trivial.hpp>

namespace rfnoc
{
    namespace openairlink
    {
        namespace mt_protocol
        {

            //=============================================================================
            // Helper Functions
            //=============================================================================

            static inline void copy_from_buffer(void *dest, const uint8_t *src, size_t count, size_t &offset)
            {
                std::memcpy(dest, src + offset, count);
                offset += count;
            }

            static inline void copy_to_buffer(uint8_t *dest, const void *src, size_t count, size_t &offset)
            {
                std::memcpy(dest + offset, src, count);
                offset += count;
            }

            static uint64_t get_current_ntp_time()
            {
                auto now = std::chrono::system_clock::now();
                uint64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                       now.time_since_epoch())
                                       .count() +
                                   2208988800UL; // Unix to NTP epoch
                uint64_t frac_sec = std::chrono::duration_cast<std::chrono::microseconds>(
                                        now.time_since_epoch())
                                        .count() -
                                    (seconds - 2208988800UL) * 1000000;
                frac_sec = static_cast<uint64_t>(static_cast<double>(frac_sec) * 4294.967295);
                return (seconds << 32) | static_cast<uint32_t>(frac_sec);
            }

            //=============================================================================
            // Utility Functions
            //=============================================================================

            uint8_t get_message_type(const uint8_t *data, size_t size)
            {
                if (data == nullptr || size < 1)
                {
                    return 255; // Invalid/undefined
                }
                return data[0];
            }

            void pdp_to_fir_coeffs(const col_filter_t &pdp, std::vector<int16_t> &fir_coeffs)
            {
                // The dynscen client sends all 41 taps directly
                // Simply extract the real part of each coefficient (imaginary is ignored)
                fir_coeffs.resize(NUM_FIR_TAPS);

                for (size_t i = 0; i < NUM_FIR_TAPS; i++)
                {
                    // Use the real part directly as int16_t
                    // The coefficients are in Q15 format (uint16_t), interpret as signed
                    fir_coeffs[i] = static_cast<int16_t>(pdp.coeff_real[i]);
                }
            }

            int channel_to_fir_index(uint16_t src_chan, uint16_t dst_chan)
            {
                // Fixed mapping for 1 gNB + 3 UE topology
                // Channel IDs: 0=gNB, 2=UE1, 4=UE2, 6=UE3 (channel_id = node_index * 2)

                // Downlink: gNB (0) -> UEs
                if (src_chan == 0)
                {
                    switch (dst_chan)
                    {
                    case 2:
                        return 0; // DL0: gNB -> UE1, FIR#0
                    case 4:
                        return 1; // DL1: gNB -> UE2, FIR#1
                    case 6:
                        return 2; // DL2: gNB -> UE3, FIR#2
                    default:
                        return -1;
                    }
                }

                // Uplink: UEs -> gNB (0)
                if (dst_chan == 0)
                {
                    switch (src_chan)
                    {
                    case 2:
                        return 3; // UL0: UE1 -> gNB, FIR#3
                    case 4:
                        return 4; // UL1: UE2 -> gNB, FIR#4
                    case 6:
                        return 5; // UL2: UE3 -> gNB, FIR#5
                    default:
                        return -1;
                    }
                }

                return -1; // Invalid channel combination
            }

            //=============================================================================
            // MT001_Message Implementation
            //=============================================================================

            bool MT001_Message::decode(const uint8_t *data, size_t size)
            {
                if (data == nullptr || size < COMMON_HEADER_SIZE + sizeof(mt001_header_t))
                {
                    BOOST_LOG_TRIVIAL(warning) << "[MT001] Decode failed: size=" << size
                                               << " < minimum=" << (COMMON_HEADER_SIZE + sizeof(mt001_header_t));
                    return false;
                }

                // Check message type
                if (data[0] != recv_message::radio_config)
                {
                    BOOST_LOG_TRIVIAL(warning) << "[MT001] Decode failed: unexpected message type="
                                               << static_cast<int>(data[0]);
                    return false;
                }

                // Decode common header
                size_t offset = 0;
                copy_from_buffer(&header, data, sizeof(common_header_t), offset);

                // Decode MT001 header
                copy_from_buffer(&body, data, sizeof(mt001_header_t), offset);

                // Validate size
                size_t expected_size = COMMON_HEADER_SIZE + sizeof(mt001_header_t) +
                                       body.num_radios * sizeof(radio_data_t);
                if (size < expected_size)
                {
                    BOOST_LOG_TRIVIAL(warning) << "[MT001] Decode failed: size=" << size
                                               << " < expected=" << expected_size
                                               << " (num_radios=" << static_cast<int>(body.num_radios) << ")";
                    return false;
                }

                // Decode radio data
                radios.clear();
                radios.resize(body.num_radios);
                for (size_t i = 0; i < body.num_radios; i++)
                {
                    copy_from_buffer(&radios[i], data, sizeof(radio_data_t), offset);
                }

                BOOST_LOG_TRIVIAL(trace) << "[MT001] Header: type=" << static_cast<int>(header.message_type)
                                         << " src=" << static_cast<int>(header.src)
                                         << " dst=" << static_cast<int>(header.dst)
                                         << " total_size=" << header.total_size
                                         << " seconds=" << header.seconds
                                         << " msg_counter=" << header.message_counter;
                BOOST_LOG_TRIVIAL(trace) << "[MT001] Body: num_radios=" << static_cast<int>(body.num_radios)
                                         << " reservation_id=" << body.reservation_id;
                for (const auto &radio : radios)
                {
                    BOOST_LOG_TRIVIAL(trace) << "[MT001] Radio: id=" << static_cast<int>(radio.radio_id)
                                             << " rx_gain=" << static_cast<int>(radio.rx_channel_gain)
                                             << " tx_gain=" << static_cast<int>(radio.tx_channel_gain)
                                             << " rx_freq_mhz=" << radio.rx_center_freq
                                             << " tx_freq_mhz=" << radio.tx_center_freq;
                }

                return true;
            }

            const radio_data_t *MT001_Message::get_radio(uint8_t radio_id) const
            {
                for (const auto &radio : radios)
                {
                    if (radio.radio_id == radio_id)
                    {
                        return &radio;
                    }
                }
                return nullptr;
            }

            //=============================================================================
            // MT010_Message Implementation
            //=============================================================================

            bool MT010_Message::decode(const uint8_t *data, size_t size)
            {
                if (data == nullptr || size < COMMON_HEADER_SIZE + sizeof(mt010_header_t))
                {
                    BOOST_LOG_TRIVIAL(warning) << "[MT010] Decode failed: size=" << size
                                               << " < minimum=" << (COMMON_HEADER_SIZE + sizeof(mt010_header_t));
                    return false;
                }

                // Check message type
                if (data[0] != recv_message::coeff_matrix_req)
                {
                    BOOST_LOG_TRIVIAL(warning) << "[MT010] Decode failed: unexpected message type="
                                               << static_cast<int>(data[0]);
                    return false;
                }

                // Decode common header
                size_t offset = 0;
                copy_from_buffer(&header, data, sizeof(common_header_t), offset);
                
                BOOST_LOG_TRIVIAL(trace) << "[MT010] Header: type=" << static_cast<int>(header.message_type)
                                         << " src=" << static_cast<int>(header.src)
                                         << " dst=" << static_cast<int>(header.dst)
                                         << " total_size=" << header.total_size
                                         << " seconds=" << header.seconds
                                         << " msg_counter=" << header.message_counter;

                // Decode MT010 header
                copy_from_buffer(&body, data, sizeof(mt010_header_t), offset);
                
                BOOST_LOG_TRIVIAL(trace) << "[MT010] Body: tap_app_seconds=" << body.tap_app_seconds
                                         << " num_channels=" << body.num_channels_per_packet
                                         << " packets_per_update=" << static_cast<int>(body.packets_per_update)
                                         << " scenario_set_count=" << body.scenario_set_count;

                // Validate size
                size_t expected_size = COMMON_HEADER_SIZE + sizeof(mt010_header_t) +
                                       body.num_channels_per_packet * sizeof(col_filter_t);
                if (size < expected_size)
                {
                    BOOST_LOG_TRIVIAL(warning) << "[MT010] Decode failed: size=" << size
                                               << " < expected=" << expected_size
                                               << " (channels=" << body.num_channels_per_packet << ")";
                    return false;
                }

                // Decode PDPs
                pdps.clear();
                pdps.resize(body.num_channels_per_packet);
                for (size_t i = 0; i < body.num_channels_per_packet; i++)
                {
                    copy_from_buffer(&pdps[i], data, sizeof(col_filter_t), offset);
                }

                

                for (const auto &pdp : pdps)
                {
                    BOOST_LOG_TRIVIAL(trace) << "[MT010] PDP: src=" << pdp.src_chan
                                             << " dst=" << pdp.dst_chan
                                             << " coeff_real[0]=" << static_cast<int16_t>(pdp.coeff_real[0])
                                             << " coeff_real[1]=" << static_cast<int16_t>(pdp.coeff_real[1])
                                             << " coeff_real[2]=" << static_cast<int16_t>(pdp.coeff_real[2]);
                }

                return true;
            }

            const col_filter_t *MT010_Message::find_pdp(uint16_t src_chan, uint16_t dst_chan) const
            {
                for (const auto &pdp : pdps)
                {
                    if (pdp.src_chan == src_chan && pdp.dst_chan == dst_chan)
                    {
                        return &pdp;
                    }
                }
                return nullptr;
            }

            //=============================================================================
            // MT134_Message Implementation
            //=============================================================================

            void MT134_Message::init(uint8_t num_radios, bool wilco, uint8_t src, uint8_t dst, uint64_t msg_counter)
            {
                // Initialize header
                header.message_type = send_message::radio_config_resp;
                header.spare = 0;
                header.src = src;
                header.dst = dst;
                header.total_size = sizeof(mt134_header_t) + num_radios * sizeof(radio_data_t);
                header.seconds = get_current_ntp_time();
                header.message_counter = msg_counter;

                // Initialize body
                body.num_radios = num_radios;
                body.wilco_cantco_val = wilco ? 1 : 0;
                body.spare_0 = 0;

                // Clear radios (can be populated later)
                radios.clear();
            }

            std::vector<uint8_t> MT134_Message::encode() const
            {
                size_t body_size = sizeof(mt134_header_t) + radios.size() * sizeof(radio_data_t);
                std::vector<uint8_t> packet(COMMON_HEADER_SIZE + body_size, 0);

                size_t offset = 0;

                // Encode header
                copy_to_buffer(packet.data(), &header, sizeof(common_header_t), offset);

                // Encode MT134 body
                copy_to_buffer(packet.data(), &body, sizeof(mt134_header_t), offset);

                // Encode radio data
                for (const auto &radio : radios)
                {
                    copy_to_buffer(packet.data(), &radio, sizeof(radio_data_t), offset);
                }

                return packet;
            }

            //=============================================================================
            // MT250_Message Implementation
            //=============================================================================

            void MT250_Message::init(uint8_t src, uint8_t dst, uint64_t msg_counter)
            {
                // Initialize header
                header.message_type = send_message::status;
                header.spare = 0;
                header.src = src;
                header.dst = dst;
                header.total_size = sizeof(mt250_body_t);
                header.seconds = get_current_ntp_time();
                header.message_counter = msg_counter;

                // Initialize body with default values
                body.col_state = 3;          // Running state
                body.filter_bank_status = 1; // Active
                body.filter_bank_gain = 0;
                body.spare_0 = 0;
                body.radio_status_crit = 0;
                body.radio_status_warn = 0;
                body.accel_status_crit = 0;
                body.accel_status_warn = 0;
                body.total_num_pdps_received = 0;
                body.scenario_set_count = 0;
                body.tap_app_seconds = 0;
                body.col_start_time = get_current_ntp_time();
            }

            void MT250_Message::update(uint64_t total_pdps, uint64_t set_count)
            {
                body.total_num_pdps_received = total_pdps;
                body.scenario_set_count = set_count;
                body.tap_app_seconds = get_current_ntp_time();
                header.seconds = get_current_ntp_time();
            }

            std::vector<uint8_t> MT250_Message::encode() const
            {
                std::vector<uint8_t> packet(COMMON_HEADER_SIZE + sizeof(mt250_body_t), 0);

                size_t offset = 0;

                // Encode header
                copy_to_buffer(packet.data(), &header, sizeof(common_header_t), offset);

                // Encode body
                copy_to_buffer(packet.data(), &body, sizeof(mt250_body_t), offset);

                return packet;
            }

        }
    }
} // namespace rfnoc::openairlink::mt_protocol
