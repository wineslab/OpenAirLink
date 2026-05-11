//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_upols_ifft
//
// Description:
//
//   Inverse FFT stage.  Spec §3.2 step 5 (y_buf = IFFT_K(Y_accum)).
//
//   Consumes a K-bin frequency-domain burst, produces a K-sample
//   time-domain burst with the same framing.  Convention:
//
//     y[n] = (1/K) sum_{k=0..K-1} Y[k] * exp(+j 2 pi k n / K)   (true IDFT)
//
//   Spec note: H_parts is already pre-scaled by 1/K on the host, so the
//   IFFT here can use either a true IDFT or an unscaled inverse — both
//   are documented to be equivalent (see upols_reference.hpp comment
//   "fft_inverse_unscaled").  The Xilinx FFT IP wrapper will be set to
//   the unscaled-inverse equivalent (same scaling schedule as the
//   forward stage; the 1/K is already inside H_parts).
//
//   ----------------------------------------------------------------
//   Planned implementation: Xilinx FFT IP (xfft v9.x)
//   ----------------------------------------------------------------
//
//   Identical IP generator settings to axi_upols_fft (see that file's
//   header for the full configuration), with only one difference:
//
//     s_axis_config_tdata[0]  : INV (tied to 0 in axi_upols_fft, tied
//                               to 1 in axi_upols_ifft, OR shared with
//                               a single run-time-configurable IP that
//                               both stages instantiate twice — which
//                               saves an .xci file but doubles config
//                               traffic at the runtime contract layer)
//
//   For first bring-up we use TWO separate IP instances (xfft_K_fwd
//   and xfft_K_inv) — they're independent, the dataflow is unidirectional,
//   and a constant-tied config input means we can skip the s_axis_config
//   stream entirely on the host side.
//
//   .xci location (when generated):
//     fpga/rfnoc_block_upols/ip/xfft_K_inv/xfft_K_inv.xci
//
//   Build hookup:
//     fpga/rfnoc_block_upols/Makefile.srcs adds:
//       LIB_IP_XCI_SRCS += $(LIB_IP_XFFT_K_INV_SRCS)
//
//   Verification: flavor C (osave_in -> FFT -> MAC[stub] -> IFFT ->
//   osave_out) passes only when both this stage and axi_upols_fft are
//   real and their scaling choices match.  Mismatched scaling shows up
//   as an amplitude error proportional to the schedule differential —
//   easy to diagnose, hard to miss.
//
//   ----------------------------------------------------------------
//   CURRENT STATUS: STUB
//   ----------------------------------------------------------------
//
//   AXI-Stream skid buffer; preserves rate and tlast so the chain
//   remains an end-to-end passthrough until the IP wrapper lands.
//

`default_nettype none


module axi_upols_ifft #(
  parameter integer K      = 256,
  parameter integer ITEM_W = 32
)(
  input  wire                  clk,
  input  wire                  rst,

  // AXI-Stream input: K-bin frequency-domain burst, tlast on bin K-1
  input  wire [ITEM_W-1:0]     s_axis_tdata,
  input  wire                  s_axis_tlast,
  input  wire                  s_axis_tvalid,
  output wire                  s_axis_tready,

  // AXI-Stream output: K-sample time-domain burst, tlast on sample K-1
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
      $fatal(1, "axi_upols_ifft: K must be a power of 2 (got %0d)", K);
    end
  end
  // synthesis translate_on

endmodule


`default_nettype wire
