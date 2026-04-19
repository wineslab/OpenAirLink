//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_complex_sparse_fir
//
// Description:
//
//   Complex Sparse FIR filter engine for channel emulation. Instead of a dense
//   systolic FIR with one DSP48 per tap position, this module uses a
//   BRAM-based circular delay line with NUM_TAPS independently-addressable
//   taps. Each tap reads from the delay line at a runtime-programmable
//   offset and multiplies by a runtime-programmable coefficient.
//
//   This allows covering large delay spreads (e.g. 8.33 us at 122.88 MHz
//   CE clock = 1024 samples) with only NUM_TAPS DSP48 slices, instead of
//   one DSP48 per delay sample as in a dense FIR.
//
//   Architecture:
//     - Circular buffer delay line stored in BRAM (depth = MAX_DELAY)
//     - NUM_TAPS replicated read ports (one BRAM copy per tap)
//     - NUM_TAPS DSP48 multipliers (delayed_sample * coefficient)
//     - Fully pipelined, balanced log2(NUM_TAPS)-stage adder tree
//     - Fixed-point normalization: round + saturate to output width
//
// Parameters:
//
//   IN_WIDTH    : Width of each input sample (I or Q independently)
//   OUT_WIDTH   : Width of each output sample
//   COEFF_WIDTH : Width of each tap coefficient (signed, Q1.(COEFF_WIDTH-1))
//   NUM_TAPS    : Number of active sparse taps.
//                 Must be a power of 2 in the range [1, 32].
//   MAX_DELAY   : Maximum delay in samples (circular buffer depth).
//                 Must be a power of 2.
//
// Pipeline latency (input sample_stb -> m_axis_tvalid asserted):
//
//   TREE_STAGES    = max(clog2(NUM_TAPS), 1)
//   PIPELINE_DELAY = 1(BRAM) + 1(tap_data_reg) + 1(multiply) + TREE_STAGES + 1(output_reg)
//
//   NUM_TAPS =  1 : PIPELINE_DELAY = 5  (TREE_STAGES = 1, passthrough node)
//   NUM_TAPS =  2 : PIPELINE_DELAY = 5
//   NUM_TAPS =  4 : PIPELINE_DELAY = 6
//   NUM_TAPS =  8 : PIPELINE_DELAY = 7
//   NUM_TAPS = 16 : PIPELINE_DELAY = 8
//   NUM_TAPS = 32 : PIPELINE_DELAY = 9
//
// Adder tree flat-array layout:
//
//   The tree has TREE_STAGES registered stages stored in a flat array.
//   Stage 0 sums pairs directly from products[]; stage s (s>0) sums pairs
//   from stage s-1. The closed-form flat index for stage s, node n is:
//
//     flat_index(s, n) = (NUM_TAPS - (NUM_TAPS >> s)) + n
//
//   The final accumulated result is at flat_index(TREE_STAGES-1, 0).
//
//   Example, NUM_TAPS=8, TREE_STAGES=3 (flat indices in brackets):
//
//     products:   p0  p1  p2  p3  p4  p5  p6  p7
//                  |   |   |   |   |   |   |   |
//     Stage 0:  [0]p+p [1]p+p [2]p+p [3]p+p      <- offset = 8-(8>>0) = 0
//                  |           |
//     Stage 1:  [4]s+s      [5]s+s                <- offset = 8-(8>>1) = 4
//                  |
//     Stage 2:  [6] <root>                        <- offset = 8-(8>>2) = 6
//
// Notes:
//   - Tap delays and coefficients are loaded via parallel register ports.
//     The parent RFNoC block maps ctrlport writes to these inputs.
//   - BRAM write-read collision (delay=0) is detected and forwarded.
//   - 1:1 throughput: one output sample per input sample.
//

`default_nettype none

module axi_complex_sparse_fir #(
  parameter IN_WIDTH    = 16,
  parameter OUT_WIDTH   = 16,
  parameter COEFF_WIDTH = 16,
  parameter NUM_TAPS    = 4,    // Power of 2, 1 <= NUM_TAPS <= 32
  parameter MAX_DELAY   = 1024  // Power of 2
)(
  input  wire clk,
  input  wire rst,

  // AXI-Stream input
  input  wire [IN_WIDTH-1:0]  s_axis_tdata,
  input  wire                 s_axis_tlast,
  input  wire                 s_axis_tvalid,
  output wire                 s_axis_tready,

  // AXI-Stream output
  output wire [OUT_WIDTH-1:0] m_axis_tdata,
  output wire                 m_axis_tlast,
  output wire                 m_axis_tvalid,
  input  wire                 m_axis_tready,

  // Tap configuration (driven by parent ctrlport register logic)
  // Tap i occupies bits [DELAY_W*(i+1)-1   : DELAY_W*i]   of tap_delays
  //                 and [COEFF_WIDTH*(i+1)-1 : COEFF_WIDTH*i] of tap_coeffs
  input  wire [NUM_TAPS*$clog2(MAX_DELAY)-1:0] tap_delays,
  input  wire [NUM_TAPS*COEFF_WIDTH-1:0]        tap_coeffs
);

  // -------------------------------------------------------------------------
  // Local parameters
  // -------------------------------------------------------------------------
  localparam DELAY_W = $clog2(MAX_DELAY);

  // Full-precision product width: IN_WIDTH + COEFF_WIDTH bits
  localparam MULT_W  = IN_WIDTH + COEFF_WIDTH;

  // Accumulator width: log2(NUM_TAPS) guard bits prevent overflow when
  // summing NUM_TAPS products. For NUM_TAPS=1, clog2(1)=0 so ACCUM_W=MULT_W.
  localparam ACCUM_W = MULT_W + $clog2(NUM_TAPS);

  // Adder tree depth: log2(NUM_TAPS) registered addition stages.
  // Forced to minimum 1 so NUM_TAPS=1 still gets one registered pass-through
  // stage, making PIPELINE_DELAY identical for NUM_TAPS=1 and NUM_TAPS=2.
  localparam TREE_STAGES = ($clog2(NUM_TAPS) > 0) ? $clog2(NUM_TAPS) : 1;

  // Total pipeline stages from sample_stb to output register load:
  //   1 (BRAM read) + 1 (tap_data_reg) + 1 (multiply) + TREE_STAGES (adder tree) + 1 (out reg)
  localparam PIPELINE_DELAY = 4 + TREE_STAGES;

  // Shift-register depth for tvalid/tlast: covers all stages except the
  // output register (which handles its own flag storage).
  localparam SR_DEPTH = PIPELINE_DELAY - 1;

  // Fixed-point normalization parameters:
  // Coefficients are Q1.(COEFF_WIDTH-1), so the product has (COEFF_WIDTH-1)
  // fractional bits that must be shifted out.
  localparam ROUND_BITS = COEFF_WIDTH - 1;
  localparam NORM_W     = ACCUM_W - ROUND_BITS;

  // Flat adder-tree array size: for a power-of-2 NUM_TAPS the tree uses
  // exactly NUM_TAPS-1 entries. We allocate NUM_TAPS for safe indexing.
  localparam TREE_FLAT_SIZE = NUM_TAPS;

  // -------------------------------------------------------------------------
  // Flow control
  // -------------------------------------------------------------------------
  wire sample_stb = s_axis_tvalid & s_axis_tready;

  // -------------------------------------------------------------------------
  // Write pointer: advances by 1 each time a sample is accepted
  // -------------------------------------------------------------------------
  reg [DELAY_W-1:0] wr_ptr;

  always @(posedge clk) begin
    if (rst)
      wr_ptr <= {DELAY_W{1'b0}};
    else if (sample_stb)
      wr_ptr <= wr_ptr + 1'b1;
  end

  // -------------------------------------------------------------------------
  // Delay line: NUM_TAPS independent BRAM circular buffers
  //
  // All copies are written simultaneously with the current input sample.
  // Each copy is read at (wr_ptr - tap_delay[t]), delivering the sample
  // from tap_delay[t] cycles ago with no DSP resource cost.
  //
  // Write-read collision: when tap_delay[t]=0 the read and write addresses
  // are equal. A synchronous BRAM returns stale data in this case. The
  // collision is detected one cycle early and the write data is forwarded.
  // -------------------------------------------------------------------------
  wire [IN_WIDTH-1:0] delayed_sample [0:NUM_TAPS-1];

  genvar t;
  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_delay_line

      wire [DELAY_W-1:0] this_delay = tap_delays[DELAY_W*t +: DELAY_W];
      wire [DELAY_W-1:0] rd_addr    = wr_ptr - this_delay;

      (* ram_style = "block" *)
      reg [IN_WIDTH-1:0] delay_mem [0:MAX_DELAY-1];

      integer m;
      initial begin
        for (m = 0; m < MAX_DELAY; m = m + 1)
          delay_mem[m] = {IN_WIDTH{1'b0}};
      end

      reg [IN_WIDTH-1:0] rd_data;

      always @(posedge clk) begin
        if (sample_stb)
          delay_mem[wr_ptr] <= s_axis_tdata;
      end

      always @(posedge clk) begin
        if (sample_stb)
          rd_data <= delay_mem[rd_addr];
      end

      wire rd_wr_collision = (rd_addr == wr_ptr);
      reg  rd_wr_collision_r;
      reg  [IN_WIDTH-1:0] wr_data_r;

      always @(posedge clk) begin
        if (rst) begin
          rd_wr_collision_r <= 1'b0;
          wr_data_r         <= {IN_WIDTH{1'b0}};
        end else if (sample_stb) begin
          rd_wr_collision_r <= rd_wr_collision;
          wr_data_r         <= s_axis_tdata;
        end
      end

      assign delayed_sample[t] = rd_wr_collision_r ? wr_data_r : rd_data;

    end
  endgenerate

  // -------------------------------------------------------------------------
  // Tap data register: breaks the BRAM-output -> bypass-MUX -> DSP-input
  // combinational path into two registered hops, fixing timing closure.
  // -------------------------------------------------------------------------
  reg [IN_WIDTH-1:0] tap_data [0:NUM_TAPS-1];

  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_tap_data_reg
      always @(posedge clk) begin
        if (rst)
          tap_data[t] <= {IN_WIDTH{1'b0}};
        else if (sample_stb)
          tap_data[t] <= delayed_sample[t];
      end
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Multiply: products[t] = tap_data[t] * tap_coeff[t], registered
  // -------------------------------------------------------------------------
  reg signed [MULT_W-1:0] products [0:NUM_TAPS-1];

  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_multiply
      wire signed [COEFF_WIDTH-1:0] this_coeff =
          tap_coeffs[COEFF_WIDTH*t +: COEFF_WIDTH];

      always @(posedge clk) begin
        if (rst)
          products[t] <= {MULT_W{1'b0}};
        else if (sample_stb)
          products[t] <= $signed(tap_data[t]) * this_coeff;
      end
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Adder tree: TREE_STAGES registered pairwise-sum stages
  //
  // Flat array layout uses closed-form stage offset:
  //   offset(s) = NUM_TAPS - (NUM_TAPS >> s)
  //   node (s, n) -> flat[ offset(s) + n ]
  //
  // Stage 0: inputs are products[2n] and products[2n+1], sign-extended to ACCUM_W.
  // Stage s>0: inputs are flat[offset(s-1)+2n] and flat[offset(s-1)+2n+1].
  //
  // For NUM_TAPS=1 (TREE_STAGES=1): stage 0 has one node with no addition
  // partner; the node passes products[0] through (sign-extended).
  //
  // For all other power-of-2 NUM_TAPS: every node at every stage has exactly
  // two inputs (N_PREV is always even), so the passthrough branch in
  // gen_passthrough_s is unreachable but retained for safety.
  // -------------------------------------------------------------------------
  reg signed [ACCUM_W-1:0] tree_flat [0:TREE_FLAT_SIZE-1];

  genvar s, n;
  generate

    for (s = 0; s < TREE_STAGES; s = s + 1) begin : gen_tree_stage

      // Number of output nodes at this stage.
      // For s=0: NUM_TAPS/2 nodes. For s=k: NUM_TAPS/2^(k+1) nodes.
      // Clamped to 1 to handle NUM_TAPS=1 (where NUM_TAPS >> (s+1) = 0).
      localparam integer N_OUT    = ((NUM_TAPS >> (s+1)) > 0) ?
                                      (NUM_TAPS >> (s+1)) : 1;
      // Flat-array write offset for this stage
      localparam integer DST_BASE = NUM_TAPS - (NUM_TAPS >> s);

      for (n = 0; n < N_OUT; n = n + 1) begin : gen_tree_node

        if (s == 0) begin : gen_stage0

          if (2*n + 1 < NUM_TAPS) begin : gen_add0
            // Normal pairwise addition: sign-extend products to ACCUM_W then add
            always @(posedge clk) begin
              if (rst)
                tree_flat[DST_BASE + n] <= {ACCUM_W{1'b0}};
              else if (sample_stb)
                tree_flat[DST_BASE + n] <=
                    $signed({{(ACCUM_W-MULT_W){products[2*n  ][MULT_W-1]}}, products[2*n  ]}) +
                    $signed({{(ACCUM_W-MULT_W){products[2*n+1][MULT_W-1]}}, products[2*n+1]});
            end
          end else begin : gen_passthrough0
            // NUM_TAPS=1: single tap, no addition partner; pass through
            always @(posedge clk) begin
              if (rst)
                tree_flat[DST_BASE + n] <= {ACCUM_W{1'b0}};
              else if (sample_stb)
                tree_flat[DST_BASE + n] <=
                    $signed({{(ACCUM_W-MULT_W){products[0][MULT_W-1]}}, products[0]});
            end
          end

        end else begin : gen_stage_s

          // Source offset: nodes at stage s-1
          localparam integer SRC_BASE = NUM_TAPS - (NUM_TAPS >> (s-1));
          // Number of nodes at stage s-1 (always even for power-of-2 NUM_TAPS)
          localparam integer N_PREV   = ((NUM_TAPS >> s) > 0) ? (NUM_TAPS >> s) : 1;

          if (2*n + 1 < N_PREV) begin : gen_add_s
            always @(posedge clk) begin
              if (rst)
                tree_flat[DST_BASE + n] <= {ACCUM_W{1'b0}};
              else if (sample_stb)
                tree_flat[DST_BASE + n] <=
                    tree_flat[SRC_BASE + 2*n] +
                    tree_flat[SRC_BASE + 2*n + 1];
            end
          end else begin : gen_passthrough_s
            // Safety passthrough (unreachable for power-of-2 NUM_TAPS >= 2)
            always @(posedge clk) begin
              if (rst)
                tree_flat[DST_BASE + n] <= {ACCUM_W{1'b0}};
              else if (sample_stb)
                tree_flat[DST_BASE + n] <= tree_flat[SRC_BASE + 2*n];
            end
          end

        end // gen_stage_s

      end // gen_tree_node
    end // gen_tree_stage

  endgenerate

  // Root of the adder tree: stage (TREE_STAGES-1), node 0
  localparam ROOT_IDX = NUM_TAPS - (NUM_TAPS >> (TREE_STAGES - 1));
  wire signed [ACCUM_W-1:0] raw_out = tree_flat[ROOT_IDX];

  // -------------------------------------------------------------------------
  // Pipeline flag shift register
  // Tracks tvalid and tlast through BRAM(1) + multiply(1) + tree(TREE_STAGES).
  // The output register handles its own valid/tlast storage as the final stage.
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
  // Output normalization: discard fractional bits, round, saturate
  //
  // Coefficients are Q1.(COEFF_WIDTH-1): the multiply accumulates
  // (COEFF_WIDTH-1) fractional bits. Right-shift by ROUND_BITS to normalize
  // back to integer range. Convergent (half-up) rounding: add 0.5 ULP
  // before the shift. Saturate to signed OUT_WIDTH via overflow detection.
  // Residual truncation error is bounded by 0.5 LSB on I and Q each.
  // -------------------------------------------------------------------------
  wire signed [ACCUM_W-1:0] rounded =
      raw_out + {{(ACCUM_W-ROUND_BITS){1'b0}}, 1'b1, {(ROUND_BITS-1){1'b0}}};

  wire signed [NORM_W-1:0] normalized = rounded[ACCUM_W-1 -: NORM_W];

  wire overflow;
  wire signed [OUT_WIDTH-1:0] clipped;

  generate
    if (NORM_W > OUT_WIDTH) begin : gen_saturate
      // Overflow when the upper (NORM_W-OUT_WIDTH+1) bits are not all equal
      wire [NORM_W-OUT_WIDTH:0] sign_ext =
          normalized[NORM_W-1 -: (NORM_W-OUT_WIDTH+1)];
      assign overflow =
          (sign_ext != {(NORM_W-OUT_WIDTH+1){sign_ext[NORM_W-OUT_WIDTH]}});
      assign clipped = overflow ?
          (normalized[NORM_W-1] ? {1'b1, {(OUT_WIDTH-1){1'b0}}} :   // saturate negative
                                  {1'b0, {(OUT_WIDTH-1){1'b1}}}) :   // saturate positive
          normalized[OUT_WIDTH-1:0];
    end else begin : gen_no_saturate
      assign overflow = 1'b0;
      assign clipped  = normalized[OUT_WIDTH-1:0];
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Output register: final pipeline stage
  // -------------------------------------------------------------------------
  reg signed [OUT_WIDTH-1:0] out_reg;
  reg                        out_valid_reg;
  reg                        out_tlast_reg;

  always @(posedge clk) begin
    if (rst) begin
      out_reg       <= {OUT_WIDTH{1'b0}};
      out_valid_reg <= 1'b0;
      out_tlast_reg <= 1'b0;
    end else if (sample_stb) begin
      out_reg       <= clipped;
      out_valid_reg <= pre_valid;
      out_tlast_reg <= pre_tlast;
    end else if (m_axis_tready) begin
      out_valid_reg <= 1'b0;
    end
  end

  // -------------------------------------------------------------------------
  // AXI-Stream output
  // -------------------------------------------------------------------------
  assign m_axis_tdata  = out_reg;
  assign m_axis_tvalid = out_valid_reg;
  assign m_axis_tlast  = out_tlast_reg;

  // Accept input when downstream is ready, or when the output register is idle
  assign s_axis_tready = m_axis_tready | ~out_valid_reg;

endmodule

`default_nettype wire
