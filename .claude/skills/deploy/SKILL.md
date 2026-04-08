---
name: deploy
description: Deploy FPGA bitstream to an X410 and build/install OAL host software. Covers DTS preprocessing, image loading, reboot, and library installation. Use when the user wants to flash, deploy, burn, load, build, or install on the X410.
user-invocable: true
allowed-tools: Bash Read Glob
model: sonnet
---

# Deploy to X410

Flash the FPGA image and build/install the host software on the target X410.

## Arguments

- `$ARGUMENTS` — the X410 SSH hostname or IP (e.g., `x410_0`, `10.112.1.97`). Required.

## Part 1: FPGA Image

1. **Locate the bitstream**:
   ```bash
   ls /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE/x4xx.bit
   ```
   If not found, tell the user to run `/synth` first.

2. **Preprocess the DTS** (required — raw DTS causes "no component version information" errors):
   ```bash
   cd /home/wines/Desktop/uhd/fpga/usrp3/top/x400 && \
   gcc -o /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE/usrp_x410_fpga_OAL_SPARSE.dts \
     -C -E -I dts -nostdinc -undef -x assembler-with-cpp -D__DTS__ \
     /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE/device_tree.dts
   ```

3. **Copy both files to the X410**:
   ```bash
   scp /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE/x4xx.bit <X410_HOST>:~/usrp_x410_fpga_OAL_SPARSE.bit
   scp /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build/icores/build-usrp_x410_fpga_OAL_SPARSE/usrp_x410_fpga_OAL_SPARSE.dts <X410_HOST>:~/usrp_x410_fpga_OAL_SPARSE.dts
   ```

4. **Load the image on X410**:
   ```bash
   ssh <X410_HOST> "uhd_image_loader --args 'type=x4xx,addr=127.0.0.1' --fpga-path ~/usrp_x410_fpga_OAL_SPARSE.bit"
   ```

5. **Reboot**:
   ```bash
   ssh <X410_HOST> "reboot"
   ```
   Tell the user to wait ~60 seconds for the X410 to come back up.

## Part 2: Build & Install OAL Host Software

6. **Check if source code is on the X410**:
   ```bash
   ssh <X410_HOST> "ls ~/OpenAirLink/rfnoc-openairlink/CMakeLists.txt 2>/dev/null || ls ~/emulator_*/OpenAirLink/rfnoc-openairlink/CMakeLists.txt 2>/dev/null"
   ```
   If not found, the user needs to clone or copy the repo to the X410 first.

7. **Build and install** (find the correct path from step 6):
   First, find the UHD FPGA directory on the X410:
   ```bash
   ssh <X410_HOST> "find /home /root /opt /usr -maxdepth 5 -path '*/uhd/fpga/usrp3' -type d 2>/dev/null | head -3"
   ```
   Then build with the discovered path:
   ```bash
   ssh <X410_HOST> "cd <OAL_PATH>/rfnoc-openairlink && mkdir -p build && cd build && cmake -DUHD_FPGA_DIR=<UHD_FPGA_PATH> .. && make -j\$(nproc) && make install && ldconfig"
   ```
   If no UHD FPGA source is found on the X410, `cmake ..` without `-DUHD_FPGA_DIR` may still work if only building the host library (not FPGA images). Try it — if cmake complains about missing FPGA dir, ask the user where UHD is installed on that device.

8. **Verify library installed**:
   ```bash
   ssh <X410_HOST> "ldconfig -p | grep rfnoc-openairlink"
   ```
   Should show `librfnoc-openairlink.so`.

## Part 3: Verify Everything

9. **Block discovery** — confirm custom blocks are recognized:
   ```bash
   ssh <X410_HOST> "uhd_usrp_probe --args addr=127.0.0.1 2>&1 | grep -E 'SparseFIR|Shiftright|Block#|Could not find block'"
   ```
   - `SparseFIR#N` and `Shiftright#N` = success
   - `Block#N` or `Could not find block` = library not found, check install
   - If probe fails with claim error, device is already in use

**IMPORTANT**: The user has multiple X410s on the network. Make sure to deploy to the correct one. `addr=127.0.0.1` loads on the X410 you're SSH'd into.
