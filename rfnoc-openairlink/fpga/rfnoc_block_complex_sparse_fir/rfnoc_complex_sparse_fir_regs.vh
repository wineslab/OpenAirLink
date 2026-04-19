//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module:  rfnoc_complex_sparse_fir_regs (Header)
//
// Description:  Register definitions for rfnoc_block_complex_sparse_fir.
//               All registers are 32-bit words from software's perspective.
//
// Register Map (per block instance):
//
//   0x00  REG_COMPAT_NUM      (R)   - {COMPAT_MAJOR[31:16], COMPAT_MINOR[15:0]}
//   0x04  REG_NUM_TAPS        (R)   - Number of active taps (compile-time)
//   0x08  REG_MAX_DELAY       (R)   - Maximum delay depth (compile-time)
//   0x0C  (reserved)
//
//   Per-tap registers (i = 0 .. NUM_TAPS-1):
//     0x10 + i*0x08 + 0x00  REG_TAP[i]_DELAY  (R/W) - Delay in samples
//     0x10 + i*0x08 + 0x04  REG_TAP[i]_COEFF  (R/W) - Complex coefficient (packed)
//                            Bits [15:0]  = coeff_re (signed, Q1.15)
//                            Bits [31:16] = coeff_im (signed, Q1.15)
//                            When coeff_im=0, behavior is real-only (backward compatible).
//                            COMPAT_MAJOR >= 2 indicates complex coefficient support.
//
//   For NUM_TAPS=32, last tap register is at 0x10 + 31*0x08 + 0x04 = 0x10C.
//

// Address space per complex_sparse_fir block. Each block occupies 2^COMPLEX_SPARSE_FIR_ADDR_W bytes.
// Must be wide enough for: REG_TAP_BASE + NUM_TAPS * REG_TAP_STRIDE
// For NUM_TAPS=32: 0x10 + 32*0x08 = 0x110 -> need 9 bits (512 bytes).
localparam COMPLEX_SPARSE_FIR_ADDR_W = 9; // 512 bytes

// Read-only info registers
localparam REG_COMPAT_NUM    = 'h00;
localparam REG_NUM_TAPS      = 'h04;
localparam REG_MAX_DELAY     = 'h08;

// Per-tap registers: base = 0x10, stride = 0x08 per tap
// TAP[i]_DELAY = 0x10 + i*0x08
// TAP[i]_COEFF = 0x14 + i*0x08
localparam REG_TAP_BASE      = 'h10;
localparam REG_TAP_STRIDE    = 'h08;
// Offsets within each tap's register pair
localparam REG_TAP_DELAY_OFF = 'h00;
localparam REG_TAP_COEFF_OFF = 'h04;
