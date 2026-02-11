//
// Copyright 2025 OpenAirLink Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module:  rfnoc_sparse_fir_regs (Header)
//
// Description:  Register definitions for rfnoc_block_sparse_fir.
//               All registers are 32-bit words from software's perspective.
//
// Register Map (per block instance):
//
//   0x00  REG_COMPAT_NUM      (R)   - {COMPAT_MAJOR[31:16], COMPAT_MINOR[15:0]}
//   0x04  REG_NUM_TAPS        (R)   - Number of active taps (compile-time)
//   0x08  REG_MAX_DELAY       (R)   - Maximum delay depth (compile-time)
//   0x0C  (reserved)
//   0x10  REG_TAP0_DELAY      (R/W) - Delay in samples for tap 0
//   0x14  REG_TAP0_COEFF      (R/W) - Coefficient for tap 0 (signed 16-bit)
//   0x18  REG_TAP1_DELAY      (R/W)
//   0x1C  REG_TAP1_COEFF      (R/W)
//   0x20  REG_TAP2_DELAY      (R/W)
//   0x24  REG_TAP2_COEFF      (R/W)
//   0x28  REG_TAP3_DELAY      (R/W)
//   0x2C  REG_TAP3_COEFF      (R/W)
//

// Address space per sparse_fir block. Each block occupies 2^SPARSE_FIR_ADDR_W bytes.
localparam SPARSE_FIR_ADDR_W = 6; // 64 bytes

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
