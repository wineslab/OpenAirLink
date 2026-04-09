#!/usr/bin/env python3
"""
X410 Sparse FIR FPGA Resource Trade-off Plotter

Analytically computes DSP/BRAM utilization for different configurations
of (NUM_TAPS, N_channels, MAX_DELAY) without requiring FPGA builds.

Based on measured post-route data from the OAL_SPARSE build:
  - Platform overhead: ~2791 DSPs, ~210 BRAM18
  - X410 (xczu28dr): 4272 DSP48E2, 2160 BRAM18, 1080 BRAM36

Note: Complex coefficients (h_re + j*h_im) require 4 DSP48 per tap
(h_re*I, h_im*Q, h_re*Q, h_im*I). BRAM count unchanged (2 per tap).
"""

import matplotlib
matplotlib.use("Agg")
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.patches import FancyArrowPatch

# ── X410 FPGA constants (xczu28dr) ──────────────────────────────
TOTAL_DSP = 4272
TOTAL_BRAM18 = 2160
TOTAL_BRAM36 = 1080
TOTAL_LUT = 425280

# ── Measured platform overhead (non-FIR, from post_route_util.rpt) ──
PLATFORM_DSP = 2791
PLATFORM_BRAM18 = 210

# ── Safe utilization ceilings ───────────────────────────────────
SAFE_DSP_FRAC = 0.80       # 80% — timing closure gets hard above this
WARN_DSP_FRAC = 0.85       # 85% — likely timing failures
SAFE_BRAM_FRAC = 0.90      # BRAM is less sensitive to congestion

# ── Clock / physical constants ──────────────────────────────────
CE_CLK_HZ = 122.88e6       # compute engine clock
SAMPLE_PERIOD_NS = 1e9 / CE_CLK_HZ  # ~8.14 ns
SPEED_OF_LIGHT = 3e8       # m/s


def fir_dsps(n_channels, num_taps):
    """DSP48E2 count for all FIR blocks (complex coefficients: 4 DSPs per tap)."""
    return n_channels * 4 * num_taps


def fir_bram18(n_channels, num_taps):
    """BRAM18 count for all FIR blocks (one per tap per I/Q path)."""
    return n_channels * 2 * num_taps


def delay_spread_us(max_delay):
    """Maximum delay spread in microseconds."""
    return max_delay * SAMPLE_PERIOD_NS / 1000


def delay_spread_m(max_delay):
    """Maximum delay spread converted to distance (meters)."""
    return max_delay * (1 / CE_CLK_HZ) * SPEED_OF_LIGHT


def plot_taps_vs_channels(ax):
    """Plot 1: NUM_TAPS vs N_channels heatmap with DSP utilization."""
    taps_range = np.array([4, 8, 16, 32, 64, 128, 256])
    chan_range = np.arange(1, 13)

    dsp_util = np.zeros((len(chan_range), len(taps_range)))
    for i, ch in enumerate(chan_range):
        for j, t in enumerate(taps_range):
            total = PLATFORM_DSP + fir_dsps(ch, t)
            dsp_util[i, j] = total / TOTAL_DSP * 100

    # Custom colormap: green → yellow → red → dark red
    colors = ["#2ecc71", "#f1c40f", "#e74c3c", "#8b0000"]
    cmap = LinearSegmentedColormap.from_list("dsp", colors, N=256)

    im = ax.imshow(dsp_util, cmap=cmap, aspect="auto",
                   vmin=60, vmax=120, origin="lower")

    # Annotate each cell
    for i in range(len(chan_range)):
        for j in range(len(taps_range)):
            val = dsp_util[i, j]
            color = "white" if val > 90 else "black"
            marker = ""
            if val > 100:
                marker = "\n(FAIL)"
            elif val > WARN_DSP_FRAC * 100:
                marker = "\n(risky)"
            ax.text(j, i, f"{val:.0f}%{marker}", ha="center", va="center",
                    fontsize=7, fontweight="bold", color=color)

    # Mark current config
    curr_ch_idx = np.where(chan_range == 6)[0][0]
    curr_tap_idx = np.where(taps_range == 32)[0][0]
    ax.plot(curr_tap_idx, curr_ch_idx, "s", markersize=22,
            markeredgecolor="cyan", markerfacecolor="none", markeredgewidth=2.5)
    ax.annotate("current\nconfig", (curr_tap_idx, curr_ch_idx),
                xytext=(curr_tap_idx + 1.3, curr_ch_idx + 1.5),
                fontsize=8, color="cyan", fontweight="bold",
                arrowprops=dict(arrowstyle="->", color="cyan", lw=1.5))

    # 80% feasibility contour
    cs = ax.contour(dsp_util, levels=[SAFE_DSP_FRAC * 100],
                    colors=["white"], linewidths=2, linestyles="--")
    ax.clabel(cs, fmt="80%% safe", fontsize=8, colors=["white"])

    ax.set_xticks(range(len(taps_range)))
    ax.set_xticklabels(taps_range)
    ax.set_yticks(range(len(chan_range)))
    ax.set_yticklabels(chan_range)
    ax.set_xlabel("NUM_TAPS (per FIR block)")
    ax.set_ylabel("Number of FIR channels")
    ax.set_title("DSP48E2 Utilization (%)\nX410 xczu28dr — 4272 DSPs total")

    cbar = plt.colorbar(im, ax=ax, shrink=0.8)
    cbar.set_label("DSP Utilization %")


def plot_max_taps_vs_channels(ax):
    """Plot 2: Maximum feasible NUM_TAPS for each channel count."""
    chan_range = np.arange(1, 13)
    power2_taps = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]

    max_taps_80 = []
    max_taps_100 = []
    for ch in chan_range:
        # Find largest power-of-2 taps that fits under 80%
        best_80 = 0
        best_100 = 0
        for t in power2_taps:
            total = PLATFORM_DSP + fir_dsps(ch, t)
            if total <= TOTAL_DSP * SAFE_DSP_FRAC:
                best_80 = t
            if total <= TOTAL_DSP:
                best_100 = t
        max_taps_80.append(best_80)
        max_taps_100.append(best_100)

    bar_width = 0.35
    x = np.arange(len(chan_range))
    bars1 = ax.bar(x - bar_width / 2, max_taps_100, bar_width,
                   label="Hard limit (100%)", color="#e74c3c", alpha=0.7)
    bars2 = ax.bar(x + bar_width / 2, max_taps_80, bar_width,
                   label="Safe limit (80%)", color="#2ecc71", alpha=0.9)

    # Annotate bars
    for bar in bars1:
        h = bar.get_height()
        if h > 0:
            ax.text(bar.get_x() + bar.get_width() / 2, h + 2,
                    str(int(h)), ha="center", va="bottom", fontsize=7,
                    color="#e74c3c", fontweight="bold")
    for bar in bars2:
        h = bar.get_height()
        if h > 0:
            ax.text(bar.get_x() + bar.get_width() / 2, h + 2,
                    str(int(h)), ha="center", va="bottom", fontsize=7,
                    color="#2ecc71", fontweight="bold")

    # Mark current config
    curr_idx = np.where(chan_range == 6)[0][0]
    ax.axvline(x=curr_idx, color="cyan", linestyle=":", alpha=0.7)
    ax.annotate("current: 6ch, 32 taps", (curr_idx, 32),
                xytext=(curr_idx + 1.5, 45), fontsize=8, color="cyan",
                fontweight="bold",
                arrowprops=dict(arrowstyle="->", color="cyan", lw=1.5))

    ax.set_xticks(x)
    ax.set_xticklabels(chan_range)
    ax.set_xlabel("Number of FIR channels")
    ax.set_ylabel("Maximum NUM_TAPS (power of 2)")
    ax.set_title("Max Feasible Taps per Channel Count")
    ax.set_yscale("log", basey=2)
    ax.set_yticks(power2_taps[:9])
    ax.set_yticklabels(power2_taps[:9])
    ax.set_ylim(0.5, 600)
    ax.legend(loc="upper right")
    ax.grid(axis="y", alpha=0.3)


def plot_delay_vs_taps(ax):
    """Plot 3: MAX_DELAY vs NUM_TAPS — delay spread vs resolution."""
    max_delay_range = [64, 128, 256, 512, 1024, 2048, 4096]
    taps_range = [4, 8, 16, 32, 64, 128]

    # BRAM18 usage (for 6 channels) — each tap needs one BRAM18 per I/Q
    # MAX_DELAY only affects BRAM depth, not count (up to 1024 in one BRAM18)
    # Beyond 1024 deep, need BRAM36 or cascaded BRAMs

    for t in taps_range:
        bram_counts = []
        for d in max_delay_range:
            # Each BRAM18 holds 1024x18b. For 16-bit data, 1 BRAM18 per tap
            # if MAX_DELAY <= 1024. For larger delays, need more depth.
            brams_per_tap = max(1, d // 1024)
            n_ch = 6
            total_bram18 = PLATFORM_BRAM18 + n_ch * 2 * t * brams_per_tap
            bram_counts.append(total_bram18 / TOTAL_BRAM18 * 100)
        ax.plot(max_delay_range, bram_counts, "o-", label=f"{t} taps",
                linewidth=2, markersize=5)

    ax.axhline(y=SAFE_BRAM_FRAC * 100, color="red", linestyle="--",
               alpha=0.7, label="90% safe limit")
    ax.axhline(y=100, color="darkred", linestyle="-", alpha=0.5)

    ax.set_xscale("log", basex=2)
    ax.set_xlabel("MAX_DELAY (samples)")
    ax.set_ylabel("BRAM18 Utilization (%)")
    ax.set_title("BRAM Usage: MAX_DELAY vs NUM_TAPS\n(6 channels, I+Q)")
    ax.legend(fontsize=8, ncol=2)
    ax.grid(alpha=0.3)

    # Secondary x-axis: delay spread in microseconds
    ax2 = ax.twiny()
    ax2.set_xscale("log", basex=2)
    ax2.set_xlim(ax.get_xlim())
    delay_labels = [f"{delay_spread_us(d):.1f}" for d in max_delay_range]
    ax2.set_xticks(max_delay_range)
    ax2.set_xticklabels(delay_labels, fontsize=7)
    ax2.set_xlabel("Delay spread (us) @ 122.88 MHz")


def plot_resource_summary(ax):
    """Plot 4: Stacked bar showing resource breakdown for key configs."""
    configs = [
        ("6ch\n16 taps\n(prev)", 6, 16, 1024),
        ("6ch\n32 taps\n(current)", 6, 32, 1024),
        ("6ch\n64 taps", 6, 64, 1024),
        ("4ch\n64 taps", 4, 64, 1024),
        ("4ch\n128 taps", 4, 128, 1024),
        ("2ch\n128 taps", 2, 128, 1024),
        ("6ch\n128 taps\n(FAIL)", 6, 128, 1024),
    ]

    labels = [c[0] for c in configs]
    platform_dsps = [PLATFORM_DSP / TOTAL_DSP * 100] * len(configs)
    fir_dsps_pct = [fir_dsps(c[1], c[2]) / TOTAL_DSP * 100 for c in configs]

    x = np.arange(len(configs))
    width = 0.6

    bars1 = ax.bar(x, platform_dsps, width, label="Platform (fixed)",
                   color="#3498db", alpha=0.8)
    bars2 = ax.bar(x, fir_dsps_pct, width, bottom=platform_dsps,
                   label="Sparse FIR blocks", color="#e67e22", alpha=0.8)

    # Total labels
    for i, c in enumerate(configs):
        total = PLATFORM_DSP + fir_dsps(c[1], c[2])
        pct = total / TOTAL_DSP * 100
        color = "darkred" if pct > 100 else ("red" if pct > 80 else "black")
        ax.text(i, platform_dsps[i] + fir_dsps_pct[i] + 1,
                f"{pct:.0f}%", ha="center", va="bottom",
                fontsize=9, fontweight="bold", color=color)

    ax.axhline(y=80, color="green", linestyle="--", alpha=0.7, label="80% safe")
    ax.axhline(y=100, color="red", linestyle="--", alpha=0.7, label="100% hard limit")

    # Highlight current config
    ax.bar(x[1], platform_dsps[1] + fir_dsps_pct[1], width,
           fill=False, edgecolor="cyan", linewidth=2.5)

    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8)
    ax.set_ylabel("DSP48E2 Utilization (%)")
    ax.set_title("Resource Breakdown: Key Configurations")
    ax.set_ylim(0, 115)
    ax.legend(loc="upper left", fontsize=8)
    ax.grid(axis="y", alpha=0.3)


def main():
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle("X410 Sparse FIR (Complex Coeff) — FPGA Resource Trade-off Analysis\n"
                 "xczu28dr | 4272 DSPs | 2160 BRAM18 | Platform: 2791 DSPs | 4 DSP/tap",
                 fontsize=13, fontweight="bold")

    plot_taps_vs_channels(axes[0, 0])
    plot_max_taps_vs_channels(axes[0, 1])
    plot_delay_vs_taps(axes[1, 0])
    plot_resource_summary(axes[1, 1])

    plt.tight_layout(rect=[0, 0, 1, 0.93])

    out_path = "fpga_tradeoff_analysis.png"
    plt.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")


if __name__ == "__main__":
    main()
