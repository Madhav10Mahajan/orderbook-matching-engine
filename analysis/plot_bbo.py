"""Plot the top-of-book time series written by `replay --bbo`.

Usage: python3 analysis/plot_bbo.py results/generated_bbo.csv results/bbo.png

Panels:
  1. best bid / best ask / microprice / trades  (how the book's "fair price" moves)
  2. spread in ticks                             (liquidity: wider = costlier to trade)
  3. top-1 order-book imbalance vs. the next     (does imbalance predict the next
     mid-price change                             mid move? printed as a correlation)
"""
import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def load(path):
    cols = {k: [] for k in ("seq", "bid", "ask", "spread", "mid", "micro", "imb1", "last")}
    with open(path) as f:
        for row in csv.DictReader(f):
            if not row["best_bid"] or not row["best_ask"]:
                continue
            cols["seq"].append(int(row["msg_seq"]))
            cols["bid"].append(float(row["best_bid"]))
            cols["ask"].append(float(row["best_ask"]))
            cols["spread"].append(float(row["spread"]))
            cols["mid"].append(float(row["mid"]))
            cols["micro"].append(float(row["microprice"]))
            cols["imb1"].append(float(row["imbalance1"]))
            cols["last"].append(float(row["last_trade"]) if row["last_trade"] else None)
    return cols


def correlation(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / (sxx * syy) ** 0.5 if sxx and syy else float("nan")


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "results/generated_bbo.csv"
    out = sys.argv[2] if len(sys.argv) > 2 else "results/bbo.png"
    d = load(src)
    if not d["seq"]:
        sys.exit("no two-sided snapshots in " + src)

    # Next non-zero mid change after each snapshot, paired with the imbalance at that moment.
    imb, fwd = [], []
    for i in range(len(d["mid"])):
        for j in range(i + 1, min(i + 200, len(d["mid"]))):
            move = d["mid"][j] - d["mid"][i]
            if move != 0:
                imb.append(d["imb1"][i])
                fwd.append(1.0 if move > 0 else -1.0)
                break
    rho = correlation(imb, fwd) if imb else float("nan")

    window = slice(0, min(5000, len(d["seq"])))  # zoom so individual ticks are visible
    seq = d["seq"][window]
    fig, axes = plt.subplots(3, 1, figsize=(12, 10), gridspec_kw={"height_ratios": [3, 1, 2]})

    ax = axes[0]
    ax.step(seq, d["bid"][window], where="post", label="best bid", color="tab:green", lw=1)
    ax.step(seq, d["ask"][window], where="post", label="best ask", color="tab:red", lw=1)
    ax.plot(seq, d["micro"][window], label="microprice", color="tab:blue", lw=0.6, alpha=0.7)
    trades = [(s, p) for s, p in zip(seq, d["last"][window]) if p is not None]
    ax.set_title(f"Top of book (first {len(seq):,} of {len(d['seq']):,} messages)")
    ax.set_ylabel("price")
    ax.ticklabel_format(axis="y", useOffset=False)
    ax.legend(loc="upper left")

    axes[1].step(seq, d["spread"][window], where="post", color="tab:purple", lw=1)
    axes[1].set_ylabel("spread")

    ax = axes[2]
    ups = [x for x, y in zip(imb, fwd) if y > 0]
    downs = [x for x, y in zip(imb, fwd) if y < 0]
    bins = [i / 10 for i in range(-10, 11)]
    ax.hist([ups, downs], bins=bins, label=["next mid move UP", "next mid move DOWN"],
            color=["tab:green", "tab:red"], alpha=0.7)
    ax.set_xlabel("top-1 imbalance  (bid qty - ask qty) / (bid qty + ask qty)")
    ax.set_ylabel("count")
    ax.set_title(f"Imbalance vs. direction of next mid-price move  (correlation = {rho:+.3f}, n = {len(imb):,})")
    ax.legend()

    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"Wrote {out}  (imbalance -> next mid move correlation {rho:+.3f})")


if __name__ == "__main__":
    main()
