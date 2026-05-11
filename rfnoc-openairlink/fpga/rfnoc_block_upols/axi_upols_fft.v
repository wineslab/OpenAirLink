//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_upols_fft
//
// Description:
//
//   Forward FFT stage.  Spec §3.2 step 2 (X_curr = FFT_K(input_buf)).
//
//   Consumes a K-sample time-domain burst (tlast on K-th sample), produces
//   a K-bin frequency-domain burst with the same framing.  Convention:
//
//     X[k] = sum_{n=0..K-1} x[n] * exp(-j 2 pi k n / K)        (unscaled)
//
//   No 1/K scaling is applied here — the host bakes 1/K into H_parts
//   (see streaming_upols_spec_v2.md §3.1, §7.8).
//
//   ----------------------------------------------------------------
//   Planned implementation: Xilinx FFT IP (xfft v9.x)
//   ----------------------------------------------------------------
//
//   IP configuration (reference values for K = 256, sc16 in/out):
//
//     Transform Length          : K  (parameter; e.g. 256)
//     Channels                  : 1
//     Architecture              : Pipelined, Streaming I/O
//                                 (continuous data input, lowest latency
//                                  per FFT, fixed throughput = K samples
//                                  per K cycles at NIPC=1)
//     Data Format               : Fixed Point
//     Input Data Width          : 16  (sc16: I[15:0], Q[15:0])
//     Phase Factor Width        : 16
//     Scaling                   : Scaled (Block Floating Point optional;
//                                  start with Scaled Fixed Point and
//                                  schedule [1 1 1 1 ...] = divide by 2
//                                  per stage = total /K, then re-scale
//                                  by K post-IFFT — but since the host
//                                  pre-bakes 1/K into H_parts we do NOT
//                                  need any post-FFT compensation here)
//     Rounding Mode             : Convergent Rounding (truncation also OK
//                                  for first bring-up)
//     Run-time Configurable     : No  (forward only, fixed K)
//     Output Ordering           : Natural Order
//     Cyclic Prefix Insertion   : Off
//     Throttle Scheme           : Real-time (or Non-real-time; either is
//                                  fine, real-time gives a simpler tready
//                                  contract with tvalid on every cycle)
//     Optional Pins             :
//       s_axis_config_tdata     : present (1-bit FWD/INV; tied to 1 here)
//       s_axis_config_tvalid    : present
//       s_axis_data_tlast       : present (we drive on K-th sample)
//       m_axis_data_tlast       : present (consumed downstream)
//       event_*                 : optional; tie off
//
//   .xci location (when generated):
//     fpga/rfnoc_block_upols/ip/xfft_K_fwd/xfft_K_fwd.xci
//
//   Build hookup:
//     fpga/rfnoc_block_upols/Makefile.srcs adds:
//       LIB_IP_XCI_SRCS += $(LIB_IP_XFFT_K_FWD_SRCS)
//     where xfft_K_fwd/Makefile.srcs follows the standard UHD IP pattern.
//
//   ----------------------------------------------------------------
//   CURRENT STATUS: STUB
//   ----------------------------------------------------------------
//
//   AXI-Stream skid buffer; preserves rate and tlast so the chain
//   remains an end-to-end passthrough until the IP wrapper lands.
//   With osave_in and osave_out already real, the K-burst framing now
//   flows through this stage faithfully — flavor A passthrough still
//   holds, and adding the real FFT IP will exercise the framing
//   contract on the FIRST attempt rather than coupling FFT bring-up
//   with framing bring-up.
//

`default_nettype none


module axi_upols_fft #(
  parameter integer K      = 256,
  parameter integer ITEM_W = 32
)(
  input  wire                  clk,
  input  wire                  rst,

  // AXI-Stream input: K-sample time-domain burst, tlast on sample K-1
  input  wire [ITEM_W-1:0]     s_axis_tdata,
  input  wire                  s_axis_tlast,
  input  wire                  s_axis_tvalid,
  output wire                  s_axis_tready,

  // AXI-Stream output: K-bin frequency-domain burst, tlast on bin K-1
  output wire [ITEM_W-1:0]     m_axis_tdata,
  output wire                  m_axis_tlast,
  output wire                  m_axis_tvalid,
  input  wire                  m_axis_tready
);

  // Stub: AXI-Stream skid buffer.
  axi_fifo #(
    .WIDTH (ITEM_W + 1),
    .SIZE  (1)
  ) skid (
    .clk      (clk),
    .reset    (rst),
    .clear    (1'b0),
    .i_tdata  ({s_axis_tlast, s_axis_tdata}),
    .i_tvalid (s_axis_tvalid),
    .i_tready (s_axis_tready),
    .o_tdata  ({m_axis_tlast, m_axis_tdata}),
    .o_tvalid (m_axis_tvalid),
    .o_tready (m_axis_tready)
  );

  // synthesis translate_off
  initial begin
    if (K == 0 || (K & (K-1)) != 0) begin
      $fatal(1, "axi_upols_fft: K must be a power of 2 (got %0d)", K);
    end
  end
  // synthesis translate_on

endmodule


`default_nettype wire
