//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_upols_engine
//
// Description:
//
//   DSP engine for the UPOLS block.  Top of the data pipeline; structurally
//   the analogue of axi_complex_sparse_fir_complex.v in the sparse FIR
//   block.  This module owns no DSP logic of its own — it instantiates the
//   five stages of the streaming UPOLS algorithm in series:
//
//     s_axis ─► osave_in ─► fft ─► mac+FDL ─► ifft ─► osave_out ─► m_axis
//                                  ▲
//                                  │ (h_rd_addr / h_rd_data)
//                               H_parts BRAM (in rfnoc_block_upols.v)
//
//   Each stage is its own file so the algorithm can be brought up
//   incrementally:
//
//     axi_upols_osave_in.v   — sliding K-sample window (B-cadence framing)
//     axi_upols_fft.v        — forward K-point FFT  (Xilinx xfft IP)
//     axi_upols_mac.v        — frequency-domain MAC + circular FDL
//     axi_upols_ifft.v       — inverse  K-point FFT (Xilinx xfft IP)
//     axi_upols_osave_out.v  — overlap-save extract (drop first B of K)
//
//   At the time of writing every stage is a 1-deep AXI-Stream skid buffer
//   stub.  The chain therefore behaves as a benign sample-by-sample
//   passthrough — synthesizable on the X410 today, drop-in compatible
//   with the sparse FIR block at the image-core level, and ready to host
//   real DSP one stage at a time.
//
// Parameters:
//
//   N       : Filter length in taps
//   B       : Block length in samples (K = 2*B, P = ceil(N/B))
//   ITEM_W  : AXI-Stream item width (32 for sc16)
//

`default_nettype none


module axi_upols_engine #(
  parameter integer N       = 1024,
  parameter integer B       = 512,
  parameter integer ITEM_W  = 32
)(
  input  wire                                  clk,
  input  wire                                  rst,

  // AXI-Stream input (sc16)
  input  wire [ITEM_W-1:0]                     s_axis_tdata,
  input  wire                                  s_axis_tlast,
  input  wire                                  s_axis_tvalid,
  output wire                                  s_axis_tready,

  // AXI-Stream output (sc16)
  output wire [ITEM_W-1:0]                     m_axis_tdata,
  output wire                                  m_axis_tlast,
  output wire                                  m_axis_tvalid,
  input  wire                                  m_axis_tready,

  // H_parts BRAM read port
  output wire [$clog2((N+B-1)/B * 2*B)-1:0]    h_rd_addr,
  input  wire [31:0]                           h_rd_data,

  // Status flags (sticky-OR'd at the top level)
  output wire                                  overflow,
  output wire                                  underflow
);

  localparam integer K = 2 * B;

  // -------------------------------------------------------------------
  // Elaboration-time sanity checks on the parameter shape.
  // -------------------------------------------------------------------
  // synthesis translate_off
  initial begin
    if (B == 0 || (B & (B-1)) != 0) begin
      $fatal(1, "axi_upols_engine: B must be a power of 2 (got %0d)", B);
    end
    if (N == 0) begin
      $fatal(1, "axi_upols_engine: N must be > 0");
    end
    if (ITEM_W != 32) begin
      $fatal(1, "axi_upols_engine: ITEM_W must be 32 (sc16)");
    end
  end
  // synthesis translate_on


  // -------------------------------------------------------------------
  // Inter-stage AXI-Stream signals
  //
  // Naming: sNN_<sig> where NN is the stage boundary index.
  //   s01 — osave_in -> fft
  //   s12 — fft      -> mac
  //   s23 — mac      -> ifft
  //   s34 — ifft     -> osave_out
  // -------------------------------------------------------------------
  wire [ITEM_W-1:0] s01_tdata,  s12_tdata,  s23_tdata,  s34_tdata;
  wire              s01_tlast,  s12_tlast,  s23_tlast,  s34_tlast;
  wire              s01_tvalid, s12_tvalid, s23_tvalid, s34_tvalid;
  wire              s01_tready, s12_tready, s23_tready, s34_tready;

  wire stage_in_overflow;
  wire stage_out_underflow;


  // -------------------------------------------------------------------
  // Stage 0: overlap-save input window builder
  // -------------------------------------------------------------------
  axi_upols_osave_in #(
    .B      (B),
    .K      (K),
    .ITEM_W (ITEM_W)
  ) i_osave_in (
    .clk           (clk),
    .rst           (rst),
    .s_axis_tdata  (s_axis_tdata),
    .s_axis_tlast  (s_axis_tlast),
    .s_axis_tvalid (s_axis_tvalid),
    .s_axis_tready (s_axis_tready),
    .m_axis_tdata  (s01_tdata),
    .m_axis_tlast  (s01_tlast),
    .m_axis_tvalid (s01_tvalid),
    .m_axis_tready (s01_tready),
    .overflow      (stage_in_overflow)
  );


  // -------------------------------------------------------------------
  // Stage 1: forward FFT
  // -------------------------------------------------------------------
  axi_upols_fft #(
    .K      (K),
    .ITEM_W (ITEM_W)
  ) i_fft (
    .clk           (clk),
    .rst           (rst),
    .s_axis_tdata  (s01_tdata),
    .s_axis_tlast  (s01_tlast),
    .s_axis_tvalid (s01_tvalid),
    .s_axis_tready (s01_tready),
    .m_axis_tdata  (s12_tdata),
    .m_axis_tlast  (s12_tlast),
    .m_axis_tvalid (s12_tvalid),
    .m_axis_tready (s12_tready)
  );


  // -------------------------------------------------------------------
  // Stage 2: frequency-domain MAC + FDL  (drives H_parts read port)
  // -------------------------------------------------------------------
  axi_upols_mac #(
    .N      (N),
    .B      (B),
    .K      (K),
    .ITEM_W (ITEM_W)
  ) i_mac (
    .clk           (clk),
    .rst           (rst),
    .s_axis_tdata  (s12_tdata),
    .s_axis_tlast  (s12_tlast),
    .s_axis_tvalid (s12_tvalid),
    .s_axis_tready (s12_tready),
    .m_axis_tdata  (s23_tdata),
    .m_axis_tlast  (s23_tlast),
    .m_axis_tvalid (s23_tvalid),
    .m_axis_tready (s23_tready),
    .h_rd_addr     (h_rd_addr),
    .h_rd_data     (h_rd_data)
  );


  // -------------------------------------------------------------------
  // Stage 3: inverse FFT
  // -------------------------------------------------------------------
  axi_upols_ifft #(
    .K      (K),
    .ITEM_W (ITEM_W)
  ) i_ifft (
    .clk           (clk),
    .rst           (rst),
    .s_axis_tdata  (s23_tdata),
    .s_axis_tlast  (s23_tlast),
    .s_axis_tvalid (s23_tvalid),
    .s_axis_tready (s23_tready),
    .m_axis_tdata  (s34_tdata),
    .m_axis_tlast  (s34_tlast),
    .m_axis_tvalid (s34_tvalid),
    .m_axis_tready (s34_tready)
  );


  // -------------------------------------------------------------------
  // Stage 4: overlap-save output extractor
  // -------------------------------------------------------------------
  axi_upols_osave_out #(
    .B      (B),
    .K      (K),
    .ITEM_W (ITEM_W)
  ) i_osave_out (
    .clk           (clk),
    .rst           (rst),
    .s_axis_tdata  (s34_tdata),
    .s_axis_tlast  (s34_tlast),
    .s_axis_tvalid (s34_tvalid),
    .s_axis_tready (s34_tready),
    .m_axis_tdata  (m_axis_tdata),
    .m_axis_tlast  (m_axis_tlast),
    .m_axis_tvalid (m_axis_tvalid),
    .m_axis_tready (m_axis_tready),
    .underflow     (stage_out_underflow)
  );


  // -------------------------------------------------------------------
  // Status aggregation.  Sticky behaviour is implemented one level up,
  // in rfnoc_block_upols.v, via overflow_sticky / underflow_sticky.
  // -------------------------------------------------------------------
  assign overflow  = stage_in_overflow;
  assign underflow = stage_out_underflow;

endmodule // axi_upols_engine


`default_nettype wire
