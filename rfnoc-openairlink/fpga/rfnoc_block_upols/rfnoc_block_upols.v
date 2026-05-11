//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module: rfnoc_block_upols
//
// Description:
//
//   Top wrapper for the UPOLS (Uniformly-Partitioned Overlap-Save) FIR block.
//   Handles the RFNoC framework integration (noc_shell, ctrlport register
//   decode, AXI-Stream to the DSP engine) and owns the H_parts BRAM that
//   stores the pre-transformed sub-filter spectra.
//
//   The heavy DSP (input slide, forward FFT, frequency-domain MAC + FDL,
//   inverse FFT, overlap-save extraction) lives in axi_upols_engine.
//
//   See streaming_upols_spec_v2.md for the algorithmic contract and
//   rfnoc_upols_regs.vh for the register layout this module decodes.
//
// Parameters:
//
//   THIS_PORTID : Control crossbar port to which this block is connected
//   CHDR_W      : AXIS-CHDR data bus width (64 on X410)
//   MTU         : Maximum transmission unit
//   N           : Filter length in taps
//   B           : Block length in samples (also UHD recv() size)
//
//   K and P are derived:
//     K = 2 * B
//     P = (N + B - 1) / B   // ceil(N/B)
//

`default_nettype none


module rfnoc_block_upols #(
  parameter [9:0] THIS_PORTID = 10'd0,
  parameter       CHDR_W      = 64,
  parameter [5:0] MTU         = 10,
  parameter       N           = 1024,
  parameter       B           = 512
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

  `include "rfnoc_upols_regs.vh"

  // ------------------------------------------------------------------
  // Derived parameters.  Elaboration-time checks on the user-supplied
  // N and B are done in axi_upols_engine (where the math lives).
  // ------------------------------------------------------------------
  localparam integer K           = 2 * B;
  localparam integer P           = (N + B - 1) / B;
  localparam integer PK          = P * K;
  localparam integer H_ADDR_W    = $clog2(PK);
  localparam integer LOAD_CNT_W  = $clog2(PK + 1);

  localparam [15:0] COMPAT_MAJOR = 16'h0001;
  localparam [15:0] COMPAT_MINOR = 16'h0000;

  localparam ITEM_W = 32;  // sc16 = {I[31:16], Q[15:0]}

  //---------------------------------------------------------------------------
  // Signal Declarations
  //---------------------------------------------------------------------------

  // Clocks and Resets from NoC shell
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

  noc_shell_upols #(
    .CHDR_W      (CHDR_W),
    .THIS_PORTID (THIS_PORTID),
    .MTU         (MTU)
  ) noc_shell_upols_i (
    .rfnoc_chdr_clk      (rfnoc_chdr_clk),
    .rfnoc_ctrl_clk      (rfnoc_ctrl_clk),
    .ce_clk              (ce_clk),
    .rfnoc_chdr_rst      (),
    .rfnoc_ctrl_rst      (),
    .ce_rst              (),
    .rfnoc_core_config   (rfnoc_core_config),
    .rfnoc_core_status   (rfnoc_core_status),
    .s_rfnoc_chdr_tdata  (s_rfnoc_chdr_tdata),
    .s_rfnoc_chdr_tlast  (s_rfnoc_chdr_tlast),
    .s_rfnoc_chdr_tvalid (s_rfnoc_chdr_tvalid),
    .s_rfnoc_chdr_tready (s_rfnoc_chdr_tready),
    .m_rfnoc_chdr_tdata  (m_rfnoc_chdr_tdata),
    .m_rfnoc_chdr_tlast  (m_rfnoc_chdr_tlast),
    .m_rfnoc_chdr_tvalid (m_rfnoc_chdr_tvalid),
    .m_rfnoc_chdr_tready (m_rfnoc_chdr_tready),
    .s_rfnoc_ctrl_tdata  (s_rfnoc_ctrl_tdata),
    .s_rfnoc_ctrl_tlast  (s_rfnoc_ctrl_tlast),
    .s_rfnoc_ctrl_tvalid (s_rfnoc_ctrl_tvalid),
    .s_rfnoc_ctrl_tready (s_rfnoc_ctrl_tready),
    .m_rfnoc_ctrl_tdata  (m_rfnoc_ctrl_tdata),
    .m_rfnoc_ctrl_tlast  (m_rfnoc_ctrl_tlast),
    .m_rfnoc_ctrl_tvalid (m_rfnoc_ctrl_tvalid),
    .m_rfnoc_ctrl_tready (m_rfnoc_ctrl_tready),
    .ctrlport_clk              (ctrlport_clk),
    .ctrlport_rst              (ctrlport_rst),
    .m_ctrlport_req_wr         (m_ctrlport_req_wr),
    .m_ctrlport_req_rd         (m_ctrlport_req_rd),
    .m_ctrlport_req_addr       (m_ctrlport_req_addr),
    .m_ctrlport_req_data       (m_ctrlport_req_data),
    .m_ctrlport_resp_ack       (m_ctrlport_resp_ack),
    .m_ctrlport_resp_data      (m_ctrlport_resp_data),
    .axis_data_clk       (axis_data_clk),
    .axis_data_rst       (axis_data_rst),
    .m_in_payload_tdata  (m_in_payload_tdata),
    .m_in_payload_tkeep  (m_in_payload_tkeep),
    .m_in_payload_tlast  (m_in_payload_tlast),
    .m_in_payload_tvalid (m_in_payload_tvalid),
    .m_in_payload_tready (m_in_payload_tready),
    .m_in_context_tdata  (m_in_context_tdata),
    .m_in_context_tuser  (m_in_context_tuser),
    .m_in_context_tlast  (m_in_context_tlast),
    .m_in_context_tvalid (m_in_context_tvalid),
    .m_in_context_tready (m_in_context_tready),
    .s_out_payload_tdata  (s_out_payload_tdata),
    .s_out_payload_tkeep  (s_out_payload_tkeep),
    .s_out_payload_tlast  (s_out_payload_tlast),
    .s_out_payload_tvalid (s_out_payload_tvalid),
    .s_out_payload_tready (s_out_payload_tready),
    .s_out_context_tdata  (s_out_context_tdata),
    .s_out_context_tuser  (s_out_context_tuser),
    .s_out_context_tlast  (s_out_context_tlast),
    .s_out_context_tvalid (s_out_context_tvalid),
    .s_out_context_tready (s_out_context_tready)
  );


  //---------------------------------------------------------------------------
  // Register bank: decode ctrlport requests into soft-reset pulse, H_parts
  // write commands, and readback of N/B/K/P/CTRL/STATUS/H_ADDR.
  //
  // H_DATA reads return 0 (the BRAM behind H_parts is write-mostly from the
  // host's perspective — readback is not required by the software contract).
  // H_ADDR auto-increments modulo PK on every H_DATA access.
  //---------------------------------------------------------------------------

  wire [UPOLS_ADDR_W-1:0] local_addr = m_ctrlport_req_addr[UPOLS_ADDR_W-1:0];

  reg [H_ADDR_W-1:0]   h_addr_r;
  reg                  h_data_wr_stb;     // 1-cycle strobe when H_DATA is written
  reg [31:0]           h_data_wr_val;     // the 32-bit packed value to store
  reg                  soft_reset_pulse;  // 1-cycle pulse for engine reset
  reg [LOAD_CNT_W-1:0] load_cnt;          // counts valid H_parts writes since reset
  reg                  h_loaded;
  reg                  overflow_sticky;
  reg                  underflow_sticky;

  // Signals from the engine — status bits get OR'd into the sticky regs
  wire engine_overflow;
  wire engine_underflow;

  // Next-state helpers so the same always_ff can update multiple regs cleanly
  wire [H_ADDR_W-1:0] h_addr_next =
      (h_addr_r == (PK-1)) ? {H_ADDR_W{1'b0}} : (h_addr_r + 1'b1);

  always @(posedge ctrlport_clk) begin
    if (ctrlport_rst) begin
      m_ctrlport_resp_ack  <= 1'b0;
      m_ctrlport_resp_data <= 32'b0;
      h_addr_r             <= {H_ADDR_W{1'b0}};
      h_data_wr_stb        <= 1'b0;
      h_data_wr_val        <= 32'b0;
      soft_reset_pulse     <= 1'b0;
      load_cnt             <= {LOAD_CNT_W{1'b0}};
      h_loaded             <= 1'b0;
      overflow_sticky      <= 1'b0;
      underflow_sticky     <= 1'b0;
    end else begin
      // Defaults — one-cycle pulses clear themselves
      m_ctrlport_resp_ack <= 1'b0;
      m_ctrlport_resp_data <= 32'b0;
      h_data_wr_stb       <= 1'b0;
      soft_reset_pulse    <= 1'b0;

      // Latch sticky engine status
      if (engine_overflow)  overflow_sticky  <= 1'b1;
      if (engine_underflow) underflow_sticky <= 1'b1;

      // ---- Reads ----
      if (m_ctrlport_req_rd) begin
        m_ctrlport_resp_ack <= 1'b1;
        case (local_addr)
          REG_COMPAT_NUM[UPOLS_ADDR_W-1:0]: m_ctrlport_resp_data <= {COMPAT_MAJOR, COMPAT_MINOR};
          REG_N[UPOLS_ADDR_W-1:0]:          m_ctrlport_resp_data <= N;
          REG_B[UPOLS_ADDR_W-1:0]:          m_ctrlport_resp_data <= B;
          REG_K[UPOLS_ADDR_W-1:0]:          m_ctrlport_resp_data <= K;
          REG_P[UPOLS_ADDR_W-1:0]:          m_ctrlport_resp_data <= P;
          REG_CTRL[UPOLS_ADDR_W-1:0]:       m_ctrlport_resp_data <= 32'b0;  // CTRL bits are self-clearing
          REG_STATUS[UPOLS_ADDR_W-1:0]: begin
            m_ctrlport_resp_data <= {29'b0,
                                     underflow_sticky,
                                     overflow_sticky,
                                     h_loaded};
          end
          REG_H_ADDR[UPOLS_ADDR_W-1:0]: begin
            m_ctrlport_resp_data <= {{(32-H_ADDR_W){1'b0}}, h_addr_r};
          end
          REG_H_DATA[UPOLS_ADDR_W-1:0]: begin
            // Readback not implemented — returns zero.  Also auto-increments
            // h_addr_r to keep write/read symmetry.
            m_ctrlport_resp_data <= 32'b0;
            h_addr_r             <= h_addr_next;
          end
          default: m_ctrlport_resp_data <= 32'b0;
        endcase
      end

      // ---- Writes ----
      if (m_ctrlport_req_wr) begin
        m_ctrlport_resp_ack <= 1'b1;
        case (local_addr)
          REG_CTRL[UPOLS_ADDR_W-1:0]: begin
            // soft_reset is a self-clearing pulse: fires for 1 cycle on
            // the next ce_clk edge, then latches back to 0.
            if (m_ctrlport_req_data[CTRL_SOFT_RESET_BIT]) begin
              soft_reset_pulse <= 1'b1;
              load_cnt         <= {LOAD_CNT_W{1'b0}};
              h_loaded         <= 1'b0;
              overflow_sticky  <= 1'b0;
              underflow_sticky <= 1'b0;
              h_addr_r         <= {H_ADDR_W{1'b0}};
            end
          end
          REG_H_ADDR[UPOLS_ADDR_W-1:0]: begin
            // Truncating upper bits is intentional — spec says upper bits
            // are don't-care and the address wraps modulo PK.
            if (m_ctrlport_req_data[H_ADDR_W-1:0] < PK[H_ADDR_W-1:0])
              h_addr_r <= m_ctrlport_req_data[H_ADDR_W-1:0];
            else
              h_addr_r <= {H_ADDR_W{1'b0}};
          end
          REG_H_DATA[UPOLS_ADDR_W-1:0]: begin
            // One-cycle BRAM write strobe; actual BRAM update happens in a
            // separate always block below (cleaner synth — BRAM write is
            // not inside the register-decode case statement).
            h_data_wr_stb <= 1'b1;
            h_data_wr_val <= m_ctrlport_req_data;
            h_addr_r      <= h_addr_next;
            // Count H_parts writes toward the "h_loaded" flag.  Only count
            // up to PK; the flag latches once all partitions are populated.
            if (!h_loaded) begin
              if (load_cnt == (PK[LOAD_CNT_W-1:0] - 1'b1)) begin
                h_loaded <= 1'b1;
                load_cnt <= {LOAD_CNT_W{1'b0}};
              end else begin
                load_cnt <= load_cnt + 1'b1;
              end
            end
          end
          default: ; // writes to read-only regs silently ack
        endcase
      end
    end
  end


  //---------------------------------------------------------------------------
  // H_parts BRAM  (Port A: ctrlport write + read; Port B: DSP engine read)
  //
  //   Inferred true dual-port BRAM.  Xilinx synthesis maps this to either a
  //   BRAM36 (if PK*32 bits > 18Kb) or a pair of BRAM18s.  At the default
  //   PK = 2048 × 32b = 64 Kbits, this is 2 BRAM18 or 1 BRAM36 per block.
  //---------------------------------------------------------------------------

  (* ram_style = "block" *)
  reg [31:0] h_parts_mem [0:PK-1];

  // Initialize to zero so an un-loaded filter behaves as "no output" rather
  // than outputting uninitialized garbage during bring-up simulation.
  integer init_i;
  initial begin
    for (init_i = 0; init_i < PK; init_i = init_i + 1)
      h_parts_mem[init_i] = 32'b0;
  end

  // Port A (ctrlport write) — h_addr_r already holds the slot to write
  // because we updated it on the previous cycle.  Use a small delay-by-1
  // pipeline so the address matches the strobed data.
  reg [H_ADDR_W-1:0] h_wr_addr_d;
  always @(posedge ctrlport_clk) begin
    // h_addr_r was post-incremented in the ctrlport block above.  The slot
    // that was written-to is (h_addr_r - 1) mod PK; cache it here.
    if (h_data_wr_stb) begin
      h_wr_addr_d <= (h_addr_r == {H_ADDR_W{1'b0}}) ? (PK-1) : (h_addr_r - 1'b1);
    end
  end

  // Actual BRAM write — happens the cycle after h_data_wr_stb fires.
  // Using h_wr_addr_d / a 1-cycle delayed path keeps the write port
  // outside the ctrlport case statement, helping synthesis infer a BRAM.
  reg               h_data_wr_stb_d;
  reg [31:0]        h_data_wr_val_d;
  always @(posedge ctrlport_clk) begin
    h_data_wr_stb_d <= h_data_wr_stb;
    h_data_wr_val_d <= h_data_wr_val;
    if (h_data_wr_stb_d) begin
      h_parts_mem[h_wr_addr_d] <= h_data_wr_val_d;
    end
  end

  // Port B (engine read)
  wire [H_ADDR_W-1:0] engine_h_rd_addr;
  reg  [31:0]         engine_h_rd_data;
  always @(posedge ce_clk) begin
    engine_h_rd_data <= h_parts_mem[engine_h_rd_addr];
  end


  //---------------------------------------------------------------------------
  // DSP Engine
  //---------------------------------------------------------------------------

  axi_upols_engine #(
    .N        (N),
    .B        (B),
    .ITEM_W   (ITEM_W)
  ) axi_upols_engine_i (
    .clk            (ce_clk),
    .rst            (ctrlport_rst | soft_reset_pulse),

    // AXI-Stream data in (from noc_shell)
    .s_axis_tdata   (m_in_payload_tdata),
    .s_axis_tlast   (m_in_payload_tlast),
    .s_axis_tvalid  (m_in_payload_tvalid),
    .s_axis_tready  (m_in_payload_tready),

    // AXI-Stream data out (to noc_shell)
    .m_axis_tdata   (s_out_payload_tdata),
    .m_axis_tlast   (s_out_payload_tlast),
    .m_axis_tvalid  (s_out_payload_tvalid),
    .m_axis_tready  (s_out_payload_tready),

    // H_parts BRAM read port
    .h_rd_addr      (engine_h_rd_addr),
    .h_rd_data      (engine_h_rd_data),

    // Status
    .overflow       (engine_overflow),
    .underflow      (engine_underflow)
  );

  assign s_out_payload_tkeep = 1'b1;

  // Context passthrough (same pattern as sparse FIR — UPOLS does not
  // modify CHDR headers; packet timing is preserved modulo the 2B-sample
  // algorithmic latency).
  assign s_out_context_tdata  = m_in_context_tdata;
  assign s_out_context_tuser  = m_in_context_tuser;
  assign s_out_context_tlast  = m_in_context_tlast;
  assign s_out_context_tvalid = m_in_context_tvalid;
  assign m_in_context_tready  = s_out_context_tready;

endmodule // rfnoc_block_upols


`default_nettype wire
