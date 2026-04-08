---
name: synth
description: Build/synthesize an FPGA image for the X410 using Vivado. Use when the user wants to build, synthesize, or compile a bitstream.
user-invocable: true
allowed-tools: Bash Read Grep Glob
---

# Synthesize FPGA Image

Build an FPGA bitstream for the X410 channel emulator.

## Arguments

- `$ARGUMENTS` — optional image target name. Defaults to `x410_rfnoc_image_core_4chan_sparse`.
  - Valid targets: `x410_rfnoc_image_core`, `x410_rfnoc_image_core_4chan`, `x410_rfnoc_image_core_4chan_sparse`

## Steps

1. **Pre-flight check**: Verify the build directory exists and cmake has been configured:
   ```bash
   ls /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/Makefile
   ```
   If not found, run:
   ```bash
   cd /home/wines/Desktop/OpenAirLink/rfnoc-openairlink && mkdir -p build && cd build && cmake -DUHD_FPGA_DIR=/home/wines/Desktop/uhd/fpga/ ../
   ```

2. **Show current image core config**: Read the YAML to confirm NUM_TAPS, MAX_DELAY, and block topology:
   ```bash
   grep -E "NUM_TAPS|MAX_DELAY|COEFF_WIDTH" /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/icores/x410_rfnoc_image_core_4chan_sparse.yml
   ```

3. **Launch synthesis** (this takes ~2-3 hours):
   ```bash
   cd /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build && make x410_rfnoc_image_core_4chan_sparse
   ```

4. **Check results**: After completion, check timing:
   ```bash
   grep -A3 "WNS(ns)" /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE/post_route_timing_summary.rpt | head -5
   ```
   - If WNS is negative, read the failing paths from `post_route_timing.rpt` to determine if the failure is in our code or BSP IP.
   - Known: BRAM NO_CHANGE warnings (REQP-1934/1935) are pre-existing in Xilinx AXI DMA/DDR4 IP — safe to ignore.
   - Known: `viv_utils.tcl` does strict text match for "timing constraints are met" — any negative WNS causes a hard error even if it's 6 ps.

5. **Locate bitstream**:
   ```bash
   ls -la /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE/x4xx.bit
   ```

Report the WNS, resource utilization summary, and bitstream path to the user.
