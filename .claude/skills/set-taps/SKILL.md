---
name: set-taps
description: Change the number of sparse FIR taps (NUM_TAPS) in the image core YAML. Use when the user wants to change tap count, filter size, or sparse FIR parameters.
user-invocable: true
allowed-tools: Read Edit Grep Glob
model: sonnet
---

# Set Sparse FIR NUM_TAPS

Change the NUM_TAPS parameter for all sparse FIR blocks in the image core.

## Arguments

- `$ARGUMENTS` — the desired NUM_TAPS value (must be a power of 2: 1, 2, 4, 8, 16, 32)

## Steps

1. **Validate** the argument is a power of 2 in range [1, 32].

2. **Read current config**:
   ```
   rfnoc-openairlink/icores/x410_rfnoc_image_core_4chan_sparse.yml
   ```
   Show the user the current NUM_TAPS value.

3. **Update all 6 sfir blocks** (sfir_dl0, sfir_dl1, sfir_dl2, sfir_ul0, sfir_ul1, sfir_ul2) to the new NUM_TAPS value using Edit with `replace_all`.

4. **Update the header comment** with corrected resource estimates:
   - Per block (complex coefficients): `4*NUM_TAPS` DSP48, `2*NUM_TAPS` BRAM18
   - Total (6 blocks): `24*NUM_TAPS` DSP48, `12*NUM_TAPS` BRAM18

5. **Warn about timing and DSP budget** (X410 has 4272 DSP48s):
   - NUM_TAPS=32: 768 DSP48 for sparse FIR, ~3847 total (90%) — passed timing with WNS +0.043 ns
   - NUM_TAPS=16: 384 DSP48 for sparse FIR — comfortable fit
   - NUM_TAPS <= 8: easy timing closure
   - DSP budget above 90% is a warning zone

6. Remind the user to run `/synth` to rebuild the FPGA image after changing taps.
