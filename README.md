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