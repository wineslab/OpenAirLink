# OpenAirLink: Reproducible Wireless Channel Emulation using Software Defined Radios

An open-source channel emulator for reproducible testing of wireless mobility scenarios.
The emulator implements a FIR filter on Software Defined Radios, the NI USRP (formerly by Ettus). 
OpenAirLink was developed by the [Chair of Communication Networks](https://www.ce.cit.tum.de/lkn/startseite/) at the Technical University of Munich. 

## Requirements
- UHD 4.4

To rebuild FPGA image:
- [Vivado 2021.1](https://www.xilinx.com/support/download/index.html/content/xilinx/en/downloadNav/vivado-design-tools/archive.html)
- [AR76780 Patch](https://support.xilinx.com/s/article/76780?language=en_US)


## Synthesizing a New Image

### Prerequisites

- Vivado 2021.1 with the [AR76780 patch](https://support.xilinx.com/s/article/76780)
- CMake configured with `UHD_FPGA_DIR` pointing to the UHD FPGA source tree

### Available targets

| CMake target | Filter type | Description |
|---|---|---|
| `x410_rfnoc_image_core_4chan_sparse` | Complex sparse FIR | 4-ch (1 gNB + 3 UEs), programmable taps — **primary target** |
| `x410_rfnoc_image_core_4chan` | Dense FIR | 4-ch, fixed 41-tap FIR |

### Build

```bash
cd rfnoc-openairlink/build
cmake -DUHD_FPGA_DIR=/path/to/uhd/fpga/ ../
make x410_rfnoc_image_core_4chan_sparse
```

Vivado runs (~1 hour). On success the build automatically archives the outputs:

```
build/bitstreams/<YYYY-MM-DD_HHMMSS>_lchem_x410_4ch_csfir_16taps/
  lchem_x410_4ch_csfir_16taps.bit   ← flash with uhd_image_loader
  lchem_x410_4ch_csfir_16taps.dts   ← preprocessed DTS, required alongside .bit
```

Both files share the same base name so `uhd_image_loader` picks up the DTS automatically.

### DTS post-processing

The X410 firmware requires a Device Tree Source (`.dts`) file alongside every `.bit` bitstream. The DTS describes the FPGA component versions to the UHD driver; without it, `uhd_image_loader` prints "no component version information" and the loaded image may not enumerate correctly.

The raw `device_tree.dts` produced by `rfnoc_image_builder` uses C preprocessor `#include` directives and macros, so it must be run through the C preprocessor before it can be used. The build system does this automatically as a post-build step, but if you need to do it manually (e.g. after a bare Vivado synthesis run):

```bash
IMAGE_NAME=usrp_x410_lchem_4ch_csfir_16taps
BUILD_DIR=rfnoc-openairlink/build/icores/build-${IMAGE_NAME}
UHD_DTS_DIR=/path/to/uhd/fpga/usrp3/top/x400/dts

gcc -o ${BUILD_DIR}/${IMAGE_NAME}.dts \
    -C -E -I ${UHD_DTS_DIR} \
    -nostdinc -undef -x assembler-with-cpp -D__DTS__ \
    ${BUILD_DIR}/device_tree.dts
```

The flags do the following:
- `-C -E` — run only the C preprocessor and keep comments
- `-I ${UHD_DTS_DIR}` — find the Ettus-supplied DTS include files
- `-nostdinc -undef` — prevent system headers and predefined macros from leaking in
- `-x assembler-with-cpp` — treat the input as assembly with CPP (standard for DTS)
- `-D__DTS__` — activates the DTS-specific branches inside the include files

The output `.dts` must sit next to the `.bit` file with the same base name before calling `uhd_image_loader`.

### Flashing to the X410

```bash
uhd_image_loader --args "type=x4xx,addr=<X410_IP>" \
  --fpga-path build/bitstreams/<timestamp>_lchem_x410_4ch_csfir_16taps/lchem_x410_4ch_csfir_16taps.bit
# Reboot the X410 after loading
```

## Building the host library and applications

After synthesising the FPGA image (or independently), build the host-side block controllers and emulator application:

```bash
cd rfnoc-openairlink
mkdir -p build && cd build
cmake -DUHD_FPGA_DIR=/path/to/uhd/fpga/ ../
make -j$(nproc)
sudo make install
sudo ldconfig
```

This produces `librfnoc-openairlink.so` (block controllers) and the emulator binaries under `build/apps/`. The install step copies the library to `/usr/local/lib/` so `LD_PRELOAD` is no longer needed.

## Running the emulator

After flashing and rebooting the X410, run the 4-channel sparse FIR emulator from the build directory:

```bash
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so \
  ./apps/oal_4chan_sparse \
  --args "addr=<X410_IP>,clock_source=external,time_source=external" \
  --gnb-freq <center_freq_hz> \
  --ue-freq <center_freq_hz> \
  --rx-gains "30,30,30,30" \
  --tx-gains "30,30,30,30"
```

If the library is installed system-wide (`sudo make install && sudo ldconfig`), the `LD_PRELOAD` can be omitted.

Key options:

| Option | Default | Description |
|--------|---------|-------------|
| `--gnb-freq` | 3619200000 | gNB RX/TX center frequency (Hz) |
| `--ue-freq` | 3619200000 | UE RX/TX center frequency (Hz) |
| `--rx-gains` | — | Per-port RX gains: gNB,UE1,UE2,UE3 |
| `--tx-gains` | — | Per-port TX gains: gNB,UE1,UE2,UE3 |
| `--udt` | 1 | Channel CSV poll interval (s) |
| `--script` | — | Use time-indexed script CSV instead of manual |
| `--doppler` | — | Enable Doppler mode (see below) |

**Port mapping:** inject your tone into **DB0 RX0** (gNB input) and probe **DB0 TX1** for the first downlink output (DL0 → UE1).

To verify the blocks are enumerated correctly before running:

```bash
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so \
  uhd_usrp_probe --tree --args "addr=<X410_IP>"
```

You should see `SparseFIR#0`–`#5`, `Shiftright#0`–`#5`, `SplitStream#0`–`#1`, and `AddSub#0`–`#1` in the block tree.

## Doppler emulation

The emulator supports Doppler effect emulation by rapidly updating complex-coefficient sparse FIR taps. Each tap has a programmable delay, amplitude, Doppler frequency, and initial phase; the host computes time-varying complex coefficients and pushes them to the FPGA at up to ~500–1000 Hz.

**Supported Doppler range:**

| Scenario | Speed | f_d @ 3.5 GHz | Supported |
|----------|-------|----------------|-----------|
| Pedestrian | 3 km/h | 9.7 Hz | Yes |
| Vehicular | 60 km/h | 194 Hz | Yes |
| Fast vehicular | 120 km/h | 389 Hz | Borderline |
| High-speed rail | 350 km/h | 1134 Hz | No (under-sampled) |

Max f_d is ~250 Hz at the default 2 ms update rate (`--doppler-rate 0.002`), or ~500 Hz at 1 ms. Minimum is effectively 0 Hz.

**Quick start — single-tap vehicular (f_d = 194 Hz):**

```bash
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so \
  ./apps/oal_4chan_sparse \
  --args "addr=<X410_IP>,clock_source=external,time_source=external" \
  --gnb-freq <freq_hz> --ue-freq <freq_hz> \
  --rx-gains "30,30,30,30" --tx-gains "30,30,30,30" \
  --doppler '0:0.9:194:0,4,0:0.9:194:0,4,0:0.9:194:0,4,0:0.9:-194:0,4,0:0.9:-194:0,4,0:0.9:-194:0,4' \
  --doppler-rate 0.002
```

The `--doppler` string has 12 comma-separated fields (6 channels × tap-spec + shift). Each tap is `delay:amplitude:fd_hz:phi0_deg`. Multiple taps per channel are space-separated. Downlink channels conventionally get positive f_d, uplink negative.

**Pre-generated script mode** (ms-level updates, lower CPU overhead):

```bash
cd channel_control
python3 generate_doppler_script.py --preset vehicular --num-taps 16 \
    -o chan_4chan_sparse_script.csv

./apps/oal_4chan_sparse --args "..." --script --fast-script
```

Built-in presets: `pedestrian`, `vehicular`, `high-speed`, `static`.

### Doppler spectral shift verification (CW tone)

To directly verify the output spectrum is shifted by f_d Hz, switch the TX X410 to a CW tone and run the spectral verification tests.

**On the TX X410 (`x410_1`):**

```bash
python3 tx_sounder_x410_sa.py \
  --waveform cw \
  --cw-offset 1000000 \
  --freq 3.58e9 \
  --gain 30
```

This transmits a single tone at `--freq + --cw-offset` (e.g. 3.581 GHz) continuously. `--freq` must match `--gnb-freq` on the emulator.

**On the emulator X410, restart with `--doppler`:**

```bash
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so \
  ./apps/oal_4chan_sparse \
  --args "addr=127.0.0.1,clock_source=external,time_source=external" \
  --gnb-freq 3.58e9 --ue-freq 3.58e9 \
  --rx-gains "30,30,30,30" --tx-gains "30,30,30,30" \
  --doppler '0:0.9:200:0,4,0:0.9:200:0,4,0:0.9:200:0,4,0:0.9:-200:0,4,0:0.9:-200:0,4,0:0.9:-200:0,4' \
  --doppler-rate 0.002
```

**Run the spectral verification suite:**

```bash
cd LCHEM_Sounder
./run_verification_doppler.sh --enable-spectral --emulator-freq 3580000000
```

The post-processor multiplies the reference and emulated captures to form a beat signal at exactly f_d Hz, then FFTs it to measure the actual frequency shift. Results appear in `doppler_spectral_shift.pdf` and Section 5 of `doppler_report.txt`. Frequency resolution is ~50 Hz at the default 20 ms capture time, so tests use f_d ≥ 100 Hz.

### Changing image parameters

Open `icores/x410_rfnoc_image_core_4chan_sparse.yml`. The key fields are:

```yaml
image_core_name: lchem_x410_4ch_csfir_16taps   # rename when you change tap count
...
# Under each block instance (sfir_dl0 … sfir_ul2):
parameters:
  NUM_TAPS: 16      # number of sparse FIR taps (must match across all 6 instances)
  MAX_DELAY: 1024   # circular buffer depth in samples (~4.17 µs at 245.76 MHz)
  COEFF_WIDTH: 16   # coefficient bit width
```

After editing, update `image_core_name` to reflect the new configuration (e.g. `lchem_x410_4ch_csfir_32taps`), re-run `cmake ../` so it picks up the new name, then rebuild.

> **Note on timing:** NUM_TAPS=16 is the validated maximum that meets timing at 266 MHz on the X410. NUM_TAPS=32 failed with WNS −0.129 ns due to routing congestion.

## Currently Supported Hardware
1. [NI USRP X410](https://www.ettus.com/all-products/usrp-x410/)


## X410 4-Channel Mode (1 gNB + 3 UEs)

**Port Mapping:**
| Port | Radio | Role |
|------|-------|------|
| DB0 RX0 | radio0:0 | gNB |
| DB0 RX1 | radio0:1 | UE1 |
| DB1 RX0 | radio1:0 | UE2 |
| DB1 RX1 | radio1:1 | UE3 |
