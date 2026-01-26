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
 * MT Protocol Message Definitions for OpenAirLink
 * 
 * Ported from Colosseum/DynScen pymt protocol library.
 * Supports MT001 (Radio Config), MT010 (PDP), MT134 (Config Response), MT250 (Status).
 */

#ifndef INCLUDED_RFNOC_OPENAIRLINK_MT_PROTOCOL_HPP
#define INCLUDED_RFNOC_OPENAIRLINK_MT_PROTOCOL_HPP

#include <cstdint>
#include <cstring>
#include <complex>
#include <vector>
#include <string>

namespace rfnoc { namespace openairlink { namespace mt_protocol {

//=============================================================================
// Constants
//=============================================================================

constexpr size_t COMMON_HEADER_SIZE = 24;  // Size of common message header
constexpr size_t MAX_PACKET_SIZE = 65535;  // Maximum packet size (jumbo frame)
constexpr size_t NUM_FIR_TAPS = 41;        // Number of FIR taps (5ns spacing, 200ns max delay)

//=============================================================================
// Message Type IDs
//=============================================================================

namespace recv_message {
    enum type : uint8_t {
        radio_config = 1,       // MT-001: Radio configuration
        colosseum_init = 2,     // MT-002: Initialization
        coeff_matrix_req = 10,  // MT-010: Scenario coefficient matrix (PDP)
        undefined_message = 255
    };
}

namespace send_message {
    enum type : uint8_t {
        radio_config_resp = 134,   // MT-134: Radio config response
        colosseum_init_resp = 135, // MT-135: Init response
        status = 250,              // MT-250: Status message
        error = 255                // MT-255: Error message
    };
}

//=============================================================================
// Packed Structures (wire format)
//=============================================================================

#pragma pack(push, 1)

/**
 * Common Message Header (24 bytes)
 * Present at the start of every MT message.
 */
struct common_header_t {
    uint8_t message_type;       // 1 byte
    uint8_t spare;              // 1 byte  
    uint8_t src;                // 1 byte
    uint8_t dst;                // 1 byte
    uint32_t total_size;        // 4 bytes
    uint64_t seconds;           // 8 bytes
    uint64_t message_counter;   // 8 bytes
};

/**
 * Radio Configuration Data (12 bytes per radio)
 * Used in MT-001 and MT-134 messages.
 */
struct radio_data_t {
    uint8_t radio_id;
    uint8_t rx_channel_gain;  // In 0.5 dB increments
    uint8_t tx_channel_gain;  // In 0.5 dB increments
    uint8_t spare_0;
    float rx_center_freq;     // In MHz
    float tx_center_freq;     // In MHz
};

/**
 * Power Delay Profile / Channel Filter for OpenAirLink
 * 
 * Contains 41 FIR tap coefficients (5ns spacing, 200ns max delay).
 * The dynscen client sends all 41 taps with both real and imaginary parts.
 * OpenAirLink only uses the real part (imaginary is ignored).
 * 
 * Wire format: src_chan(2) + dst_chan(2) + coeff_real[41](82) + coeff_imag[41](82) = 168 bytes
 */
struct col_filter_t {
    uint16_t src_chan;                              // Source channel ID
    uint16_t dst_chan;                              // Destination channel ID
    uint16_t coeff_real[NUM_FIR_TAPS];              // Real part of coefficients (Q15)
    uint16_t coeff_imag[NUM_FIR_TAPS];              // Imaginary part (ignored by OAL)
};

/**
 * MT-001 Radio Configuration Header (12 bytes)
 * Followed by num_radios * radio_data_t structures.
 */
struct mt001_header_t {
    uint8_t num_radios;
    uint8_t spare_0;
    uint16_t spare_1;
    uint64_t reservation_id;
};

/**
 * MT-010 Coefficient Matrix Header (20 bytes)
 * Followed by num_channels_per_packet * col_filter_t structures.
 */
struct mt010_header_t {
    uint64_t tap_app_seconds;
    uint16_t num_channels_per_packet;
    uint8_t packets_per_update;
    uint8_t spare_0;
    uint64_t scenario_set_count;
};

/**
 * MT-134 Radio Config Response Header (4 bytes)
 * Followed by num_radios * radio_data_t structures.
 */
struct mt134_header_t {
    uint8_t num_radios;
    uint8_t wilco_cantco_val;  // 1 = WILCO (success), 0 = CANTCO (failure)
    uint16_t spare_0;
};

/**
 * MT-250 Status Message (60 bytes)
 */
struct mt250_body_t {
    uint8_t col_state;
    uint8_t filter_bank_status;
    uint8_t filter_bank_gain;
    uint8_t spare_0;
    uint64_t radio_status_crit;
    uint64_t radio_status_warn;
    uint32_t accel_status_crit;
    uint32_t accel_status_warn;
    uint64_t total_num_pdps_received;
    uint64_t scenario_set_count;
    uint64_t tap_app_seconds;
    uint64_t col_start_time;
};

#pragma pack(pop)

//=============================================================================
// Decoded Message Classes
//=============================================================================

/**
 * Decoded MT-001 Radio Configuration Message
 */
class MT001_Message {
public:
    common_header_t header;
    mt001_header_t body;
    std::vector<radio_data_t> radios;

    /**
     * Decode MT-001 message from raw bytes
     * @param data Raw message bytes (including header)
     * @param size Size of data buffer
     * @return true if successful, false otherwise
     */
    bool decode(const uint8_t* data, size_t size);

    /**
     * Get radio data by radio ID
     * @param radio_id Radio ID to find
     * @return Pointer to radio_data_t or nullptr if not found
     */
    const radio_data_t* get_radio(uint8_t radio_id) const;
};

/**
 * Decoded MT-010 Coefficient Matrix Message
 */
class MT010_Message {
public:
    common_header_t header;
    mt010_header_t body;
    std::vector<col_filter_t> pdps;

    /**
     * Decode MT-010 message from raw bytes
     * @param data Raw message bytes (including header)
     * @param size Size of data buffer
     * @return true if successful, false otherwise
     */
    bool decode(const uint8_t* data, size_t size);

    /**
     * Find PDP for given source and destination channels
     * @param src_chan Source channel ID
     * @param dst_chan Destination channel ID
     * @return Pointer to col_filter_t or nullptr if not found
     */
    const col_filter_t* find_pdp(uint16_t src_chan, uint16_t dst_chan) const;
};

/**
 * MT-134 Radio Config Response Message
 */
class MT134_Message {
public:
    common_header_t header;
    mt134_header_t body;
    std::vector<radio_data_t> radios;

    /**
     * Initialize response message
     * @param num_radios Number of radios configured
     * @param wilco True for success (WILCO), false for failure (CANTCO)
     * @param src Source endpoint ID
     * @param dst Destination endpoint ID
     * @param msg_counter Message counter value
     */
    void init(uint8_t num_radios, bool wilco, uint8_t src, uint8_t dst, uint64_t msg_counter);

    /**
     * Encode message to raw bytes
     * @return Encoded message bytes
     */
    std::vector<uint8_t> encode() const;
};

/**
 * MT-250 Status Message
 */
class MT250_Message {
public:
    common_header_t header;
    mt250_body_t body;

    /**
     * Initialize status message with default values
     * @param src Source endpoint ID
     * @param dst Destination endpoint ID
     * @param msg_counter Message counter value
     */
    void init(uint8_t src, uint8_t dst, uint64_t msg_counter);

    /**
     * Update status fields
     * @param total_pdps Total number of PDPs received
     * @param set_count Current scenario set count
     */
    void update(uint64_t total_pdps, uint64_t set_count);

    /**
     * Encode message to raw bytes
     * @return Encoded message bytes
     */
    std::vector<uint8_t> encode() const;
};

//=============================================================================
// Utility Functions
//=============================================================================

/**
 * Get message type from raw packet
 * @param data Raw packet data
 * @param size Size of data buffer
 * @return Message type ID or 255 if invalid
 */
uint8_t get_message_type(const uint8_t* data, size_t size);

/**
 * Extract FIR coefficients from PDP for OpenAirLink
 * @param pdp Power delay profile (contains 41 taps)
 * @param fir_coeffs Output FIR coefficients (41 taps)
 * 
 * Simply extracts the real part of each coefficient.
 * The imaginary part is ignored (not supported by OAL).
 */
void pdp_to_fir_coeffs(const col_filter_t& pdp, std::vector<int16_t>& fir_coeffs);

/**
 * Map MT channel IDs to OpenAirLink FIR block index
 * 
 * Fixed mapping (1 gNB + 3 UE topology):
 *   Channel 0 = gNB, Channel 2 = UE1, Channel 4 = UE2, Channel 6 = UE3
 * 
 * @param src_chan Source channel ID
 * @param dst_chan Destination channel ID
 * @return FIR block index (0-5) or -1 if invalid mapping
 */
int channel_to_fir_index(uint16_t src_chan, uint16_t dst_chan);

}}} // namespace rfnoc::openairlink::mt_protocol

#endif /* INCLUDED_RFNOC_OPENAIRLINK_MT_PROTOCOL_HPP */

