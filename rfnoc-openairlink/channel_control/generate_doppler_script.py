#!/usr/bin/env python3
"""
Generate time-varying complex tap coefficients for Doppler channel emulation.

Produces a script-mode CSV file compatible with oal_4chan_sparse --script.
Each row contains a time index and 6 channels (3 DL + 3 UL) of sparse FIR
taps with complex coefficients that rotate in phase according to per-tap
Doppler shifts.

Channel model specification is provided via a JSON file. Example:

{
    "duration": 10.0,
    "update_interval": 0.002,
    "shift_value": 6,
    "channels": {
        "dl0": {
            "taps": [
                {"delay": 0,   "amplitude": 1.0,  "fd_hz": 100.0,  "phi0_deg": 0},
                {"delay": 50,  "amplitude": 0.5,  "fd_hz": -80.0,  "phi0_deg": 45},
                {"delay": 200, "amplitude": 0.25, "fd_hz": 150.0,  "phi0_deg": 90}
            ]
        },
        "dl1": { "taps": [...] },
        "dl2": { "taps": [...] },
        "ul0": { "taps": [...] },
        "ul1": { "taps": [...] },
        "ul2": { "taps": [...] }
    }
}

Usage:
    python generate_doppler_script.py model.json -o chan_4chan_sparse_script.csv
    python generate_doppler_script.py --preset vehicular-3gpp -o script.csv
"""

import argparse
import json
import math
import sys
import os

CHANNEL_ORDER = ["dl0", "dl1", "dl2", "ul0", "ul1", "ul2"]
MAX_COEFF = 32767  # Q1.15 max


def quantize_coeff(value):
    """Quantize a float in [-1, 1] to signed 16-bit Q1.15."""
    clamped = max(-1.0, min(1.0, value))
    return int(round(clamped * MAX_COEFF))


def format_tap(delay, coeff_re, coeff_im):
    """Format a single tap as 'delay:re+imj' or 'delay:re' string."""
    if coeff_im == 0:
        return f"{delay}:{coeff_re}"
    elif coeff_im > 0:
        return f"{delay}:{coeff_re}+{coeff_im}j"
    else:
        return f"{delay}:{coeff_re}{coeff_im}j"


def generate_channel_taps(channel_spec, t, num_taps):
    """Compute complex coefficients for one channel at time t.

    Each tap's coefficient is: amplitude * exp(j*(2*pi*fd*t + phi0))
    """
    taps = channel_spec.get("taps", [])
    delays = []
    coeffs_re = []
    coeffs_im = []

    for tap in taps:
        delay = tap["delay"]
        amplitude = tap.get("amplitude", 1.0)
        fd_hz = tap.get("fd_hz", 0.0)
        phi0 = math.radians(tap.get("phi0_deg", 0.0))

        phase = 2.0 * math.pi * fd_hz * t + phi0
        re = amplitude * math.cos(phase)
        im = amplitude * math.sin(phase)

        delays.append(delay)
        coeffs_re.append(quantize_coeff(re))
        coeffs_im.append(quantize_coeff(im))

    # Pad to num_taps
    while len(delays) < num_taps:
        delays.append(0)
        coeffs_re.append(0)
        coeffs_im.append(0)

    return delays, coeffs_re, coeffs_im


def generate_script_csv(model, num_taps, output_path):
    """Generate the full script-mode CSV file."""
    duration = model["duration"]
    dt = model["update_interval"]
    default_shift = model.get("shift_value", 0)

    steps = int(duration / dt) + 1
    actual_rate = 1.0 / dt

    print(f"Generating Doppler script CSV:")
    print(f"  Duration:        {duration:.3f} s")
    print(f"  Update interval: {dt*1000:.3f} ms ({actual_rate:.0f} Hz)")
    print(f"  Steps:           {steps}")
    print(f"  Num taps:        {num_taps}")
    print(f"  Output:          {output_path}")

    # Validate Nyquist
    max_fd = 0.0
    for ch_name in CHANNEL_ORDER:
        ch = model.get("channels", {}).get(ch_name, {})
        for tap in ch.get("taps", []):
            max_fd = max(max_fd, abs(tap.get("fd_hz", 0.0)))

    if max_fd > 0:
        nyquist_rate = 2.0 * max_fd
        if actual_rate < nyquist_rate:
            print(f"  WARNING: Update rate {actual_rate:.0f} Hz < Nyquist rate "
                  f"{nyquist_rate:.0f} Hz for max fd={max_fd:.1f} Hz")
            print(f"  Consider reducing update_interval to {1.0/(2.5*max_fd)*1000:.3f} ms")
        else:
            print(f"  Max Doppler:     {max_fd:.1f} Hz (Nyquist OK: "
                  f"{actual_rate:.0f} >= {nyquist_rate:.0f} Hz)")

    with open(output_path, "w") as f:
        for step in range(steps):
            t = step * dt

            parts = [f"{t:.6f}"]

            for ch_idx, ch_name in enumerate(CHANNEL_ORDER):
                ch_spec = model.get("channels", {}).get(ch_name, {"taps": []})
                ch_shift = ch_spec.get("shift_value", default_shift)
                delays, coeffs_re, coeffs_im = generate_channel_taps(ch_spec, t, num_taps)

                tap_strs = []
                for i in range(num_taps):
                    tap_strs.append(format_tap(delays[i], coeffs_re[i], coeffs_im[i]))

                taps_field = " ".join(tap_strs)

                parts.append(f" {taps_field}")
                parts.append(f" {ch_shift}")

            # Join with commas; last UL channel ends the line (no trailing comma)
            # Format: time, taps_dl0, shift_dl0, taps_dl1, shift_dl1, ..., taps_ul2, shift_ul2
            line_parts = []
            line_parts.append(parts[0])  # time
            for i in range(1, len(parts) - 1):
                line_parts.append(parts[i])
            line_parts.append(parts[-1])

            f.write(",".join(line_parts) + "\n")

        f.write("eos\n")

    file_size_kb = os.path.getsize(output_path) / 1024.0
    print(f"  File size:       {file_size_kb:.1f} KB")
    print("Done.")


def make_preset_model(preset_name, num_taps):
    """Create a channel model from a named preset."""
    presets = {
        "pedestrian": {
            "description": "3GPP pedestrian (3 km/h @ 3.5 GHz, fd_max ~9.7 Hz)",
            "duration": 30.0,
            "update_interval": 0.010,  # 100 Hz, well above 2*9.7=19.4 Hz
            "shift_value": 4,
            "channels": {
                "dl0": {"taps": [
                    {"delay": 0,   "amplitude": 0.8,  "fd_hz":  5.0,  "phi0_deg": 0},
                    {"delay": 30,  "amplitude": 0.4,  "fd_hz": -3.0,  "phi0_deg": 120},
                    {"delay": 100, "amplitude": 0.2,  "fd_hz":  9.7,  "phi0_deg": 240},
                ]},
            }
        },
        "vehicular": {
            "description": "Vehicular (60 km/h @ 3.5 GHz, fd_max ~194 Hz)",
            "duration": 5.0,
            "update_interval": 0.001,  # 1000 Hz > 2*194=388 Hz
            "shift_value": 4,
            "channels": {
                "dl0": {"taps": [
                    {"delay": 0,   "amplitude": 0.9,  "fd_hz":  194.0, "phi0_deg": 0},
                    {"delay": 20,  "amplitude": 0.5,  "fd_hz": -120.0, "phi0_deg": 60},
                    {"delay": 80,  "amplitude": 0.3,  "fd_hz":  50.0,  "phi0_deg": 180},
                    {"delay": 200, "amplitude": 0.15, "fd_hz": -180.0, "phi0_deg": 300},
                ]},
            }
        },
        "high-speed": {
            "description": "High-speed train (350 km/h @ 3.5 GHz, fd_max ~1134 Hz)",
            "duration": 2.0,
            "update_interval": 0.001,  # 1000 Hz — under Nyquist! Best effort.
            "shift_value": 4,
            "channels": {
                "dl0": {"taps": [
                    {"delay": 0,  "amplitude": 0.95, "fd_hz":  1134.0, "phi0_deg": 0},
                    {"delay": 10, "amplitude": 0.2,  "fd_hz": -800.0,  "phi0_deg": 90},
                ]},
            }
        },
        "static": {
            "description": "Static complex channel (no Doppler, verifies complex taps)",
            "duration": 10.0,
            "update_interval": 1.0,
            "shift_value": 4,
            "channels": {
                "dl0": {"taps": [
                    {"delay": 0,   "amplitude": 0.707, "fd_hz": 0, "phi0_deg": 45},
                    {"delay": 100, "amplitude": 0.5,   "fd_hz": 0, "phi0_deg": 135},
                ]},
            }
        },
    }

    if preset_name not in presets:
        print(f"Unknown preset '{preset_name}'. Available: {', '.join(presets.keys())}")
        sys.exit(1)

    model = presets[preset_name]
    print(f"Preset: {preset_name} — {model['description']}")

    # Replicate dl0 to all channels if they're not specified
    dl0_spec = model["channels"].get("dl0", {"taps": []})
    for ch_name in CHANNEL_ORDER:
        if ch_name not in model["channels"]:
            model["channels"][ch_name] = dl0_spec

    return model


def main():
    parser = argparse.ArgumentParser(
        description="Generate Doppler channel emulation script CSV for OpenAirLink",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s model.json -o script.csv
  %(prog)s --preset pedestrian -o script.csv
  %(prog)s --preset vehicular --num-taps 16 -o script.csv
  %(prog)s --preset vehicular --duration 10 --update-interval 0.002 -o script.csv
        """)

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("model_json", nargs="?",
                       help="JSON channel model specification file")
    group.add_argument("--preset", choices=["pedestrian", "vehicular", "high-speed", "static"],
                       help="Use a built-in channel model preset")

    parser.add_argument("-o", "--output", required=True,
                        help="Output CSV file path")
    parser.add_argument("--num-taps", type=int, default=16,
                        help="Number of FIR taps (default: 16, must match FPGA)")
    parser.add_argument("--duration", type=float, default=None,
                        help="Override model duration (seconds)")
    parser.add_argument("--update-interval", type=float, default=None,
                        help="Override update interval (seconds)")

    args = parser.parse_args()

    if args.preset:
        model = make_preset_model(args.preset, args.num_taps)
    else:
        with open(args.model_json) as f:
            model = json.load(f)

    # Apply overrides
    if args.duration is not None:
        model["duration"] = args.duration
    if args.update_interval is not None:
        model["update_interval"] = args.update_interval

    generate_script_csv(model, args.num_taps, args.output)


if __name__ == "__main__":
    main()
