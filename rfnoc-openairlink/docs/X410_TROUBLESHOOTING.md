# X410 4-Channel Emulator Guide

## Building the Host Application

```bash
cd /home/wines/Desktop/OpenAirLink/rfnoc-openairlink/build
cmake ..
make -j$(nproc) oal_4chan
```

## Running the Emulator

```bash
./apps/oal_4chan --args "addr=<X410_IP>" --gnb-freq 3619.2e6 --ue-freq 3619.2e6
```

**Options:**
| Option | Default | Description |
|--------|---------|-------------|
| `--gnb-freq` | 3619.2 MHz | gNB center frequency |
| `--ue-freq` | 3619.2 MHz | UE center frequency |
| `--rx-gain` | 0 dB | RX gain |
| `--tx-gain` | 0 dB | TX gain |
| `--script` | off | Use time-scripted config |

**Channel config files:** `channel_control/chan_4chan_manually.csv` or `chan_4chan_script.csv`

---

# Troubleshooting

## Error: "no component version information in DTS file"

**Symptom:**
```
[ERROR] [MPM.PeriphManager] no component version information in DTS file
Error: RuntimeError: Error during RPC call to `update_component'
```

**Cause:** The generated `.dts` file contains unresolved `#include` directives. MPM expects a preprocessed DTS with embedded version comments.

**Fix:**

1. Generate version info DTSI:
```bash
cd /home/wines/Desktop/uhd/fpga/usrp3/top/x400
python3 tools/parse_versions_for_dts.py \
  --input regmap/x410/versioning_regs_regmap_utils.vh \
  --output dts/x410-version-info.dtsi \
  --components fpga,cpld_ifc,db_gpio_ifc,rf_core_100m,rf_core_400m
```

2. Preprocess the DTS:
```bash
gcc -o build/<IMAGE_NAME>.dts -C -E -I dts -nostdinc -undef \
  -x assembler-with-cpp -D__DTS__ \
  /path/to/rfnoc-openairlink/icores/build-<IMAGE_NAME>/device_tree.dts
```

3. Copy both `.bit` and preprocessed `.dts` to X410 and load:
```bash
scp build/<IMAGE_NAME>.{bit,dts} root@<X410_IP>:~/
# On X410:
uhd_image_loader --args "type=x4xx,addr=127.0.0.1" --fpga-path <IMAGE_NAME>.bit
```

The preprocessed DTS should contain `// mpm_version ...` comments (~12KB vs ~500B raw).
