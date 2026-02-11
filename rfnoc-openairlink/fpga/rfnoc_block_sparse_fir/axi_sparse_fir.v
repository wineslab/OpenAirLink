//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: axi_sparse_fir
//
// Description:
//
//   Sparse FIR filter engine for channel emulation. Instead of a dense
//   systolic FIR with one DSP48 per tap position, this module uses a
//   BRAM-based delay line with NUM_TAPS independently-addressable taps.
//   Each tap reads from the delay line at a runtime-programmable offset
//   and multiplies by a runtime-programmable coefficient.
//
//   This allows covering large delay spreads (e.g., 5 us at 200 MHz =
//   1024 samples) with only NUM_TAPS DSP48 slices instead of 1024.
//
//   Architecture:
//     - Circular buffer delay line stored in BRAM (depth = MAX_DELAY)
//     - NUM_TAPS replicated read ports (one BRAM copy per tap)
//     - NUM_TAPS DSP48 multipliers (sample × coefficient)
//     - Pipelined adder tree to sum all products
//     - Round-and-clip to output width
//
// Parameters:
//
//   IN_WIDTH   : Width of each input sample (I or Q, not combined IQ)
//   OUT_WIDTH  : Width of each output sample
//   COEFF_WIDTH: Width of each tap coefficient
//   NUM_TAPS   : Number of active sparse taps (each with programmable delay)
//   MAX_DELAY  : Maximum delay depth in samples. Determines BRAM depth.
//                Must be a power of 2.
//
// Notes:
//   - Tap delays and coefficients are loaded via parallel register ports
//     (not AXI-Stream reload). The parent module maps ctrlport writes to
//     these ports.
//   - Latency from input to output: 4 clock cycles (BRAM read + multiply +
//     adder tree + output register).
//   - One output sample per input sample (1:1 throughput).
//

module axi_sparse_fir #(
  parameter IN_WIDTH    = 16,
  parameter OUT_WIDTH   = 16,
  parameter COEFF_WIDTH = 16,
  parameter NUM_TAPS    = 4,
  parameter MAX_DELAY   = 1024
)(
  input  wire clk,
  input  wire rst,

  // AXI-Stream Data Input
  input  wire [IN_WIDTH-1:0]  s_axis_tdata,
  input  wire                 s_axis_tlast,
  input  wire                 s_axis_tvalid,
  output wire                 s_axis_tready,

  // AXI-Stream Data Output
  output wire [OUT_WIDTH-1:0] m_axis_tdata,
  output wire                 m_axis_tlast,
  output wire                 m_axis_tvalid,
  input  wire                 m_axis_tready,

  // Tap configuration (active-high load, directly from register logic)
  input  wire [NUM_TAPS*$clog2(MAX_DELAY)-1:0] tap_delays,  // Packed delay values
  input  wire [NUM_TAPS*COEFF_WIDTH-1:0]        tap_coeffs   // Packed coefficient values
);

  // -----------------------------------------------------------------------
  // Local parameters
  // -----------------------------------------------------------------------
  localparam DELAY_W    = $clog2(MAX_DELAY);
  localparam MULT_W     = IN_WIDTH + COEFF_WIDTH;     // Full product width
  // Accumulator needs room for summing NUM_TAPS products
  localparam ACCUM_W    = MULT_W + $clog2(NUM_TAPS);

  // Pipeline depth: BRAM read (1) + multiply (1) + adder tree (1) + output reg (1) = 4
  localparam PIPELINE_DELAY = 4;

  // Flow control: we consume one sample per clock when downstream is ready
  wire sample_stb = s_axis_tvalid & s_axis_tready;

  // -----------------------------------------------------------------------
  // Write pointer for circular buffer
  // -----------------------------------------------------------------------
  reg [DELAY_W-1:0] wr_ptr;

  always @(posedge clk) begin
    if (rst) begin
      wr_ptr <= 0;
    end else if (sample_stb) begin
      wr_ptr <= wr_ptr + 1;
    end
  end

  // -----------------------------------------------------------------------
  // Delay line: NUM_TAPS copies of simple-dual-port BRAM
  //   - All written in parallel with the same sample
  //   - Each read at a different address (wr_ptr - tap_delay[i])
  // -----------------------------------------------------------------------
  wire [IN_WIDTH-1:0] delayed_sample [0:NUM_TAPS-1];

  genvar t;
  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_delay_line

      // Extract this tap's delay value
      wire [DELAY_W-1:0] this_delay = tap_delays[DELAY_W*t +: DELAY_W];

      // Read address: circular buffer offset
      wire [DELAY_W-1:0] rd_addr = wr_ptr - this_delay;

      // Simple dual-port RAM: one write port, one read port
      // Synthesis attribute to force BRAM inference
      (* ram_style = "block" *)
      reg [IN_WIDTH-1:0] delay_mem [0:MAX_DELAY-1];

      // Initialize BRAM to zero to prevent startup noise
      integer m;
      initial begin
        for (m = 0; m < MAX_DELAY; m = m + 1)
          delay_mem[m] = {IN_WIDTH{1'b0}};
      end

      reg [IN_WIDTH-1:0] rd_data;

      // Write port: write new sample on valid strobe
      always @(posedge clk) begin
        if (sample_stb) begin
          delay_mem[wr_ptr] <= s_axis_tdata;
        end
      end

      // Read port: read delayed sample
      always @(posedge clk) begin
        if (sample_stb) begin
          rd_data <= delay_mem[rd_addr];
        end
      end

      // Bypass: when reading the same address being written (delay=0),
      // the BRAM read returns the OLD value (stale/zero). Detect the
      // collision and forward the write data directly.
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

  // -----------------------------------------------------------------------
  // Multiply: each delayed sample × its coefficient
  // -----------------------------------------------------------------------
  reg signed [MULT_W-1:0] products [0:NUM_TAPS-1];

  generate
    for (t = 0; t < NUM_TAPS; t = t + 1) begin : gen_multiply
      wire signed [COEFF_WIDTH-1:0] this_coeff =
          tap_coeffs[COEFF_WIDTH*t +: COEFF_WIDTH];

      always @(posedge clk) begin
        if (rst) begin
          products[t] <= 0;
        end else if (sample_stb) begin
          products[t] <= $signed(delayed_sample[t]) * this_coeff;
        end
      end
    end
  endgenerate

  // -----------------------------------------------------------------------
  // Adder tree: sum all products (pipelined, 1 stage for NUM_TAPS=4)
  //
  // For NUM_TAPS=4:
  //   Stage 1: sum01 = p[0]+p[1], sum23 = p[2]+p[3]
  //   Stage 2: total = sum01 + sum23
  //
  // For generality, we implement a 2-stage adder for 4 taps.
  // -----------------------------------------------------------------------
  reg signed [ACCUM_W-1:0] sum_stage1 [0:1];
  reg signed [ACCUM_W-1:0] sum_stage2;

  always @(posedge clk) begin
    if (rst) begin
      sum_stage1[0] <= 0;
      sum_stage1[1] <= 0;
      sum_stage2    <= 0;
    end else if (sample_stb) begin
      // Stage 1: pairwise addition
      sum_stage1[0] <= $signed(products[0]) + $signed(products[1]);
      sum_stage1[1] <= $signed(products[2]) + $signed(products[3]);
      // Stage 2: final sum (uses stage1 from previous cycle — pipeline!)
      sum_stage2    <= sum_stage1[0] + sum_stage1[1];
    end
  end

  // -----------------------------------------------------------------------
  // Pipeline delay tracking for tlast and tvalid
  // -----------------------------------------------------------------------

  // Total pipeline stages: BRAM read(1) + multiply(1) + adder stage1(1) + adder stage2(1) = 4
  // But adder stage1 and stage2 share pipeline with multiply, so actual delay:
  //   Cycle 0: sample arrives, written to BRAM
  //   Cycle 1: rd_data available (BRAM read latency)
  //   Cycle 2: products[t] available (multiply registered)
  //   Cycle 3: sum_stage1 available
  //   Cycle 4: sum_stage2 available (output)
  // That's 4 cycles of pipeline delay from sample_stb to valid output.

  reg [PIPELINE_DELAY-1:0] valid_sr;
  reg [PIPELINE_DELAY-1:0] tlast_sr;

  always @(posedge clk) begin
    if (rst) begin
      valid_sr <= 0;
      tlast_sr <= 0;
    end else if (sample_stb) begin
      valid_sr <= {valid_sr[PIPELINE_DELAY-2:0], 1'b1};
      tlast_sr <= {tlast_sr[PIPELINE_DELAY-2:0], s_axis_tlast};
    end
  end

  // -----------------------------------------------------------------------
  // Output: normalize fixed-point product, round, and clip to OUT_WIDTH
  // -----------------------------------------------------------------------
  //
  // Coefficients are in Q1.(COEFF_WIDTH-1) format, e.g. Q1.15 for 16-bit.
  // The multiply produces a result with (COEFF_WIDTH-1) fractional bits.
  // We right-shift by (COEFF_WIDTH-1) to normalize back to integer range,
  // with convergent rounding, then saturate into OUT_WIDTH bits.
  //
  // ACCUM_W = IN_WIDTH + COEFF_WIDTH + $clog2(NUM_TAPS)
  // After right-shift by (COEFF_WIDTH-1), effective width is:
  //   NORM_W = ACCUM_W - (COEFF_WIDTH-1) = IN_WIDTH + 1 + $clog2(NUM_TAPS)
  // For defaults: 16 + 1 + 2 = 19 bits, which must be clipped to OUT_WIDTH=16.

  wire signed [ACCUM_W-1:0] raw_out = sum_stage2;
  wire out_valid = valid_sr[PIPELINE_DELAY-1];
  wire out_tlast = tlast_sr[PIPELINE_DELAY-1];

  // Fixed-point normalization: discard (COEFF_WIDTH-1) fractional LSBs
  localparam ROUND_BITS = COEFF_WIDTH - 1;             // Bits to shift out
  localparam NORM_W     = ACCUM_W - ROUND_BITS;        // Width after normalization

  // Rounding: add 0.5 ULP of the bits being discarded
  wire signed [ACCUM_W-1:0] rounded = raw_out + (1 <<< (ROUND_BITS - 1));

  // Extract normalized result (upper NORM_W bits after rounding)
  wire signed [NORM_W-1:0] normalized = rounded[ACCUM_W-1 -: NORM_W];

  // Saturate to OUT_WIDTH bits
  wire overflow;
  wire signed [OUT_WIDTH-1:0] clipped;

  generate
    if (NORM_W > OUT_WIDTH) begin : gen_clip
      // Check if the upper (NORM_W - OUT_WIDTH) bits + sign of OUT_WIDTH result
      // are all the same (sign extension). If not, we have overflow.
      wire [NORM_W-OUT_WIDTH:0] sign_bits = normalized[NORM_W-1 -: (NORM_W-OUT_WIDTH+1)];
      assign overflow = (sign_bits != {(NORM_W-OUT_WIDTH+1){sign_bits[NORM_W-OUT_WIDTH]}});

      // Saturate on overflow
      assign clipped = overflow ?
        (normalized[NORM_W-1] ? {1'b1, {(OUT_WIDTH-1){1'b0}}} :   // Negative saturate
                                {1'b0, {(OUT_WIDTH-1){1'b1}}}) :   // Positive saturate
        normalized[OUT_WIDTH-1:0];
    end else begin : gen_no_clip
      assign overflow = 1'b0;
      assign clipped  = normalized[OUT_WIDTH-1:0];
    end
  endgenerate

  // Output register
  reg signed [OUT_WIDTH-1:0] out_reg;
  reg                        out_valid_reg;
  reg                        out_tlast_reg;

  always @(posedge clk) begin
    if (rst) begin
      out_reg       <= 0;
      out_valid_reg <= 0;
      out_tlast_reg <= 0;
    end else if (sample_stb) begin
      out_reg       <= clipped;
      out_valid_reg <= out_valid;
      out_tlast_reg <= out_tlast;
    end else if (m_axis_tready) begin
      out_valid_reg <= 1'b0;
    end
  end

  // -----------------------------------------------------------------------
  // AXI-Stream output
  // -----------------------------------------------------------------------
  assign m_axis_tdata  = out_reg;
  assign m_axis_tvalid = out_valid_reg;
  assign m_axis_tlast  = out_tlast_reg;

  // Backpressure: accept input when downstream is ready or output is idle
  assign s_axis_tready = m_axis_tready | ~out_valid_reg;

endmodule
