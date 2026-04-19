# OpenAirLink: Reproducible Wireless Channel Emulation using Software Defined Radios

An open-source channel emulator for reproducible testing of wireless mobility scenarios.
The emulator implements a FIR filter on Software Defined Radios, the NI USRP (formerly by Ettus). 
OpenAirLink was developed by the [Chair of Communication Networks](https://www.ce.cit.tum.de/lkn/startseite/) at the Technical University of Munich. 

## Requirements
- UHD 4.4

To rebuild FPGA image:
- [Vivado 2021.1](https://www.xilinx.com/support/download/index.html/content/xilinx/en/downloadNav/vivado-design-tools/archive.html)
- [AR76780 Patch](https://support.xilinx.com/s/article/76780?language=en_US)

## Installation
Clone this repository:
```
git clone https://github.com/N3Martix/OpenAirLink.git
```
Use following steps to install OpenAirLink:
```
cd ~/OpenAirLink/rfnoc-openairlink
mkdir build && cd build
cmake -DUHD_FPGA_DIR=<path-to-uhd>/uhd/fpga/ ../
make
sudo make install
sudo ldconfig
```
Load FPGA image to your USRP:
```
cd ~/OpenAirLink/fpga-openairlink
uhd_image_loader --args="type=x400" --fpga-path="usrp_x410_fpga_UC_200.bit"
```
To check installation, run:
```
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so uhd_usrp_probe
```
If OpenAirLink is correctly installed, the output should look like:
```
|     _____________________________________________________
   |    /
   |   |       RFNoC blocks on this device:
   ...
   |   |   * 0/FIR#0
   |   |   * 0/FIR#1
   |   |   * 0/Shiftright#0
   |   |   * 0/Shiftright#1
   ...
```

## Usage
**1. Lanuch OpenAirLink**

Lanuch with follow command:
```
cd ~/OpenAirLink/rfnoc-openairlink/build
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so ./apps/oal_single
```
OpenAirLink also supports two channels running independently and simultaneously. To do so, replace `oal_single` with `oal_dual`.

The OpenAirLink's channel configuration has two models:

- **Manually**: By default, OpenAirLink periodically scans the configuration file in the `channel_control/` folder to update the channel. The frequency of updates can be adjusted using the `--udt` argument.
- **Script**: The configuration is sent to the USRP if the emulator's running time exceeds its time index. To run the script mode, use the argument `--script`.

**2. Channel coefficient generation**
 TODO


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

### Flashing to the X410

```bash
uhd_image_loader --args "type=x4xx,addr=<X410_IP>" \
  --fpga-path build/bitstreams/<timestamp>_lchem_x410_4ch_csfir_16taps/lchem_x410_4ch_csfir_16taps.bit
# Reboot the X410 after loading
```

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

## X410 Quick Start

**Build FPGA Image:**
```bash
cd rfnoc-openairlink/build
cmake -DUHD_FPGA_DIR=/path/to/uhd/fpga/ ../
make x410_rfnoc_image_core
```
The bitstream will be at: `icores/build-x410_rfnoc_image_core/x4xx.bit`

**Load to X410:**
```bash
uhd_image_loader --args="type=x4xx,addr=<X410_IP>" --fpga-path="x4xx.bit"
reboot  # Reboot X410 after loading
```

**Hardware Connections:**
- **Downlink:** TX → X410 Port A (DB0) RX → X410 Port B (DB1) TX → RX
- **Uplink:** TX → X410 Port B (DB1) RX → X410 Port A (DB0) TX → RX

**Channel Updates:**
Edit `channel_control/chan_singel_manually.csv` while emulator runs:
```
32767 0 0 0 ... 0, 6    # Format: FIR_taps (41 int16 values), shift_value
```
- FIR taps: Channel impulse response (from ray tracing)
- Shift value: Attenuation (4-7 typical)

**Configuration:**
- Manual mode: Real-time updates via CSV editing
- Script mode: Time-based channel changes with `--script` flag
- Update rate: Adjust with `--udt <seconds>` argument

**Run the Emulation:**
```bash
cd ~/OpenAirLink/rfnoc-openairlink/build
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so ./apps/oal_single
```

## X410 4-Channel Mode (1 gNB + 3 UEs)

**Build FPGA Image:**
```bash
cd rfnoc-openairlink/build
cmake -DUHD_FPGA_DIR=/path/to/uhd/fpga/ ../
make x410_rfnoc_image_core_4chan
```

**Load to X410:**
```bash
uhd_image_loader --args="type=x4xx,addr=<X410_IP>" --fpga-path="icores/build-x410_rfnoc_image_core_4chan/x4xx.bit"
# Reboot X410 after loading
```

**Port Mapping:**
| Port | Radio | Role |
|------|-------|------|
| DB0 RX0 | radio0:0 | gNB |
| DB0 RX1 | radio0:1 | UE1 |
| DB1 RX0 | radio1:0 | UE2 |
| DB1 RX1 | radio1:1 | UE3 |

**Run the Emulator:**
```bash
LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so ./apps/oal_4chan
```

**Per-Port Gain Control:**
```bash
# Format: gNB,UE1,UE2,UE3
./apps/oal_4chan --rx-gains "0,10,15,20" --tx-gains "5,10,10,10"
```

**Channel Configuration:**
Edit `channel_control/chan_4chan_manually.csv` while running (6 channels: 3 DL + 3 UL)
