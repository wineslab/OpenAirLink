#!/usr/bin/env python3
"""
X410 Sparse FIR FPGA Resource Trade-off Plotter — paper-quality figures.

Each sub-plot is saved as an individual PDF.

LaTeX packages required (install once):
  sudo apt-get install texlive-latex-extra texlive-fonts-recommended \
                       cm-super dvipng
"""

import matplotlib as mpl
mpl.use("Agg")
mpl.rc("text", usetex=True)
mpl.rcParams["text.latex.preamble"] = r"\usepackage{mathptmx}"
mpl.rcParams["font.family"] = "serif"
mpl.rcParams["axes.formatter.use_mathtext"] = True

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LinearSegmentedColormap

plt.rcParams["font.size"]       = 15
plt.rcParams["lines.linewidth"] = 2
plt.rcParams["figure.figsize"]  = (6, 4)
plt.rcParams["savefig.bbox"]    = "tight"
plt.rcParams["savefig.dpi"]     = 300
plt.rcParams["savefig.format"]  = "pdf"
plt.rcParams["grid.linestyle"]  = "--"
plt.rcParams["grid.alpha"]      = 0.7

# ── Colors ────────────────────────────────────────────────────────
GRAY = "#6c757d"
BLUE = "#0d6efd"
RED  = "#dc3545"

# ── X410 FPGA constants (xczu28dr) ────────────────────────────────
TOTAL_DSP       = 4272
TOTAL_BRAM18    = 2160
PLATFORM_DSP    = 2791    # measured post-route overhead
PLATFORM_BRAM18 = 210

SAFE_DSP_FRAC   = 0.80    # timing closure degrades above this
SAFE_BRAM_FRAC  = 0.90

CE_CLK_HZ        = 122.88e6
SAMPLE_PERIOD_NS = 1e9 / CE_CLK_HZ

DSP_REAL    = 2   # DSP48E2 per tap — real coefficients (h*I, h*Q)
DSP_COMPLEX = 4   # DSP48E2 per tap — complex (h_re*I, h_im*Q, h_re*Q, h_im*I)


# ── Helpers ───────────────────────────────────────────────────────
def fir_dsps(n_ch, n_taps, dsp_per_tap=DSP_COMPLEX):
    return n_ch * dsp_per_tap * n_taps


def fir_bram18(n_ch, n_taps):
    """One BRAM18 per tap per I/Q path."""
    return n_ch * 2 * n_taps


def delay_spread_us(max_delay):
    return max_delay * SAMPLE_PERIOD_NS / 1000


def save_fig(fig, name):
    fig.savefig(name)
    print(f"Saved: {name}")
    plt.close(fig)


# ── Plot 1: DSP utilization heatmap (real coefficients, d=2) ─────
def plot_dsp_heatmap():
    taps_range = np.array([4, 8, 16, 32, 64, 128, 256])
    chan_range  = np.arange(1, 13)

    cmap = LinearSegmentedColormap.from_list("dsp", [BLUE, GRAY, RED], N=256)

    util = np.zeros((len(chan_range), len(taps_range)))
    for i, ch in enumerate(chan_range):
        for j, t in enumerate(taps_range):
            util[i, j] = (PLATFORM_DSP + fir_dsps(ch, t, DSP_REAL)) / TOTAL_DSP * 100

    fig, ax = plt.subplots(figsize=(8, 5))
    im = ax.imshow(util, cmap=cmap, aspect="auto", vmin=60, vmax=130, origin="lower")

    # Per-cell percentage labels
    for i in range(len(chan_range)):
        for j in range(len(taps_range)):
            val = util[i, j]
            # White text on dark cells, black on light
            txt_color = "white" if (val > 95 or val < 68) else "black"
            ax.text(j, i, fr"${val:.0f}\%$", ha="center", va="center",
                    fontsize=9.5, color=txt_color)

    # Cell borders via minor ticks + grid
    ax.set_xticks(np.arange(-0.5, len(taps_range), 1), minor=True)
    ax.set_yticks(np.arange(-0.5, len(chan_range),  1), minor=True)
    ax.grid(which="minor", color="white", linewidth=0.8)
    ax.tick_params(which="minor", bottom=False, left=False)

    # 80 % safe-limit contour
    cs = ax.contour(util, levels=[80.0], colors=["white"], linewidths=2,
                    linestyles="--")
    ax.clabel(cs, fmt={80.0: r"$80\,\%$ safe"}, fontsize=11, colors=["white"])

    # Current-config marker (6 channels, 32 taps)
    ci = int(np.where(chan_range == 6)[0][0])
    cj = int(np.where(taps_range == 32)[0][0])
    ax.plot(cj, ci, "ws", markersize=20, markerfacecolor="none", markeredgewidth=2.5)

    ax.set_xticks(range(len(taps_range)))
    ax.set_xticklabels(taps_range)
    ax.set_yticks(range(len(chan_range)))
    ax.set_yticklabels(chan_range)
    ax.set_xlabel(r"Number of FIR taps (real coeff., 2\,DSP/tap)")
    ax.set_ylabel(r"Number of channels")

    cbar = plt.colorbar(im, ax=ax)
    cbar.set_label(r"DSP48E2 utilization (\%)")

    save_fig(fig, "fpga_dsp_heatmap.pdf")


# ── Plot 2: BRAM utilization vs. MAX_DELAY ────────────────────────
def plot_bram_delay():
    max_delay_range = [64, 128, 256, 512, 1024, 2048, 4096]
    taps_list       = [4, 8, 16, 32, 64, 128]
    n_ch = 6

    # Perceptually uniform sequential palette (dark→light, colorblind-safe)
    PALETTE = ["#03045e", "#0077b6", "#00b4d8", "#90e0ef", "#f77f00", "#d62828"]
    MARKERS = ["o", "s", "^", "D", "v", "P"]

    fig, ax = plt.subplots()

    for t, c, m in zip(taps_list, PALETTE, MARKERS):
        util = []
        for d in max_delay_range:
            brams_per_tap = max(1, d // 1024)
            total = PLATFORM_BRAM18 + n_ch * 2 * t * brams_per_tap
            util.append(total / TOTAL_BRAM18 * 100)
        ax.plot(max_delay_range, util, marker=m, linestyle="-", color=c,
                markersize=6, label=f"{t} taps")

    ax.axhline(y=90,  color=GRAY,  linestyle="--", linewidth=1.5,
               label=r"$90\,\%$ safe")
    ax.axhline(y=100, color="black", linestyle="-", linewidth=1, alpha=0.5)

    ax.set_xscale("log", basex=2)
    ax.set_xlabel(r"MAX\_DELAY (samples)")
    ax.set_ylabel(r"BRAM18 utilization (\%)")
    ax.legend(fontsize=10, ncol=2)
    ax.grid()

    # Secondary x-axis: delay spread in microseconds
    ax2 = ax.twiny()
    ax2.set_xscale("log", basex=2)
    ax2.set_xlim(ax.get_xlim())
    ax2.set_xticks(max_delay_range)
    ax2.set_xticklabels(
        [fr"{delay_spread_us(d):.1f}" for d in max_delay_range], fontsize=10)
    ax2.set_xlabel(r"Delay spread ($\mu$s) @ 122.88\,MHz")

    save_fig(fig, "fpga_bram_delay.pdf")


# ── Plot 3: DSP utilization vs. tap count — real vs. complex (6 ch) ─
def plot_real_vs_complex():
    """DSP % vs N_t at N_ch=6; shows where each coefficient type hits the ceiling."""
    taps_range = np.array([4, 8, 16, 32, 64, 128])
    n_ch = 6

    util_real    = [(PLATFORM_DSP + fir_dsps(n_ch, t, DSP_REAL))    / TOTAL_DSP * 100
                    for t in taps_range]
    util_complex = [(PLATFORM_DSP + fir_dsps(n_ch, t, DSP_COMPLEX)) / TOTAL_DSP * 100
                    for t in taps_range]

    fig, ax = plt.subplots()

    ax.plot(range(len(taps_range)), util_real,    "o-", color=BLUE,
            label=r"Real coeff.\ ($d=2$\,DSP/tap)")
    ax.plot(range(len(taps_range)), util_complex, "s-", color=RED,
            label=r"Complex coeff.\ ($d=4$\,DSP/tap)")

    # Safe and hard-limit lines
    ax.axhline(y=SAFE_DSP_FRAC * 100, color=GRAY, linestyle="--", linewidth=1.5,
               label=r"$80\,\%$ safe ceiling")
    ax.axhline(y=100, color="black", linestyle="-", linewidth=1, alpha=0.4)

    # # Mark current config (N_t=32, complex)
    # ci = int(np.where(taps_range == 32)[0][0])
    # ax.annotate(r"current ($N_t=32$)", xy=(ci, util_complex[ci]),
    #             xytext=(ci - 2.2, util_complex[ci] + 4),
    #             fontsize=10, color=RED,
    #             arrowprops=dict(arrowstyle="->", color=RED, lw=1.2))

    ax.set_xticks(range(len(taps_range)))
    ax.set_xticklabels(taps_range)
    ax.set_xlabel(r"Number of FIR taps $N_t$")
    ax.set_ylabel(r"DSP48E2 utilization (\%)")
    ax.legend(fontsize=11)
    ax.grid(axis="y")
    ax.set_ylim(60, 115)

    save_fig(fig, "fpga_real_vs_complex.pdf")


# ── LaTeX table: full resource usage at fixed tap count (real coeff) ─
def print_resource_table(n_taps=32):
    chan_range = [1, 2, 3, 4, 6, 8, 10, 12]

    lines = []
    lines.append(r"\begin{table}[t]")
    lines.append(r"  \centering")
    lines.append(
        r"  \caption{FPGA resource utilization on the xczu28dr at $N_t = "
        + str(n_taps)
        + r"$ taps per channel (real coefficients, $d=2$\,DSP/tap). "
        r"Platform overhead: 2{,}791\,DSP48E2, 210\,BRAM18 (fixed). "
        r"Entries exceeding the $80\,\%$ DSP safe ceiling are marked~$\dagger$.}"
    )
    lines.append(r"  \label{tab:fpga_resources}")
    lines.append(r"  \begin{tabular}{c rr rr}")
    lines.append(r"    \hline")
    lines.append(
        r"    $N_\mathrm{ch}$ & DSP48E2 & DSP\,(\%) & BRAM18 & BRAM\,(\%) \\"
    )
    lines.append(r"    \hline")

    for ch in chan_range:
        dsp  = PLATFORM_DSP    + fir_dsps(ch, n_taps, DSP_REAL)
        bram = PLATFORM_BRAM18 + fir_bram18(ch, n_taps)

        pct_d = dsp  / TOTAL_DSP    * 100
        pct_b = bram / TOTAL_BRAM18 * 100

        flag = r"$^{\dagger}$" if pct_d > SAFE_DSP_FRAC * 100 else ""

        lines.append(
            f"    {ch} "
            f"& {dsp:,} & {pct_d:.1f}\\,\\%{flag} "
            f"& {bram:,} & {pct_b:.1f}\\,\\% \\\\"
        )

    lines.append(r"    \hline")
    lines.append(r"  \end{tabular}")
    lines.append(r"\end{table}")

    table_str = "\n".join(lines)
    print("\n--- LaTeX resource table (N_t={}) ---".format(n_taps))
    print(table_str)
    return table_str


def main():
    plot_dsp_heatmap()
    plot_bram_delay()
    plot_real_vs_complex()
    print_resource_table(n_taps=32)
    print("\nAll figures saved.")


if __name__ == "__main__":
    main()
