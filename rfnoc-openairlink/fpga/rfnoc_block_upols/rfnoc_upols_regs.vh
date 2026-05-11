//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module:  rfnoc_upols_regs (Header)
//
// Description:  Register definitions for rfnoc_block_upols.
//               All registers are 32-bit words from software's perspective.
//
// Register Map (per block instance):
//
//   0x00  REG_COMPAT_NUM   (R)   - {COMPAT_MAJOR[31:16], COMPAT_MINOR[15:0]}
//   0x04  REG_N            (R)   - Filter length in taps  (compile-time)
//   0x08  REG_B            (R)   - Block length in samples (compile-time)
//   0x0C  REG_K            (R)   - FFT size   (compile-time, K = 2*B)
//   0x10  REG_P            (R)   - Number of partitions (compile-time, P = ceil(N/B))
//   0x14  REG_CTRL         (R/W) - Control bits:
//                                    [0] soft_reset — write 1 to clear FDL,
//                                        input_buf, write_ptr, output pipeline.
//                                        Self-clearing (always reads 0).
//   0x18  REG_STATUS       (R)   - Status bits:
//                                    [0] h_loaded          — set once P*K words
//                                        have been written into H_parts.
//                                        Cleared by soft_reset.
//                                    [1] overflow_sticky   — input ran out of
//                                        samples mid-block (frame miss).
//                                        Cleared by soft_reset.
//                                    [2] underflow_sticky  — output backpressure
//                                        exceeded internal buffering.
//                                        Cleared by soft_reset.
//   0x1C  (reserved)
//
//   0x20  REG_H_ADDR       (R/W) - Current index into the H_parts BRAM window.
//                                    Range [0, P*K).  Auto-increments on every
//                                    read or write of REG_H_DATA, wrapping
//                                    modulo P*K.  Upper bits are don't-care.
//                                    Index layout: addr = p*K + k
//                                    (partition-major, frequency-bin-minor).
//   0x24  REG_H_DATA       (R/W) - One complex coefficient in Q1.15 format,
//                                    packed as { H_parts[addr].im[15:0],
//                                                H_parts[addr].re[15:0] }.
//                                    Read returns the stored value at H_ADDR,
//                                    then auto-increments H_ADDR.
//                                    Write stores to H_parts[H_ADDR], then
//                                    auto-increments H_ADDR.
//
// Upload protocol (host):
//   1. poke32(REG_CTRL, 1)                            -> stop streaming, clear state
//   2. poke32(REG_H_ADDR, 0)                          -> rewind upload window
//   3. for i in 0 .. P*K-1:
//          poke32(REG_H_DATA, pack(H_parts_flat[i]))  -> addr auto-increments
//   4. Streaming resumes automatically once soft_reset is self-cleared.
//
// Compat major bump rules:
//   - 1.0  first release, N/B/K/P compile-time, no crossfade.
//   - Any breaking change to the H_parts memory layout, data format, or
//     register addresses MUST increment COMPAT_MAJOR.

// Address space per UPOLS block.  Must be wide enough for REG_H_DATA = 0x24.
// 7 bits -> 128 bytes, leaves headroom for v2 additions.
localparam UPOLS_ADDR_W = 7;

// Read-only info registers
localparam REG_COMPAT_NUM  = 'h00;
localparam REG_N           = 'h04;
localparam REG_B           = 'h08;
localparam REG_K           = 'h0C;
localparam REG_P           = 'h10;

// Control / status
localparam REG_CTRL        = 'h14;
localparam REG_STATUS      = 'h18;

// H_parts upload window
localparam REG_H_ADDR      = 'h20;
localparam REG_H_DATA      = 'h24;

// Bit positions within REG_CTRL
localparam CTRL_SOFT_RESET_BIT = 0;

// Bit positions within REG_STATUS
localparam STATUS_H_LOADED_BIT = 0;
localparam STATUS_OVERFLOW_BIT = 1;
localparam STATUS_UNDERFLOW_BIT = 2;
