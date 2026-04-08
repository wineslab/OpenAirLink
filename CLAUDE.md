# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### Host library and applications
```bash
cd rfnoc-openairlink/build
cmake -DUHD_FPGA_DIR=<path-to-uhd>/uhd/fpga/ ../
make                    # builds librfnoc-openairlink.so + all apps
sudo make install       # installs to /usr/local/lib/
sudo ldconfig
```

### FPGA images (requires Vivado 2021.1 + AR76780 patch)
```bash
cd rfnoc-openairlink/build
make x410_rfnoc_image_core               # 2-channel (dense FIR)
make x410_rfnoc_image_core_4chan          # 4-channel (dense FIR)
make x410_rfnoc_image_core_4chan_sparse   # 4-channel (sparse FIR)
```
Bitstreams land at `icores/build-<image_name>/x4xx.bit`.

### FPGA deployment to X410
DTS preprocessing is **required** or you get "no component version information" errors:
```bash
cd <uhd>/fpga/usrp3/top/x400
gcc -o <build_dir>/<IMAGE>.dts -C -E -I dts -nostdinc -undef \
  -x assembler-with-cpp -D__DTS__ <build_dir>/device_tree.dts
```
Then copy both `.bit` and `.dts` (same base name) to the X410 and load:
```bash
uhd_image_loader --args "type=x4xx,addr=<IP>" --fpga-path <IMAGE>.bit
reboot
```

### Running applications
Block controllers must be preloaded unless installed to /usr/local/lib/:
```bash
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so ./apps/oal_4chan_sparse \
  --args "addr=<X410_IP>,clock_source=external,time_source=external"
```

## Architecture

### RFNoC blocks (rfnoc-openairlink/fpga/)
Two custom blocks, each following the standard RFNoC pattern:
- **sparse_fir** (NOC ID `0x5F1A0004`): BRAM-based sparse FIR filter. NUM_TAPS replicated BRAM delay lines with independent programmable delays and coefficients. Processes sc16 with separate I/Q FIR cores.
- **shiftright** (NOC ID `0x02D024`): Arithmetic right-shift for attenuation. Single register controls shift amount.

Standard Ettus blocks used in image cores: `split_stream`, `addsub_patched`, `radio`.

### Per-block file structure
```
rfnoc_block_<name>/
  noc_shell_<name>.v      # RFNoC shell (framework adapter, auto-generated pattern)
  rfnoc_block_<name>.v    # Block wrapper (ctrlport registers, IQ split, core instantiation)
  axi_<name>.v            # Core DSP engine (if applicable)
  rfnoc_<name>_regs.vh    # Register address map (included via `include)
```
HDL file lists for synthesis are in `Makefile.srcs` (appended to `RFNOC_OOT_SRCS`).

### 4-channel signal flow (1 gNB + 3 UEs)
```
DOWNLINK: radio0:RX0 -> split -> sparse_fir x3 -> shiftright x3 -> radio0:TX1, radio1:TX0, radio1:TX1
UPLINK:   radio0:RX1, radio1:RX0, radio1:RX1 -> sparse_fir x3 -> shiftright x3 -> addsub cascade -> radio0:TX0
```
Topology defined in `icores/x410_rfnoc_image_core_4chan_sparse.yml`.

### Host-side block controllers (lib/, include/)
- `sparse_fir_block_control`: Reads NUM_TAPS and MAX_DELAY from FPGA at init. All register access uses computed addresses (`REG_TAP_BASE + idx * REG_TAP_STRIDE`). Fully parameterized — works with any NUM_TAPS.
- `shiftright_block_control`: Single register read/write.

### Host applications (apps/)
- `oal_single` / `oal_dual` / `oal_4chan`: Dense FIR variants (41 taps)
- `oal_4chan_sparse`: Sparse FIR variant (dynamic NUM_TAPS from FPGA)

All apps use Boost.program_options. CSV config parsed at runtime; sparse format is `delay:coeff` pairs, dense format is space-separated coefficients.

## Key Conventions

- **Data format**: sc16 everywhere — 32-bit items with I[31:16], Q[15:0]
- **Register pattern**: ctrlport with 20-bit address, 32-bit data. Per-block address decoded via `local_addr = req_addr[ADDR_W-1:0]`. Fixed registers at low addresses, per-tap/per-element registers at stride-based offsets.
- **Sparse FIR register map**: `0x00` compat, `0x04` NUM_TAPS (R), `0x08` MAX_DELAY (R), `0x10 + i*0x08` delay/coeff pairs. Bit[2] of intra-tap offset selects delay(0) vs coeff(1).
- **Block descriptors**: `blocks/*.yml` define NOC ID, parameters, clocks, and data interfaces for `rfnoc_image_builder`.
- **Image cores**: `icores/*.yml` define the full FPGA topology (blocks, connections, clock domains, transport adapters).

## FPGA Timing Notes

- ce_clk runs at 266 MHz (3.75 ns period) on X410. This is tight for BRAM-to-DSP paths.
- The sparse FIR's critical path is BRAM read output -> bypass MUX -> DSP multiply. An explicit `tap_data` pipeline register was added to break this path.
- `viv_utils.tcl` (in UHD source) does a strict text match for "timing constraints are met" — any negative WNS, even 6 ps, causes a hard build error. No threshold tolerance.
- BRAM NO_CHANGE warnings (REQP-1934/1935) in build logs are from Xilinx/Ettus IP (AXI DMA, DDR4), not from this project's code.
- NUM_TAPS=32 caused timing failure (WNS -0.129 ns from routing congestion); NUM_TAPS=16 fits after the pipeline register fix.

## Channel Configuration (CSV)

Files in `channel_control/`:
- **Dense FIR**: `coeff0 coeff1 ... coeff40, shift_value` (41 int16 taps)
- **Sparse FIR**: `delay0:coeff0 delay1:coeff1 ..., shift_value` (any number of taps, zero-padded to NUM_TAPS)
- **Modes**: `manually` files are polled periodically (--udt interval); `script` files are time-indexed sequences (--script flag)
- 4-channel CSV has 6 channel columns per row (3 DL + 3 UL), comma-separated.
