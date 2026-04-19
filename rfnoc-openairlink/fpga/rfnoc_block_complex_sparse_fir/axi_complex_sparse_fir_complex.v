//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_complex_sparse_fir_complex
//
// Description:
//
//   Complex-coefficient complex sparse FIR filter engine for channel emulation.
//   Processes sc16 (I[31:16], Q[15:0]) samples with complex tap coefficients
//   h[k] = h_re[k] + j*h_im[k], performing true complex multiplication:
//
//     out_I[n] = sum_k ( h_re[k]*I[n-d[k]] - h_im[k]*Q[n-d[k]] )
//     out_Q[n] = sum_k ( h_re[k]*Q[n-d[k]] + h_im[k]*I[n-d[k]] )
//
//   Uses BRAM-based circular delay lines (one per I/Q per tap) with
//   NUM_TAPS independently-addressable taps. Each tap requires 4 DSP48
//   multiplies (h_re*I, h_im*Q, h_re*Q, h_im*I).
//
//   When h_im[k]=0 for all taps, behavior is identical to the real-only
//   axi_complex_sparse_fir module.
//
// Parameters:
//
//   IN_WIDTH    : Width of each I or Q component (default 16)
//   OUT_WIDTH   : Width of each output component (default 16)
//   COEFF_WIDTH : Width of each coefficient component (default 16, Q1.15)
//   NUM_TAPS    : Number of sparse taps (power of 2, 1..32)
//   MAX_DELAY   : Circular buffer depth (power of 2)
//
// Pipeline latency (sample_stb -> m_axis_tvalid):
//
//   1(BRAM) + 1(tap_data_reg) + 1(multiply) + 1(cross_combine) + TREE_STAGES + 1(output_reg)
//   = 5 + TREE_STAGES
//

`default_nettype none

module axi_complex_sparse_fir_complex #(
  parameter IN_WIDTH    = 16,
  parameter OUT_WIDTH   = 16,
  parameter COEFF_WIDTH = 16,
  parameter NUM_TAPS    = 4,
  parameter MAX_DELAY   = 1024
)(
  input  wire clk,
  input  wire rst,

  // AXI-Stream input: sc16 {I[31:16], Q[15:0]}
  input  wire [2*IN_WIDTH-1:0]  s_axis_tdata,
  input  wire                   s_axis_tlast,
  input  wire                   s_axis_tvalid,
  output wire                   s_axis_tready,

  // AXI-Stream output: sc16 {I[31:16], Q[15:0]}
  output wire [2*OUT_WIDTH-1:0] m_axis_tdata,
  output wire                   m_axis_tlast,
  output wire                   m_axis_tvalid,
  input  wire                   m_axis_tready,

  // Tap configuration
  input  wire [NUM_TAPS*$clog2(MAX_DELAY)-1:0] tap_delays,
  input  wire [NUM_TAPS*COEFF_WIDTH-1:0]        tap_coeffs_re,
  input  wire [NUM_TAPS*COEFF_WIDTH-1:0]        tap_coeffs_im
);

  // -------------------------------------------------------------------------
  // Local parameters
  // -------------------------------------------------------------------------
  localparam DELAY_W = $clog2(MAX_DELAY);
  localparam MULT_W  = IN_WIDTH + COEFF_WIDTH;

  // Cross-combine adds two MULT_W products, needs 1 extra guard bit
  localparam CROSS_W = MULT_W + 1;

  // Accumulator: guard bits for summing NUM_TAPS cross-combined products
  localparam ACCUM_W = CROSS_W + $clog2(NUM_TAPS);

  localparam TREE_STAGES = ($clog2(NUM_TAPS) > 0) ? $clog2(NUM_TAPS) : 1;
  localparam PIPELINE_DELAY = 5 + TREE_STAGES;
  localparam SR_DEPTH = PIPELINE_DELAY - 1;

  // Normalization: coefficients are Q1.(COEFF_WIDTH-1)
  localparam ROUND_BITS = COEFF_WIDTH - 1;
  localparam NORM_W     = ACCUM_W - ROUND_BITS;

  localparam TREE_FLAT_SIZE = NUM_TAPS;

  // -------------------------------------------------------------------------
  // Flow control
  // -------------------------------------------------------------------------
  wire sample_stb = s_axis_tvalid & s_axis_tready;

  // -------------------------------------------------------------------------
  // Input split
  // -------------------------------------------------------------------------
  wire [IN_WIDTH-1:0] in_i = s_axis_tdata[2*IN_WIDTH-1 : IN_WIDTH];
  wire [IN_WIDTH-1:0] in_q = s_axis_tdata[IN_WIDTH-1   : 0];

  // -------------------------------------------------------------------------
  // Write pointer
  // -------------------------------------------------------------------------
  reg [DELAY_W-1:0] wr_ptr;

  always @(posedge clk) begin
    if (rst)
      wr_ptr <= {DELAY_W{1'b0}};
    else if (sample_stb)
      wr_ptr <= wr_ptr + 1'b1;
  end

  // -------------------------------------------------------------------------
  // Delay lines: one BRAM for I, one for Q, per tap
  // -------------------------------------------------------------------------
  wire [IN_WIDTH-1:0] delayed_i [0:NUM_TAPS-1];
  wire [IN_WIDTH-1:0] delayed_q [0:NUM_TAPS-1];

  genvar t;
  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_delay_line

      wire [DELAY_W-1:0] this_delay = tap_delays[DELAY_W*t +: DELAY_W];
      wire [DELAY_W-1:0] rd_addr    = wr_ptr - this_delay;

      // I delay line
      (* ram_style = "block" *)
      reg [IN_WIDTH-1:0] delay_mem_i [0:MAX_DELAY-1];

      integer m;
      initial begin
        for (m = 0; m < MAX_DELAY; m = m + 1)
          delay_mem_i[m] = {IN_WIDTH{1'b0}};
      end

      reg [IN_WIDTH-1:0] rd_data_i;

      always @(posedge clk) begin
        if (sample_stb)
          delay_mem_i[wr_ptr] <= in_i;
      end

      always @(posedge clk) begin
        if (sample_stb)
          rd_data_i <= delay_mem_i[rd_addr];
      end

      // Q delay line
      (* ram_style = "block" *)
      reg [IN_WIDTH-1:0] delay_mem_q [0:MAX_DELAY-1];

      initial begin
        for (m = 0; m < MAX_DELAY; m = m + 1)
          delay_mem_q[m] = {IN_WIDTH{1'b0}};
      end

      reg [IN_WIDTH-1:0] rd_data_q;

      always @(posedge clk) begin
        if (sample_stb)
          delay_mem_q[wr_ptr] <= in_q;
      end

      always @(posedge clk) begin
        if (sample_stb)
          rd_data_q <= delay_mem_q[rd_addr];
      end

      // Collision detection (delay=0)
      wire rd_wr_collision = (rd_addr == wr_ptr);
      reg  rd_wr_collision_r;
      reg  [IN_WIDTH-1:0] wr_data_i_r, wr_data_q_r;

      always @(posedge clk) begin
        if (rst) begin
          rd_wr_collision_r <= 1'b0;
          wr_data_i_r       <= {IN_WIDTH{1'b0}};
          wr_data_q_r       <= {IN_WIDTH{1'b0}};
        end else if (sample_stb) begin
          rd_wr_collision_r <= rd_wr_collision;
          wr_data_i_r       <= in_i;
          wr_data_q_r       <= in_q;
        end
      end

      assign delayed_i[t] = rd_wr_collision_r ? wr_data_i_r : rd_data_i;
      assign delayed_q[t] = rd_wr_collision_r ? wr_data_q_r : rd_data_q;

    end
  endgenerate

  // -------------------------------------------------------------------------
  // Tap data register: breaks BRAM -> bypass MUX -> DSP path
  // -------------------------------------------------------------------------
  reg [IN_WIDTH-1:0] tap_data_i [0:NUM_TAPS-1];
  reg [IN_WIDTH-1:0] tap_data_q [0:NUM_TAPS-1];

  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_tap_data_reg
      always @(posedge clk) begin
        if (rst) begin
          tap_data_i[t] <= {IN_WIDTH{1'b0}};
          tap_data_q[t] <= {IN_WIDTH{1'b0}};
        end else if (sample_stb) begin
          tap_data_i[t] <= delayed_i[t];
          tap_data_q[t] <= delayed_q[t];
        end
      end
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Multiply: 4 products per tap
  //   prod_re_i[t] = h_re[t] * I[t]   (for out_I, positive term)
  //   prod_im_q[t] = h_im[t] * Q[t]   (for out_I, subtracted)
  //   prod_re_q[t] = h_re[t] * Q[t]   (for out_Q, positive term)
  //   prod_im_i[t] = h_im[t] * I[t]   (for out_Q, positive term)
  // -------------------------------------------------------------------------
  reg signed [MULT_W-1:0] prod_re_i [0:NUM_TAPS-1];
  reg signed [MULT_W-1:0] prod_im_q [0:NUM_TAPS-1];
  reg signed [MULT_W-1:0] prod_re_q [0:NUM_TAPS-1];
  reg signed [MULT_W-1:0] prod_im_i [0:NUM_TAPS-1];

  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_multiply
      wire signed [COEFF_WIDTH-1:0] coeff_re =
          tap_coeffs_re[COEFF_WIDTH*t +: COEFF_WIDTH];
      wire signed [COEFF_WIDTH-1:0] coeff_im =
          tap_coeffs_im[COEFF_WIDTH*t +: COEFF_WIDTH];

      always @(posedge clk) begin
        if (rst) begin
          prod_re_i[t] <= {MULT_W{1'b0}};
          prod_im_q[t] <= {MULT_W{1'b0}};
          prod_re_q[t] <= {MULT_W{1'b0}};
          prod_im_i[t] <= {MULT_W{1'b0}};
        end else if (sample_stb) begin
          prod_re_i[t] <= $signed(tap_data_i[t]) * coeff_re;
          prod_im_q[t] <= $signed(tap_data_q[t]) * coeff_im;
          prod_re_q[t] <= $signed(tap_data_q[t]) * coeff_re;
          prod_im_i[t] <= $signed(tap_data_i[t]) * coeff_im;
        end
      end
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Cross-combine: compute complex products per tap
  //   cprod_i[t] = h_re*I - h_im*Q   (real part of complex product)
  //   cprod_q[t] = h_re*Q + h_im*I   (imag part of complex product)
  // -------------------------------------------------------------------------
  reg signed [CROSS_W-1:0] cprod_i [0:NUM_TAPS-1];
  reg signed [CROSS_W-1:0] cprod_q [0:NUM_TAPS-1];

  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_cross_combine
      always @(posedge clk) begin
        if (rst) begin
          cprod_i[t] <= {CROSS_W{1'b0}};
          cprod_q[t] <= {CROSS_W{1'b0}};
        end else if (sample_stb) begin
          cprod_i[t] <= $signed({{1{prod_re_i[t][MULT_W-1]}}, prod_re_i[t]})
                      - $signed({{1{prod_im_q[t][MULT_W-1]}}, prod_im_q[t]});
          cprod_q[t] <= $signed({{1{prod_re_q[t][MULT_W-1]}}, prod_re_q[t]})
                      + $signed({{1{prod_im_i[t][MULT_W-1]}}, prod_im_i[t]});
        end
      end
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Adder trees: one for I output, one for Q output
  // Same flat-array layout as axi_complex_sparse_fir.v
  // -------------------------------------------------------------------------
  reg signed [ACCUM_W-1:0] tree_i [0:TREE_FLAT_SIZE-1];
  reg signed [ACCUM_W-1:0] tree_q [0:TREE_FLAT_SIZE-1];

  genvar s, n;
  generate
    for (s = 0; s < TREE_STAGES; s = s + 1) begin : gen_tree_stage
      localparam integer N_OUT    = ((NUM_TAPS >> (s+1)) > 0) ?
                                      (NUM_TAPS >> (s+1)) : 1;
      localparam integer DST_BASE = NUM_TAPS - (NUM_TAPS >> s);

      for (n = 0; n < N_OUT; n = n + 1) begin : gen_tree_node

        if (s == 0) begin : gen_stage0

          if (2*n + 1 < NUM_TAPS) begin : gen_add0
            always @(posedge clk) begin
              if (rst) begin
                tree_i[DST_BASE + n] <= {ACCUM_W{1'b0}};
                tree_q[DST_BASE + n] <= {ACCUM_W{1'b0}};
              end else if (sample_stb) begin
                tree_i[DST_BASE + n] <=
                    $signed({{(ACCUM_W-CROSS_W){cprod_i[2*n  ][CROSS_W-1]}}, cprod_i[2*n  ]}) +
                    $signed({{(ACCUM_W-CROSS_W){cprod_i[2*n+1][CROSS_W-1]}}, cprod_i[2*n+1]});
                tree_q[DST_BASE + n] <=
                    $signed({{(ACCUM_W-CROSS_W){cprod_q[2*n  ][CROSS_W-1]}}, cprod_q[2*n  ]}) +
                    $signed({{(ACCUM_W-CROSS_W){cprod_q[2*n+1][CROSS_W-1]}}, cprod_q[2*n+1]});
              end
            end
          end else begin : gen_passthrough0
            always @(posedge clk) begin
              if (rst) begin
                tree_i[DST_BASE + n] <= {ACCUM_W{1'b0}};
                tree_q[DST_BASE + n] <= {ACCUM_W{1'b0}};
              end else if (sample_stb) begin
                tree_i[DST_BASE + n] <=
                    $signed({{(ACCUM_W-CROSS_W){cprod_i[0][CROSS_W-1]}}, cprod_i[0]});
                tree_q[DST_BASE + n] <=
                    $signed({{(ACCUM_W-CROSS_W){cprod_q[0][CROSS_W-1]}}, cprod_q[0]});
              end
            end
          end

        end else begin : gen_stage_s

          localparam integer SRC_BASE = NUM_TAPS - (NUM_TAPS >> (s-1));
          localparam integer N_PREV   = ((NUM_TAPS >> s) > 0) ? (NUM_TAPS >> s) : 1;

          if (2*n + 1 < N_PREV) begin : gen_add_s
            always @(posedge clk) begin
              if (rst) begin
                tree_i[DST_BASE + n] <= {ACCUM_W{1'b0}};
                tree_q[DST_BASE + n] <= {ACCUM_W{1'b0}};
              end else if (sample_stb) begin
                tree_i[DST_BASE + n] <=
                    tree_i[SRC_BASE + 2*n] + tree_i[SRC_BASE + 2*n + 1];
                tree_q[DST_BASE + n] <=
                    tree_q[SRC_BASE + 2*n] + tree_q[SRC_BASE + 2*n + 1];
              end
            end
          end else begin : gen_passthrough_s
            always @(posedge clk) begin
              if (rst) begin
                tree_i[DST_BASE + n] <= {ACCUM_W{1'b0}};
                tree_q[DST_BASE + n] <= {ACCUM_W{1'b0}};
              end else if (sample_stb) begin
                tree_i[DST_BASE + n] <= tree_i[SRC_BASE + 2*n];
                tree_q[DST_BASE + n] <= tree_q[SRC_BASE + 2*n];
              end
            end
          end

        end

      end
    end
  endgenerate

  // Root of adder trees
  localparam ROOT_IDX = NUM_TAPS - (NUM_TAPS >> (TREE_STAGES - 1));
  wire signed [ACCUM_W-1:0] raw_out_i = tree_i[ROOT_IDX];
  wire signed [ACCUM_W-1:0] raw_out_q = tree_q[ROOT_IDX];

  // -------------------------------------------------------------------------
  // Pipeline flag shift register
  // -------------------------------------------------------------------------
  reg [SR_DEPTH-1:0] valid_sr;
  reg [SR_DEPTH-1:0] tlast_sr;

  always @(posedge clk) begin
    if (rst) begin
      valid_sr <= {SR_DEPTH{1'b0}};
      tlast_sr <= {SR_DEPTH{1'b0}};
    end else if (sample_stb) begin
      valid_sr <= {valid_sr[SR_DEPTH-2:0], 1'b1};
      tlast_sr <= {tlast_sr[SR_DEPTH-2:0], s_axis_tlast};
    end
  end

  wire pre_valid = valid_sr[SR_DEPTH-1];
  wire pre_tlast = tlast_sr[SR_DEPTH-1];

  // -------------------------------------------------------------------------
  // Output normalization: round and saturate, independently for I and Q
  // -------------------------------------------------------------------------

  // --- I path ---
  wire signed [ACCUM_W-1:0] rounded_i =
      raw_out_i + {{(ACCUM_W-ROUND_BITS){1'b0}}, 1'b1, {(ROUND_BITS-1){1'b0}}};
  wire signed [NORM_W-1:0] normalized_i = rounded_i[ACCUM_W-1 -: NORM_W];

  wire overflow_i;
  wire signed [OUT_WIDTH-1:0] clipped_i;

  generate
    if (NORM_W > OUT_WIDTH) begin : gen_saturate_i
      wire [NORM_W-OUT_WIDTH:0] sign_ext_i =
          normalized_i[NORM_W-1 -: (NORM_W-OUT_WIDTH+1)];
      assign overflow_i =
          (sign_ext_i != {(NORM_W-OUT_WIDTH+1){sign_ext_i[NORM_W-OUT_WIDTH]}});
      assign clipped_i = overflow_i ?
          (normalized_i[NORM_W-1] ? {1'b1, {(OUT_WIDTH-1){1'b0}}} :
                                    {1'b0, {(OUT_WIDTH-1){1'b1}}}) :
          normalized_i[OUT_WIDTH-1:0];
    end else begin : gen_no_saturate_i
      assign overflow_i = 1'b0;
      assign clipped_i  = normalized_i[OUT_WIDTH-1:0];
    end
  endgenerate

  // --- Q path ---
  wire signed [ACCUM_W-1:0] rounded_q =
      raw_out_q + {{(ACCUM_W-ROUND_BITS){1'b0}}, 1'b1, {(ROUND_BITS-1){1'b0}}};
  wire signed [NORM_W-1:0] normalized_q = rounded_q[ACCUM_W-1 -: NORM_W];

  wire overflow_q;
  wire signed [OUT_WIDTH-1:0] clipped_q;

  generate
    if (NORM_W > OUT_WIDTH) begin : gen_saturate_q
      wire [NORM_W-OUT_WIDTH:0] sign_ext_q =
          normalized_q[NORM_W-1 -: (NORM_W-OUT_WIDTH+1)];
      assign overflow_q =
          (sign_ext_q != {(NORM_W-OUT_WIDTH+1){sign_ext_q[NORM_W-OUT_WIDTH]}});
      assign clipped_q = overflow_q ?
          (normalized_q[NORM_W-1] ? {1'b1, {(OUT_WIDTH-1){1'b0}}} :
                                    {1'b0, {(OUT_WIDTH-1){1'b1}}}) :
          normalized_q[OUT_WIDTH-1:0];
    end else begin : gen_no_saturate_q
      assign overflow_q = 1'b0;
      assign clipped_q  = normalized_q[OUT_WIDTH-1:0];
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Output register
  // -------------------------------------------------------------------------
  reg signed [OUT_WIDTH-1:0] out_reg_i;
  reg signed [OUT_WIDTH-1:0] out_reg_q;
  reg                        out_valid_reg;
  reg                        out_tlast_reg;

  always @(posedge clk) begin
    if (rst) begin
      out_reg_i     <= {OUT_WIDTH{1'b0}};
      out_reg_q     <= {OUT_WIDTH{1'b0}};
      out_valid_reg <= 1'b0;
      out_tlast_reg <= 1'b0;
    end else if (sample_stb) begin
      out_reg_i     <= clipped_i;
      out_reg_q     <= clipped_q;
      out_valid_reg <= pre_valid;
      out_tlast_reg <= pre_tlast;
    end else if (m_axis_tready) begin
      out_valid_reg <= 1'b0;
    end
  end

  // -------------------------------------------------------------------------
  // AXI-Stream output: pack {I, Q} back to sc16
  // -------------------------------------------------------------------------
  assign m_axis_tdata  = {out_reg_i, out_reg_q};
  assign m_axis_tvalid = out_valid_reg;
  assign m_axis_tlast  = out_tlast_reg;

  assign s_axis_tready = m_axis_tready | ~out_valid_reg;

endmodule

`default_nettype wire
