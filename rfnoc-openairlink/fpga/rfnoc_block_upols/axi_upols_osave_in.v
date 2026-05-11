//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_upols_osave_in
//
// Description:
//
//   Overlap-save input window builder.  Spec §3.2 step 1 + step 2-prep.
//
//   Consumes a continuous sc16 sample stream and produces K-sample bursts.
//   Each output burst is the time-domain OVS window:
//
//     burst[0   .. B-1]   = previous block (samples written K..2K-1 ago)
//     burst[B   .. K-1]   = current  block (the B samples just received)
//
//   Output framing: tlast asserted on the K-th sample of each burst.
//
//   ----------------------------------------------------------------
//   Throughput: 1/3 input rate (FILL=B + EMIT=K cycles per block).
//   ----------------------------------------------------------------
//
//   This is the simplest correct implementation: a single K-deep BRAM and
//   a 2-state FSM that accepts B input samples (FILL), then emits the
//   K-sample window (EMIT, with input back-pressured), then returns to
//   FILL.  At ce_clk = 266 MHz this caps the supported input rate to
//   ~88 MHz — fine for LTE-20 / NR-50 bring-up but below NR-100's
//   122.88 MSPS.  The upgrade paths (β: dual-port BRAM with input-side
//   throttle => 1/2 rate, γ: NIPC=2 internal bus => full rate) are
//   noted in the engine-level Makefile.srcs comment.
//
//   ----------------------------------------------------------------
//   Window addressing
//   ----------------------------------------------------------------
//
//   `win` is treated as a K-sample circular buffer with one write
//   pointer.  After every K writes, `wr_ptr` returns to its starting
//   position; the OLDEST sample at any time is at `wr_ptr` (the slot
//   about to be overwritten next).  After a block of B writes, the
//   K-sample window is the K samples preceding `wr_ptr` (mod K), so
//   reading K positions starting from `wr_ptr` produces them in order
//   from oldest to newest — exactly the OVS time-domain window.
//
//   At reset, `win` is zero (Vivado initializes inferred BRAM to zero,
//   and the explicit `initial` loop covers simulation).  The first P
//   blocks of output therefore correspond to the natural startup
//   transient described in spec §3.3 — partial convolutions where the
//   "previous block" half is zeros.
//

`default_nettype none


module axi_upols_osave_in #(
  parameter integer B      = 128,
  parameter integer K      = 256,
  parameter integer ITEM_W = 32
)(
  input  wire                  clk,
  input  wire                  rst,

  // AXI-Stream input: sc16 samples, 1 per beat
  input  wire [ITEM_W-1:0]     s_axis_tdata,
  input  wire                  s_axis_tlast,
  input  wire                  s_axis_tvalid,
  output wire                  s_axis_tready,

  // AXI-Stream output: K-sample time-domain bursts (tlast on K-th sample)
  output wire [ITEM_W-1:0]     m_axis_tdata,
  output wire                  m_axis_tlast,
  output wire                  m_axis_tvalid,
  input  wire                  m_axis_tready,

  // Sticky-OR'd at engine top
  output wire                  overflow
);

  localparam integer ADDR_W = $clog2(K);

  // ------------------------------------------------------------------
  // Window buffer (BRAM-inferred)
  // ------------------------------------------------------------------
  (* ram_style = "block" *)
  reg [ITEM_W-1:0] win [0:K-1];

  integer init_i;
  initial begin
    for (init_i = 0; init_i < K; init_i = init_i + 1)
      win[init_i] = {ITEM_W{1'b0}};
  end

  // ------------------------------------------------------------------
  // FSM state
  //   S_FILL : accepting input, writing to win[]
  //   S_EMIT : reading K samples from win[], input back-pressured
  // ------------------------------------------------------------------
  localparam S_FILL = 1'b0;
  localparam S_EMIT = 1'b1;
  reg state;

  reg [ADDR_W-1:0]              wr_ptr;
  reg [ADDR_W-1:0]              rd_ptr;
  reg [$clog2(B+1)-1:0]         fill_cnt;  // 0..B-1 during FILL
  reg [$clog2(K+1)-1:0]         emit_cnt;  // 0..K-1 during EMIT

  // ------------------------------------------------------------------
  // BRAM-read pipeline: 1-cycle synchronous read.  rd_data captures
  // win[rd_ptr] on the clock edge after rd_ptr is presented; rd_valid
  // delays alongside it so the output skid sees aligned (data, valid).
  // ------------------------------------------------------------------
  reg [ITEM_W-1:0]              rd_data;
  reg                           rd_valid;
  reg                           rd_last;

  // ------------------------------------------------------------------
  // Output skid buffer.  Decouples BRAM read from m_axis_tready and
  // gives one cycle of slack to absorb a momentary downstream stall
  // without losing a read in flight.
  // ------------------------------------------------------------------
  wire skid_in_ready;

  axi_fifo #(
    .WIDTH (ITEM_W + 1),
    .SIZE  (2)         // depth = 4
  ) out_skid (
    .clk      (clk),
    .reset    (rst),
    .clear    (1'b0),
    .i_tdata  ({rd_last, rd_data}),
    .i_tvalid (rd_valid),
    .i_tready (skid_in_ready),
    .o_tdata  ({m_axis_tlast, m_axis_tdata}),
    .o_tvalid (m_axis_tvalid),
    .o_tready (m_axis_tready)
  );

  assign s_axis_tready = (state == S_FILL);

  // ------------------------------------------------------------------
  // Main FSM
  // ------------------------------------------------------------------
  always @(posedge clk) begin
    if (rst) begin
      state    <= S_FILL;
      wr_ptr   <= {ADDR_W{1'b0}};
      rd_ptr   <= {ADDR_W{1'b0}};
      fill_cnt <= {($clog2(B+1)){1'b0}};
      emit_cnt <= {($clog2(K+1)){1'b0}};
      rd_valid <= 1'b0;
      rd_last  <= 1'b0;
    end else begin
      // Default: no read this cycle (single-cycle pulses).
      rd_valid <= 1'b0;
      rd_last  <= 1'b0;

      case (state)
        // ----------------------------------------------------------
        // FILL: accept B input samples
        // ----------------------------------------------------------
        S_FILL: begin
          if (s_axis_tvalid) begin
            win[wr_ptr] <= s_axis_tdata;
            wr_ptr      <= wr_ptr + 1'b1;   // wraps mod K (K is power of 2)

            if (fill_cnt == B - 1) begin
              // B-th sample of this block — switch to EMIT.
              // The K-sample window is at positions wr_ptr_new .. wr_ptr_new+K-1
              // (mod K), where wr_ptr_new = wr_ptr + 1 (the value being
              // assigned this cycle).  rd_ptr starts at wr_ptr_new.
              state    <= S_EMIT;
              rd_ptr   <= wr_ptr + 1'b1;
              emit_cnt <= {($clog2(K+1)){1'b0}};
              fill_cnt <= {($clog2(B+1)){1'b0}};
            end else begin
              fill_cnt <= fill_cnt + 1'b1;
            end
          end
        end

        // ----------------------------------------------------------
        // EMIT: read K samples from win[], starting at rd_ptr
        // ----------------------------------------------------------
        S_EMIT: begin
          // Only issue a read if the skid can absorb the result.
          if (skid_in_ready && (emit_cnt < K)) begin
            rd_data  <= win[rd_ptr];
            rd_valid <= 1'b1;
            rd_last  <= (emit_cnt == K - 1);
            rd_ptr   <= rd_ptr + 1'b1;     // wraps mod K
            emit_cnt <= emit_cnt + 1'b1;

            if (emit_cnt == K - 1) begin
              // Last read of this burst issued — return to FILL on
              // the next cycle.  The skid buffer holds the in-flight
              // sample until downstream takes it.
              state <= S_FILL;
            end
          end
        end

        default: state <= S_FILL;
      endcase
    end
  end

  // Overflow detection deferred — implementation note:
  //   set when s_axis_tvalid drops mid-FILL and stays low long enough
  //   that the upstream radio framing breaks.  Not needed for first
  //   bring-up; left as a TODO for the integration test pass.
  assign overflow = 1'b0;

  // ------------------------------------------------------------------
  // Elaboration-time checks
  // ------------------------------------------------------------------
  // synthesis translate_off
  initial begin
    if (K != 2 * B)
      $fatal(1, "axi_upols_osave_in: K (%0d) must equal 2*B (%0d)", K, 2*B);
    if ((K & (K-1)) != 0)
      $fatal(1, "axi_upols_osave_in: K (%0d) must be a power of 2", K);
  end
  // synthesis translate_on

endmodule


`default_nettype wire
