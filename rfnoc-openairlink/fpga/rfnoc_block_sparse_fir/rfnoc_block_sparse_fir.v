//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: rfnoc_block_sparse_fir
//
// Description:
//
//   Sparse FIR filter RFNoC block for channel emulation. Processes sc16
//   (signed complex 16-bit) IQ samples. Each IQ sample contains I in the
//   upper 16 bits and Q in the lower 16 bits.
//
//   Unlike the standard RFNoC FIR filter which uses one DSP48 per tap
//   position, this block uses a BRAM-based delay line with only NUM_TAPS
//   independently-addressable taps. Each tap has a runtime-programmable
//   delay offset and coefficient, allowing large delay spread coverage
//   (e.g., 5 us) with minimal DSP48 usage.
//
//   Resources per block instance (NUM_TAPS taps, MAX_DELAY=1024):
//     - DSP48E2: 2*NUM_TAPS (one multiplier per tap per I/Q path)
//     - BRAM18:  2*NUM_TAPS (one delay-line copy per tap per I/Q path)
//     - Compare to dense FIR: 82 DSP48 for just 41 taps!
//
// Parameters:
//
//   THIS_PORTID : Control crossbar port to which this block is connected
//   CHDR_W      : AXIS-CHDR data bus width
//   MTU         : Maximum transmission unit
//   NUM_TAPS    : Number of sparse taps (default 4)
//   MAX_DELAY   : Maximum delay line depth in samples (default 1024, power of 2)
//   COEFF_WIDTH : Bit-width of tap coefficients (default 16)
//

`default_nettype none


module rfnoc_block_sparse_fir #(
  parameter [9:0] THIS_PORTID = 10'd0,
  parameter       CHDR_W      = 64,
  parameter [5:0] MTU         = 10,
  parameter       NUM_TAPS    = 4,
  parameter       MAX_DELAY   = 1024,
  parameter       COEFF_WIDTH = 16
)(
  // RFNoC Framework Clocks and Resets
  input  wire                   rfnoc_chdr_clk,
  input  wire                   rfnoc_ctrl_clk,
  input  wire                   ce_clk,
  // RFNoC Backend Interface
  input  wire [511:0]           rfnoc_core_config,
  output wire [511:0]           rfnoc_core_status,
  // AXIS-CHDR Input Ports (from framework)
  input  wire [(1)*CHDR_W-1:0]  s_rfnoc_chdr_tdata,
  input  wire [(1)-1:0]         s_rfnoc_chdr_tlast,
  input  wire [(1)-1:0]         s_rfnoc_chdr_tvalid,
  output wire [(1)-1:0]         s_rfnoc_chdr_tready,
  // AXIS-CHDR Output Ports (to framework)
  output wire [(1)*CHDR_W-1:0]  m_rfnoc_chdr_tdata,
  output wire [(1)-1:0]         m_rfnoc_chdr_tlast,
  output wire [(1)-1:0]         m_rfnoc_chdr_tvalid,
  input  wire [(1)-1:0]         m_rfnoc_chdr_tready,
  // AXIS-Ctrl Input Port (from framework)
  input  wire [31:0]            s_rfnoc_ctrl_tdata,
  input  wire                   s_rfnoc_ctrl_tlast,
  input  wire                   s_rfnoc_ctrl_tvalid,
  output wire                   s_rfnoc_ctrl_tready,
  // AXIS-Ctrl Output Port (to framework)
  output wire [31:0]            m_rfnoc_ctrl_tdata,
  output wire                   m_rfnoc_ctrl_tlast,
  output wire                   m_rfnoc_ctrl_tvalid,
  input  wire                   m_rfnoc_ctrl_tready
);

  `include "rfnoc_sparse_fir_regs.vh"

  localparam COMPAT_MAJOR = 16'h1;
  localparam COMPAT_MINOR = 16'h0;

  localparam IN_WIDTH  = 16;  // per I or Q component
  localparam OUT_WIDTH = 16;
  localparam ITEM_W    = 32;  // sc16 = 32 bits (I[31:16] + Q[15:0])
  localparam DELAY_W   = $clog2(MAX_DELAY);

  //---------------------------------------------------------------------------
  // Signal Declarations
  //---------------------------------------------------------------------------

  // Clocks and Resets
  wire               ctrlport_clk;
  wire               ctrlport_rst;
  wire               axis_data_clk;
  wire               axis_data_rst;

  // CtrlPort Master
  wire               m_ctrlport_req_wr;
  wire               m_ctrlport_req_rd;
  wire [19:0]        m_ctrlport_req_addr;
  wire [31:0]        m_ctrlport_req_data;
  reg                m_ctrlport_resp_ack;
  reg  [31:0]        m_ctrlport_resp_data;

  // Payload Stream to User Logic: in
  wire [ITEM_W-1:0]  m_in_payload_tdata;
  wire               m_in_payload_tkeep;
  wire               m_in_payload_tlast;
  wire               m_in_payload_tvalid;
  wire               m_in_payload_tready;

  // Context Stream to User Logic: in
  wire [CHDR_W-1:0]  m_in_context_tdata;
  wire [3:0]         m_in_context_tuser;
  wire               m_in_context_tlast;
  wire               m_in_context_tvalid;
  wire               m_in_context_tready;

  // Payload Stream from User Logic: out
  wire [ITEM_W-1:0]  s_out_payload_tdata;
  wire               s_out_payload_tkeep;
  wire               s_out_payload_tlast;
  wire               s_out_payload_tvalid;
  wire               s_out_payload_tready;

  // Context Stream from User Logic: out
  wire [CHDR_W-1:0]  s_out_context_tdata;
  wire [3:0]         s_out_context_tuser;
  wire               s_out_context_tlast;
  wire               s_out_context_tvalid;
  wire               s_out_context_tready;


  //---------------------------------------------------------------------------
  // NoC Shell
  //---------------------------------------------------------------------------

  noc_shell_sparse_fir #(
    .CHDR_W      (CHDR_W),
    .THIS_PORTID (THIS_PORTID),
    .MTU         (MTU)
  ) noc_shell_sparse_fir_i (
    // Framework Interface
    .rfnoc_chdr_clk      (rfnoc_chdr_clk),
    .rfnoc_ctrl_clk      (rfnoc_ctrl_clk),
    .ce_clk              (ce_clk),
    .rfnoc_chdr_rst      (),
    .rfnoc_ctrl_rst      (),
    .ce_rst              (),
    .rfnoc_core_config   (rfnoc_core_config),
    .rfnoc_core_status   (rfnoc_core_status),
    // CHDR Input Ports (from framework)
    .s_rfnoc_chdr_tdata  (s_rfnoc_chdr_tdata),
    .s_rfnoc_chdr_tlast  (s_rfnoc_chdr_tlast),
    .s_rfnoc_chdr_tvalid (s_rfnoc_chdr_tvalid),
    .s_rfnoc_chdr_tready (s_rfnoc_chdr_tready),
    // CHDR Output Ports (to framework)
    .m_rfnoc_chdr_tdata  (m_rfnoc_chdr_tdata),
    .m_rfnoc_chdr_tlast  (m_rfnoc_chdr_tlast),
    .m_rfnoc_chdr_tvalid (m_rfnoc_chdr_tvalid),
    .m_rfnoc_chdr_tready (m_rfnoc_chdr_tready),
    // AXIS-Ctrl Input Port (from framework)
    .s_rfnoc_ctrl_tdata  (s_rfnoc_ctrl_tdata),
    .s_rfnoc_ctrl_tlast  (s_rfnoc_ctrl_tlast),
    .s_rfnoc_ctrl_tvalid (s_rfnoc_ctrl_tvalid),
    .s_rfnoc_ctrl_tready (s_rfnoc_ctrl_tready),
    // AXIS-Ctrl Output Port (to framework)
    .m_rfnoc_ctrl_tdata  (m_rfnoc_ctrl_tdata),
    .m_rfnoc_ctrl_tlast  (m_rfnoc_ctrl_tlast),
    .m_rfnoc_ctrl_tvalid (m_rfnoc_ctrl_tvalid),
    .m_rfnoc_ctrl_tready (m_rfnoc_ctrl_tready),
    // Client Interface
    .ctrlport_clk              (ctrlport_clk),
    .ctrlport_rst              (ctrlport_rst),
    .m_ctrlport_req_wr         (m_ctrlport_req_wr),
    .m_ctrlport_req_rd         (m_ctrlport_req_rd),
    .m_ctrlport_req_addr       (m_ctrlport_req_addr),
    .m_ctrlport_req_data       (m_ctrlport_req_data),
    .m_ctrlport_resp_ack       (m_ctrlport_resp_ack),
    .m_ctrlport_resp_data      (m_ctrlport_resp_data),
    .axis_data_clk (axis_data_clk),
    .axis_data_rst (axis_data_rst),
    // Payload Stream to User Logic: in
    .m_in_payload_tdata  (m_in_payload_tdata),
    .m_in_payload_tkeep  (m_in_payload_tkeep),
    .m_in_payload_tlast  (m_in_payload_tlast),
    .m_in_payload_tvalid (m_in_payload_tvalid),
    .m_in_payload_tready (m_in_payload_tready),
    // Context Stream to User Logic: in
    .m_in_context_tdata  (m_in_context_tdata),
    .m_in_context_tuser  (m_in_context_tuser),
    .m_in_context_tlast  (m_in_context_tlast),
    .m_in_context_tvalid (m_in_context_tvalid),
    .m_in_context_tready (m_in_context_tready),
    // Payload Stream from User Logic: out
    .s_out_payload_tdata  (s_out_payload_tdata),
    .s_out_payload_tkeep  (s_out_payload_tkeep),
    .s_out_payload_tlast  (s_out_payload_tlast),
    .s_out_payload_tvalid (s_out_payload_tvalid),
    .s_out_payload_tready (s_out_payload_tready),
    // Context Stream from User Logic: out
    .s_out_context_tdata  (s_out_context_tdata),
    .s_out_context_tuser  (s_out_context_tuser),
    .s_out_context_tlast  (s_out_context_tlast),
    .s_out_context_tvalid (s_out_context_tvalid),
    .s_out_context_tready (s_out_context_tready)
  );


  //---------------------------------------------------------------------------
  // User Registers
  //---------------------------------------------------------------------------
  //
  // Tap delay and coefficient registers. Each tap has two 32-bit registers:
  //   - Delay (in samples, unsigned, 0 .. MAX_DELAY-1)
  //   - Coefficient (signed, COEFF_WIDTH bits)
  //
  // These are in the ctrlport_clk domain. Since both control and data
  // interfaces use the ce clock domain, no CDC is needed.
  //---------------------------------------------------------------------------

  reg [DELAY_W-1:0]      reg_tap_delay [0:NUM_TAPS-1];
  reg [COEFF_WIDTH-1:0]  reg_tap_coeff [0:NUM_TAPS-1];

  // Pack tap config for the axi_sparse_fir cores
  wire [NUM_TAPS*DELAY_W-1:0]      packed_delays;
  wire [NUM_TAPS*COEFF_WIDTH-1:0]  packed_coeffs;

  genvar g;
  generate
    for (g = 0; g < NUM_TAPS; g = g + 1) begin : gen_pack
      assign packed_delays[DELAY_W*g +: DELAY_W]         = reg_tap_delay[g];
      assign packed_coeffs[COEFF_WIDTH*g +: COEFF_WIDTH]  = reg_tap_coeff[g];
    end
  endgenerate

  // Initialize tap registers
  integer k;
  initial begin
    for (k = 0; k < NUM_TAPS; k = k + 1) begin
      reg_tap_delay[k] = 0;
      reg_tap_coeff[k] = 0;
    end
    // Default: first tap at delay=0 with unity gain (impulse passthrough)
    reg_tap_coeff[0] = {1'b0, {(COEFF_WIDTH-1){1'b1}}};  // Max positive
  end

  // ---- Address decode (combinational) ----
  // REG_TAP_STRIDE = 0x08 = 8, so dividing by stride is a right-shift by 3,
  // and the intra-tap offset is bits [2:0]. Bit 2 selects delay vs coeff,
  // bits [1:0] must be zero for a valid 32-bit-aligned access.
  wire [SPARSE_FIR_ADDR_W-1:0] local_addr  = m_ctrlport_req_addr[SPARSE_FIR_ADDR_W-1:0];
  wire                          in_tap_rgn  = (local_addr >= REG_TAP_BASE[SPARSE_FIR_ADDR_W-1:0]);
  wire [SPARSE_FIR_ADDR_W-1:0] tap_offset  = local_addr - REG_TAP_BASE[SPARSE_FIR_ADDR_W-1:0];
  wire [4:0]                    tap_idx     = tap_offset[7:3]; // divide by 8
  wire                          is_coeff    = tap_offset[2];   // 0 = delay, 1 = coeff
  wire                          tap_aligned = ~(|tap_offset[1:0]); // bits [1:0] == 0
  wire                          tap_valid   = in_tap_rgn & tap_aligned & (tap_idx < NUM_TAPS);

  // Register read/write logic
  always @(posedge ctrlport_clk) begin
    if (ctrlport_rst) begin
      m_ctrlport_resp_ack  <= 1'b0;
      m_ctrlport_resp_data <= 32'b0;
      for (k = 0; k < NUM_TAPS; k = k + 1) begin
        reg_tap_delay[k] <= 0;
        reg_tap_coeff[k] <= 0;
      end
      // Re-apply impulse default after reset
      reg_tap_coeff[0] <= {1'b0, {(COEFF_WIDTH-1){1'b1}}};
    end else begin
      // Default: no response
      m_ctrlport_resp_ack  <= 1'b0;
      m_ctrlport_resp_data <= 32'b0;

      // Handle reads
      if (m_ctrlport_req_rd) begin
        case (local_addr)
          REG_COMPAT_NUM[SPARSE_FIR_ADDR_W-1:0]: begin
            m_ctrlport_resp_data <= {COMPAT_MAJOR, COMPAT_MINOR};
            m_ctrlport_resp_ack  <= 1'b1;
          end
          REG_NUM_TAPS[SPARSE_FIR_ADDR_W-1:0]: begin
            m_ctrlport_resp_data <= NUM_TAPS;
            m_ctrlport_resp_ack  <= 1'b1;
          end
          REG_MAX_DELAY[SPARSE_FIR_ADDR_W-1:0]: begin
            m_ctrlport_resp_data <= MAX_DELAY;
            m_ctrlport_resp_ack  <= 1'b1;
          end
          default: begin
            if (tap_valid) begin
              m_ctrlport_resp_ack <= 1'b1;
              if (is_coeff)
                m_ctrlport_resp_data <= {{(32-COEFF_WIDTH){1'b0}}, reg_tap_coeff[tap_idx]};
              else
                m_ctrlport_resp_data <= {{(32-DELAY_W){1'b0}}, reg_tap_delay[tap_idx]};
            end
          end
        endcase
      end

      // Handle writes
      if (m_ctrlport_req_wr) begin
        if (tap_valid) begin
          m_ctrlport_resp_ack <= 1'b1;
          if (is_coeff)
            reg_tap_coeff[tap_idx] <= m_ctrlport_req_data[COEFF_WIDTH-1:0];
          else
            reg_tap_delay[tap_idx] <= m_ctrlport_req_data[DELAY_W-1:0];
        end
      end
    end
  end


  //---------------------------------------------------------------------------
  // User Logic: Sparse FIR Filter (I and Q paths)
  //---------------------------------------------------------------------------

  // Pipeline input through a small FIFO (same pattern as shiftright block)
  wire [ITEM_W-1:0] pipe_in_tdata;
  wire              pipe_in_tvalid, pipe_in_tlast;
  wire              pipe_in_tready;

  axi_fifo #(
    .WIDTH (ITEM_W + 1),
    .SIZE  (0)
  ) pipeline_in_fifo (
    .clk      (ce_clk),
    .reset    (ctrlport_rst),
    .clear    (1'b0),
    .i_tdata  ({m_in_payload_tlast, m_in_payload_tdata}),
    .i_tvalid (m_in_payload_tvalid),
    .i_tready (m_in_payload_tready),
    .o_tdata  ({pipe_in_tlast, pipe_in_tdata}),
    .o_tvalid (pipe_in_tvalid),
    .o_tready (pipe_in_tready)
  );

  // Split IQ: I = upper 16 bits, Q = lower 16 bits
  wire [IN_WIDTH-1:0] sample_i = pipe_in_tdata[2*IN_WIDTH-1 : IN_WIDTH];
  wire [IN_WIDTH-1:0] sample_q = pipe_in_tdata[IN_WIDTH-1   : 0];

  // FIR output wires
  wire [OUT_WIDTH-1:0] fir_out_i, fir_out_q;
  wire fir_out_i_valid, fir_out_i_tlast;
  wire fir_out_q_valid, fir_out_q_tlast;
  wire fir_in_i_ready, fir_in_q_ready;

  // I-path sparse FIR
  axi_sparse_fir #(
    .IN_WIDTH    (IN_WIDTH),
    .OUT_WIDTH   (OUT_WIDTH),
    .COEFF_WIDTH (COEFF_WIDTH),
    .NUM_TAPS    (NUM_TAPS),
    .MAX_DELAY   (MAX_DELAY)
  ) sparse_fir_i (
    .clk           (ce_clk),
    .rst           (ctrlport_rst),
    .s_axis_tdata  (sample_i),
    .s_axis_tlast  (pipe_in_tlast),
    .s_axis_tvalid (pipe_in_tvalid),
    .s_axis_tready (fir_in_i_ready),
    .m_axis_tdata  (fir_out_i),
    .m_axis_tlast  (fir_out_i_tlast),
    .m_axis_tvalid (fir_out_i_valid),
    .m_axis_tready (s_out_payload_tready),
    .tap_delays    (packed_delays),
    .tap_coeffs    (packed_coeffs)
  );

  // Q-path sparse FIR
  axi_sparse_fir #(
    .IN_WIDTH    (IN_WIDTH),
    .OUT_WIDTH   (OUT_WIDTH),
    .COEFF_WIDTH (COEFF_WIDTH),
    .NUM_TAPS    (NUM_TAPS),
    .MAX_DELAY   (MAX_DELAY)
  ) sparse_fir_q (
    .clk           (ce_clk),
    .rst           (ctrlport_rst),
    .s_axis_tdata  (sample_q),
    .s_axis_tlast  (pipe_in_tlast),
    .s_axis_tvalid (pipe_in_tvalid),
    .s_axis_tready (fir_in_q_ready),
    .m_axis_tdata  (fir_out_q),
    .m_axis_tlast  (fir_out_q_tlast),
    .m_axis_tvalid (fir_out_q_valid),
    .m_axis_tready (s_out_payload_tready),
    .tap_delays    (packed_delays),
    .tap_coeffs    (packed_coeffs)
  );

  // Input ready: both I and Q paths must be ready
  assign pipe_in_tready = fir_in_i_ready & fir_in_q_ready;

  // Recombine IQ output
  assign s_out_payload_tdata  = {fir_out_i, fir_out_q};
  assign s_out_payload_tlast  = fir_out_i_tlast;
  assign s_out_payload_tvalid = fir_out_i_valid;
  assign s_out_payload_tkeep  = 1'b1;

  // Context passthrough (no modification to CHDR headers)
  assign s_out_context_tdata  = m_in_context_tdata;
  assign s_out_context_tuser  = m_in_context_tuser;
  assign s_out_context_tlast  = m_in_context_tlast;
  assign s_out_context_tvalid = m_in_context_tvalid;
  assign m_in_context_tready  = s_out_context_tready;

endmodule // rfnoc_block_sparse_fir


`default_nettype wire
