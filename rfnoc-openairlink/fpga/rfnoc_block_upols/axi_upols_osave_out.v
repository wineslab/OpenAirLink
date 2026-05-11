//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_upols_osave_out
//
// Description:
//
//   Overlap-save output extractor.  Spec §3.2 step 6.
//
//   Consumes a K-sample time-domain burst (tlast on the K-th sample) and
//   forwards only the last B samples — the alias-free portion of a
//   K-point circular convolution:
//
//     y_out[0 .. B-1] = y_buf[B .. K-1]    (drop first B, keep last B)
//
//   Output framing: tlast asserted on the K-th input sample (i.e. the
//   B-th emitted sample) so the downstream CHDR framer sees one packet
//   per emitted block.  This matches the framing convention introduced
//   by axi_upols_osave_in at the head of the pipeline.
//
//   Implementation is purely combinational selection plus a small FSM
//   counter — the "drop" half consumes input bandwidth but produces no
//   output; the "emit" half forwards 1:1 with downstream backpressure
//   propagated upstream.
//
//   Pairing with axi_upols_osave_in: when every other stage in the
//   chain (fft, mac, ifft) is a stub passthrough, the end-to-end
//   behaviour is `out[n] = in[n]` (modulo a 5-stage pipeline latency
//   plus the K-sample emit cadence inside osave_in) because the K-burst
//   is `[prev | curr]` and we keep `curr`.  This is the basis of the
//   "flavor A" passthrough sanity test.
//

`default_nettype none


module axi_upols_osave_out #(
  parameter integer B      = 128,
  parameter integer K      = 256,
  parameter integer ITEM_W = 32
)(
  input  wire                  clk,
  input  wire                  rst,

  // AXI-Stream input: K-sample time-domain burst (tlast on K-th)
  input  wire [ITEM_W-1:0]     s_axis_tdata,
  input  wire                  s_axis_tlast,
  input  wire                  s_axis_tvalid,
  output wire                  s_axis_tready,

  // AXI-Stream output: B-sample stream, one packet per K-burst
  output wire [ITEM_W-1:0]     m_axis_tdata,
  output wire                  m_axis_tlast,
  output wire                  m_axis_tvalid,
  input  wire                  m_axis_tready,

  // Sticky-OR'd at engine top
  output wire                  underflow
);

  localparam integer CNT_W = $clog2(K + 1);

  // burst_idx counts each consumed input sample within its K-burst.
  // 0..B-1 are dropped; B..K-1 are forwarded.
  reg [CNT_W-1:0] burst_idx;

  wire in_drop_phase = (burst_idx < B);

  // s_axis_tready:
  //   - drop phase  : always ready (we sink into the void)
  //   - emit phase  : forward downstream backpressure
  // m_axis_tvalid:
  //   - drop phase  : 0
  //   - emit phase  : tracks s_axis_tvalid
  // m_axis_tlast:
  //   - asserted on the K-th input sample (= B-th emitted sample of the
  //     burst), giving downstream one packet per K-burst.
  assign s_axis_tready = in_drop_phase ? 1'b1 : m_axis_tready;
  assign m_axis_tvalid = !in_drop_phase && s_axis_tvalid;
  assign m_axis_tdata  = s_axis_tdata;
  assign m_axis_tlast  = !in_drop_phase && s_axis_tvalid && (burst_idx == K - 1);

  wire in_handshake = s_axis_tvalid && s_axis_tready;

  always @(posedge clk) begin
    if (rst) begin
      burst_idx <= {CNT_W{1'b0}};
    end else if (in_handshake) begin
      if (burst_idx == K - 1)
        burst_idx <= {CNT_W{1'b0}};
      else
        burst_idx <= burst_idx + 1'b1;
    end
  end

  // Underflow (downstream stall in the middle of an emit burst that
  // overruns the pipeline FIFOs) — left as a TODO for the integration
  // test pass.
  assign underflow = 1'b0;

  // ------------------------------------------------------------------
  // Elaboration-time checks
  // ------------------------------------------------------------------
  // synthesis translate_off
  initial begin
    if (K != 2 * B)
      $fatal(1, "axi_upols_osave_out: K (%0d) must equal 2*B (%0d)", K, 2*B);
  end
  // synthesis translate_on

endmodule


`default_nettype wire
