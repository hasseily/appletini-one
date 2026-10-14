"""Render measured exclusive CPU-cycle shares from the completed profile analysis."""
from pathlib import Path
import json

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.ticker import PercentFormatter


ROOT = Path(__file__).resolve().parent
SELECTED = [
    "RANDOM PLASMA",
    "FS PLASMA",
    "COLOR PLASMA",
    "TESSELLATOR",
    "SIERPINSKI",
    "ISOMETRIC FACTORY",
    "SPRITE",
]
CATEGORIES = [
    ("Arithmetic", "arithmetic", "#2863A6", "white"),
    ("Variable / array lookup", "variable/array lookup", "#78B9A3", "#16362D"),
    ("Parser / dispatch", "parser/dispatch", "#9784BF", "white"),
    ("Graphics", "graphics", "#EDB15D", "#3D2A11"),
    ("Other", None, "#D8DEE6", "#27384C"),
]


def main():
    data = json.loads((ROOT / "analysis" / "profiles.json").read_text(encoding="utf-8"))
    programs = {row["program"]: row for row in data["programs"]}
    shares = []
    for name in SELECTED:
        program = programs[name]
        cycle_counts = {row["category"]: row["cycles"] for row in program["category_shares"]}
        total = program["instruction_cycles"]
        assert sum(cycle_counts.values()) == total, name
        values = [100 * cycle_counts.get(category, 0) / total for _, category, _, _ in CATEGORIES[:-1]]
        values.append(100 - sum(values))
        assert min(values) >= 0, name
        shares.append(values)

    plt.rcParams.update({
        "font.family": "DejaVu Sans",
        "font.size": 11,
        "svg.fonttype": "none",
        "axes.unicode_minus": False,
    })
    fig, ax = plt.subplots(figsize=(12.8, 7.1), facecolor="white")
    fig.subplots_adjust(left=0.205, right=0.965, bottom=0.185, top=0.715)
    ax.set_facecolor("white")
    fig.text(0.035, 0.93, "Applesoft CPU work by routine", fontsize=22, weight="bold", color="#172A3F")
    fig.text(0.035, 0.882, "GSSquared Enhanced IIe, stock ROM", fontsize=12, color="#536579")

    handles = [Patch(facecolor=color, label=label) for label, _, color, _ in CATEGORIES]
    fig.legend(handles=handles, loc="upper left", bbox_to_anchor=(0.033, 0.831), ncol=5,
               frameon=False, handlelength=1.3, columnspacing=1.7, fontsize=10.5)

    y_positions = list(range(len(SELECTED)))
    left = [0.0] * len(SELECTED)
    for index, (_, _, color, text_color) in enumerate(CATEGORIES):
        widths = [row[index] for row in shares]
        ax.barh(y_positions, widths, left=left, height=0.67, color=color,
                edgecolor="white", linewidth=0.6, zorder=3)
        for y, start, width in zip(y_positions, left, widths):
            if width >= 3.5:
                ax.text(start + width / 2, y, f"{width:.0f}%", ha="center", va="center",
                        color=text_color, fontsize=10.5, weight="bold", zorder=4)
        left = [start + width for start, width in zip(left, widths)]

    ax.set_yticks(y_positions, SELECTED, fontsize=10.5, color="#25394E")
    ax.invert_yaxis()
    ax.set_xlim(0, 100)
    ax.set_xticks([0, 20, 40, 60, 80, 100])
    ax.xaxis.set_major_formatter(PercentFormatter(xmax=100, decimals=0))
    ax.set_xlabel("Share of exclusive CPU instruction cycles", labelpad=11, color="#42566D", fontsize=11)
    ax.tick_params(axis="y", length=0, pad=13)
    ax.tick_params(axis="x", length=0, pad=10, labelcolor="#627286", labelsize=10)
    ax.set_axisbelow(True)
    ax.grid(axis="x", color="#E8ECF1", linewidth=0.8)
    for spine in ax.spines.values():
        spine.set_visible(False)

    fig.text(0.035, 0.065,
             "Each instruction is counted once at its executed address; labels show rounded shares.",
             fontsize=9, color="#64758A")
    fig.text(0.035, 0.037,
             "Other includes number conversion, loop control, strings, monitor and DOS. These are workload shares, not speedup estimates.",
             fontsize=9, color="#64758A")

    for extension in ("png", "svg"):
        output = ROOT / "analysis" / f"workload-shares.{extension}"
        fig.savefig(output, dpi=180, facecolor="white")
        print(output)
    plt.close(fig)


if __name__ == "__main__":
    main()
