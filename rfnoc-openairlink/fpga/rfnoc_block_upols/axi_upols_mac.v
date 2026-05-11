//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_upols_mac
//
// Description:
//
//   Frequency-domain MAC + Frequency-Domain Delay Line (FDL).
//   Spec §3.2 steps 3 + 4.
//
//   For each incoming K-bin frequency burst X_curr (output of axi_upols_fft):
//
//     1. write_ptr <- (write_ptr + 1) mod P
//     2. FDL[write_ptr] <- X_curr
//     3. Y_accum[k] = sum_{p=0..P-1} H_parts[p][k] * FDL[(write_ptr - p + P) mod P][k]
//     4. emit Y_accum as a K-bin burst (tlast on bin K-1)
//
//   Memory:
//     - FDL is a P*K complex-bin BRAM owned by this module.
//     - H_parts is an external BRAM owned by rfnoc_block_upols (host writes
//       Q1.15 packed {im[31:16], re[15:0]} via ctrlport).  This module
//       drives the read address port and consumes h_rd_data.
//
//   The MAC is the heart of the algorithm and is also the resource hot
//   spot: P complex MACs per bin, K bins per block, B sample budget per
//   block.  At ce_clk = 266 MHz with B = 128 we have 128 cycles to do
//   K*P = 256*2 = 512 complex MACs, i.e. 4 complex MACs per cycle, which
//   maps cleanly to a single time-multiplexed DSP48 cluster.
//
// CURRENT STATUS: STUB.  AXI-Stream skid buffer; h_rd_addr tied to 0.
//

`default_nettype none


module axi_upols_mac #(
  parameter integer N      = 1024,
  parameter integer B      = 512,
  parameter integer K      = 1024,
  parameter integer ITEM_W = 32
)(
  input  wire                                  clk,
  input  wire                                  rst,

  // AXI-Stream input: X_curr K-bin frequency burst
  input  wire [ITEM_W-1:0]                     s_axis_tdata,
  input  wire                                  s_axis_tlast,
  input  wire                                  s_axis_tvalid,
  output wire                                  s_axis_tready,

  // AXI-Stream output: Y_accum K-bin frequency burst
  output wire [ITEM_W-1:0]                     m_axis_tdata,
  output wire                                  m_axis_tlast,
  output wire                                  m_axis_tvalid,
  input  wire                                  m_axis_tready,

  // H_parts BRAM read port (driven by this module's MAC scheduler)
  output wire [$clog2((N+B-1)/B * K)-1:0]      h_rd_addr,
  input  wire [31:0]                           h_rd_data
);

  // Stub: AXI-Stream skid buffer; H_parts read held idle.
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

  assign h_rd_addr = {$clog2((N+B-1)/B * K){1'b0}};

  wire _unused_h_rd_data_ok = &{1'b0, h_rd_data};

  // synthesis translate_off
  initial begin
    if (K != 2 * B) begin
      $fatal(1, "axi_upols_mac: K (%0d) must equal 2*B (%0d)", K, 2*B);
    end
    if (N == 0) begin
      $fatal(1, "axi_upols_mac: N must be > 0");
    end
  end
  // synthesis translate_on

endmodule


`default_nettype wire
