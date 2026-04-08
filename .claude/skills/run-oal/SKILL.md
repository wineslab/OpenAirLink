---
name: run-oal
description: Run the OpenAirLink channel emulator application on an X410. Use when the user wants to start, launch, or run the emulator.
user-invocable: true
allowed-tools: Bash Read
model: sonnet
---

# Run OpenAirLink on X410

Launch the channel emulator application on the X410.

## Arguments

- `$ARGUMENTS` — space-separated options. First arg is X410 hostname. Remaining args passed to the app.
  - Example: `/run-oal x410_0 --gnb-freq 3.55e9 --script`

## Default Command

```bash
ssh <X410_HOST> "cd ~/OpenAirLink/rfnoc-openairlink/build && \
  LD_PRELOAD=/usr/local/lib/librfnoc-openairlink.so \
  ./apps/oal_4chan_sparse \
  --args 'addr=127.0.0.1,clock_source=external,time_source=external' \
  --gnb-freq 3.55e9 \
  --ue-freq 3.55e9 \
  --rx-gains 40,5,5,5 \
  --tx-gains 10,10,10,10"
```

## Steps

1. **Confirm the FPGA image is loaded** by checking blocks:
   ```bash
   ssh <X410_HOST> "uhd_usrp_probe --args addr=127.0.0.1 2>&1 | head -5 | grep fpga"
   ```

2. **Check which app to run**:
   - `oal_4chan_sparse` — 4-channel with sparse FIR (most common)
   - `oal_4chan` — 4-channel with dense FIR
   - `oal_single` — single channel
   - `oal_dual` — dual channel

3. **Launch the application** with the user's parameters. Key options:
   - `--args "addr=127.0.0.1"` — target the local X410 (IMPORTANT: not the remote one)
   - `--clock_source external` / `--time_source external` — for synchronized operation
   - `--gnb-freq` / `--ue-freq` — center frequency in Hz
   - `--rx-gains "g0,g1,g2,g3"` / `--tx-gains "g0,g1,g2,g3"` — per-port gains (gNB,UE1,UE2,UE3)
   - `--script` — use time-indexed CSV mode instead of manual polling
   - `--udt <seconds>` — CSV poll interval for manual mode

4. **Common issues**:
   - `LookupError: 0/SparseFIR#0` — wrong FPGA image loaded, or library not installed. Run `/build-oal` or `/deploy`.
   - `LookupError: 0/FIR#0` — running the dense app (`oal_4chan`) against a sparse FPGA image.
   - App connects to wrong X410 — make sure `--args "addr=127.0.0.1"` targets the local device.
   - `No devices found` — X410 not booted or network issue.

5. **Channel configuration**: While the emulator runs, edit CSV files in `channel_control/`:
   - Sparse: `chan_4chan_sparse_manually.csv` — format: `delay0:coeff0 delay1:coeff1 ..., shift`
   - Dense: `chan_4chan_manually.csv` — format: `coeff0 coeff1 ... coeff40, shift`
