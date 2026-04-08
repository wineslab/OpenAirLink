---
name: status
description: Show the full state of the OpenAirLink project — FPGA config, Vivado build verdict (timing/utilization/power), git status, and optionally X410 device state. Use when the user asks about status, build results, timing, utilization, power, or what's deployed.
user-invocable: true
allowed-tools: Bash Read Grep Glob
model: sonnet
---

# OpenAirLink Status & Build Report

Show current project state and, if a build exists, parse Vivado reports into a pass/fail verdict.

## Arguments

- `$ARGUMENTS` — optional X410 SSH hostname (e.g., `x410_0`). If provided, also checks the remote device.

## Part 1: Design Config

1. **Current FPGA config** — show NUM_TAPS and key parameters:
   ```bash
   grep -E "NUM_TAPS|MAX_DELAY|COEFF_WIDTH" /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/icores/x410_rfnoc_image_core_4chan_sparse.yml | head -10
   ```

2. **Sparse FIR pipeline config**:
   ```bash
   grep "PIPELINE_DELAY" /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/fpga/rfnoc_block_sparse_fir/axi_sparse_fir.v | head -3
   ```

3. **Git status**:
   ```bash
   cd /home/wines/Desktop/OpenAirLink && git status --short
   ```

## Part 2: Build Verdict

Check if a build exists, then parse Vivado reports. Build directory:
`/home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE`

If `x4xx.bit` does not exist, report "No build found" and skip to Part 3.

4. **Bitstream**:
   ```bash
   ls -lh $BUILD_DIR/x4xx.bit 2>/dev/null
   ```

5. **Timing verdict** (determines pass/fail):

   Check the pass/fail string:
   ```bash
   grep -i "timing constraints" $BUILD_DIR/build.rpt
   ```
   - "All user specified timing constraints are met" = **PASS**
   - Absent = **FAIL** (viv_utils.tcl triggers error)

   Extract WNS/WHS/failing endpoints:
   ```bash
   grep -A3 "WNS(ns).*TNS" $BUILD_DIR/post_route_timing_summary.rpt | tail -1
   ```

   If timing **FAILED**, find ce_clk-specific line and worst paths:
   ```bash
   grep "ce_clk" $BUILD_DIR/post_route_timing_summary.rpt | head -2
   head -40 $BUILD_DIR/post_route_timing.rpt
   ```
   Report which module the worst path is in (sparse FIR vs BSP IP).

6. **Resource utilization**:
   ```bash
   grep -A2 "CLB LUTs" $BUILD_DIR/post_route_util.rpt | head -3
   grep -A2 "CLB Registers" $BUILD_DIR/post_route_util.rpt | head -3
   grep -A2 "Block RAM Tile" $BUILD_DIR/post_route_util.rpt | head -3
   grep -A2 "DSPs" $BUILD_DIR/post_route_util.rpt | head -3
   ```

7. **Power**:
   ```bash
   grep -E "Total On-Chip Power|Dynamic|Device Static" $BUILD_DIR/post_route_power.rpt | head -5
   grep "Junction Temperature" $BUILD_DIR/post_route_power.rpt
   ```

8. **DRC violations**:
   ```bash
   grep "Total Violations Found" $BUILD_DIR/post_imp_drc.rpt
   ```

## Part 3: Remote X410 (when hostname provided)

9. **X410 reachable**:
   ```bash
   ssh -o ConnectTimeout=5 <X410_HOST> "echo OK" 2>/dev/null
   ```

10. **Loaded FPGA image** — check via dmesg:
    ```bash
    ssh <X410_HOST> "dmesg | grep 'writing.*to.*FPGA' | tail -1"
    ```
    Note: `uhd_find_devices` always reports `fpga=X4_200` — that is the FPGA type, NOT the image name.

11. **Device claimed / block discovery**:
    ```bash
    ssh <X410_HOST> "uhd_find_devices --args addr=127.0.0.1 2>/dev/null | grep claimed"
    ```
    If `claimed: False`, also probe:
    ```bash
    ssh <X410_HOST> "LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so uhd_usrp_probe --args addr=127.0.0.1 2>&1 | grep -iE 'sparse_fir|shiftright|noc_id|0x5F1A|0x02D024'"
    ```
    If `claimed: True`, report device is in use (skip probe).

12. **OAL library installed**:
    ```bash
    ssh <X410_HOST> "ldconfig -p | grep rfnoc-openairlink"
    ```

13. **OAL binary available**:
    ```bash
    ssh <X410_HOST> "ls ~/OpenAirLink/rfnoc-openairlink/build/apps/oal_4chan_sparse 2>/dev/null || ls ~/emulator_*/OpenAirLink/rfnoc-openairlink/build/apps/oal_4chan_sparse 2>/dev/null"
    ```

## Output Format

Present as a concise verdict card:

```
## Build Verdict: PASS / FAIL / NO BUILD

| Metric            | Value        | Status |
|-------------------|--------------|--------|
| NUM_TAPS          | 16           | —      |
| PIPELINE_DELAY    | 4+TREE       | —      |
| Timing WNS        | +0.001 ns    | OK     |
| Timing WHS        | +0.007 ns    | OK     |
| Failing endpoints | 0            | OK     |
| LUT utilization   | 51.6%        | OK     |
| BRAM util.        | XX%          | OK     |
| DSP util.         | XX%          | OK     |
| Total power       | 18.1 W       | OK     |
| Junction temp     | 40.2 C       | OK     |
| DRC violations    | 477 (0 err)  | OK     |
| Bitstream         | x4xx.bit     | EXISTS |
| X410 blocks       | SparseFIR OK | OK     |
| OAL installed     | yes          | OK     |
```

Status rules:
- Timing WNS >= 0: **OK**. WNS < 0: **FAIL** (report slack and worst path module)
- LUT > 85%: **WARN**. LUT > 95%: **CRITICAL**
- BRAM/DSP > 90%: **WARN**
- Power > 25 W: **WARN** (X410 thermal limit)
- Junction temp > 85 C: **WARN**
- Any DRC ERROR: **FAIL**

If timing FAILED, add a **Root Cause** section: which clock domain, which module, suggested fix.
