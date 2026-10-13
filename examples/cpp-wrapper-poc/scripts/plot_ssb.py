#!/usr/bin/env python3
# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Graphs for SSB run results vs production.

Usage: plot_ssb.py <results-dir> <prod-metrics-dir or empty dir> <out-dir>
"""
import csv
import glob
import json
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

RES, PROD, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(OUT, exist_ok=True)

SIZE = {100000: "100 KB", 2097152: "2 MiB", 100000000: "100 MB", 209715200: "200 MiB"}
C = {"prod": "#5f6368", "native_cpp": "#1a73e8", "rust_wrapped": "#e8710a"}
LBL = {"prod": "Production C++ (SSB, 24 h)", "native_cpp": "Native C++ (POC)",
       "rust_wrapped": "Rust-wrapped C++ (POC)"}
plt.rcParams.update({"figure.dpi": 130, "font.size": 9, "axes.grid": True, "grid.alpha": 0.3,
                     "axes.spines.top": False, "axes.spines.right": False})

# ---- load ---------------------------------------------------------------
lat = defaultdict(list)    # (workload, op, size, impl) -> [ms]
cpu = defaultdict(list)    # (workload, op, size, impl) -> [ns/B]
alloc = defaultdict(list)  # (workload, op, size, impl) -> [all heap bytes / object byte]
newb = defaultdict(list)   # (workload, op, size, impl) -> [operator new bytes / object byte]
proc_rows = {}             # (workload, impl, op) -> row  (RSS_*, WORKLOAD_TOTAL)
for f in glob.glob(os.path.join(RES, "*.csv")):
  for r in csv.DictReader(open(f)):
    if r["iteration"] == "-1":
      proc_rows[(r["workload"], r["impl"], r["op"])] = r
      continue
    if r["ok"] != "1":
      continue
    size = int(r["object_size"])
    k = (r["workload"], r["op"], size, r["impl"])
    lat[k].append(float(r["latency_ms"]))
    cpu[k].append(float(r["cpu_ms"]) * 1e6 / size)
    if r.get("alloc_bytes"):
      alloc[k].append(int(r["alloc_bytes"]) / size)
      newb[k].append(int(r["new_bytes"]) / size)

prod_lat, prod_cpu, prod_mem = {}, {}, {}
for f in glob.glob(os.path.join(PROD, "*.json")):
  w, m = os.path.basename(f)[:-5].split("-", 1)
  d = json.load(open(f))
  for r in d["rows"]:
    k = (w, r["ssb_op"], int(r["ssb_object_size"]))
    if m == "latency":
      prod_lat[k] = {p: r[p] * 1000 for p in ("p50", "p90", "p99")}
    elif m == "cpu":
      prod_cpu[k] = {p: r[p] for p in ("p50", "p90", "p99")}
    elif m == "memory":
      prod_mem[k] = {p: r[p] for p in ("p50", "p90", "p99")}

# The production-metrics directory is optional. Without it, graphs compare
# native C++ and Rust-wrapped only (headline is normalised to native C++).
HAVE_PROD = bool(prod_lat or prod_cpu or prod_mem)
IMPLS = (["prod"] if HAVE_PROD else []) + ["native_cpp", "rust_wrapped"]


def off(j, n, width):
  return (j - (n - 1) / 2) * width


def pct(v, p):
  return float(np.percentile(v, p)) if v else np.nan


def stat(k, impl, p):
  if impl == "prod":
    return prod_lat.get(k, {}).get(f"p{p}", np.nan)
  return pct(lat.get(k + (impl,), []), p)


def sizes_of(w, op):
  return sorted({k[2] for k in lat if k[0] == w and k[1] == op})


saved = []


def save(fig, name):
  p = os.path.join(OUT, name)
  fig.savefig(p, bbox_inches="tight")
  plt.close(fig)
  saved.append(p)


# ---- 1. grouped bars p50 with p90..p99 whiskers, one panel per op ---------
def bars(w, ops, title, fname, logy=False):
  fig, axes = plt.subplots(1, len(ops), figsize=(4.2 * len(ops), 3.6), squeeze=False)
  impls = IMPLS
  for ax, op in zip(axes[0], ops):
    sz = sizes_of(w, op)
    x = np.arange(len(sz))
    width = 0.8 / len(impls)
    for j, impl in enumerate(impls):
      p50 = [stat((w, op, s), impl, 50) for s in sz]
      p99 = [stat((w, op, s), impl, 99) for s in sz]
      err = [np.zeros(len(sz)), np.nan_to_num(np.array(p99) - np.array(p50))]
      b = ax.bar(x + off(j, len(impls), width), p50, width, color=C[impl], label=LBL[impl],
                 yerr=err, capsize=2, error_kw={"elinewidth": 0.8, "alpha": 0.6})
      for rect, v in zip(b, p50):
        if v == v:
          ax.annotate(f"{v:.0f}" if v >= 10 else f"{v:.1f}", (rect.get_x() + rect.get_width() / 2, v),
                      ha="center", va="bottom", fontsize=6.5, xytext=(0, 1), textcoords="offset points")
    ax.set_xticks(x, [SIZE.get(s, s) for s in sz])
    ax.set_title(op)
    ax.set_ylabel("latency ms (bar = p50, whisker = p99)")
    if logy:
      ax.set_yscale("log")
  axes[0][0].legend(loc="upper left", fontsize=7)
  fig.suptitle(title, fontweight="bold")
  fig.tight_layout()
  save(fig, fname)


bars("open", ["open", "APPENDABLE"], "open: Open() latency and appendable upload (RAPID, gRPC)", "01_open.png")
bars("random8k", ["random8k", "APPENDABLE"], "random8k: Open + 8 KiB read, and appendable upload (RAPID, gRPC)", "02_random8k.png")
bars("read50x2x4", ["read50x2x4", "APPENDABLE"], "read50x2x4: 50 x 2 MiB concurrent reads, 200 MiB upload (RAPID, gRPC)", "03_read50x2x4.png")
bars("w1r3", ["READ[0]", "READ[1]", "READ[2]"], "w1r3 reads (STANDARD, JSON, sync)", "04_w1r3_reads.png", logy=True)
bars("w1r3", ["SINGLE-SHOT", "RESUMABLE", "DELETE"], "w1r3 uploads and delete (STANDARD, JSON, sync)", "05_w1r3_uploads_delete.png", logy=True)
bars("w1r3", ["W1R3D1-SINGLE-SHOT", "W1R3D1-RESUMABLE"], "w1r3 whole iteration: write + 3 reads + delete", "06_w1r3_total.png", logy=True)


# ---- 2. latency CDFs (POC raw samples) with prod p50/p99 markers ----------
def cdfs(items, fname, title):
  n = len(items)
  cols = 3
  rows = (n + cols - 1) // cols
  fig, axes = plt.subplots(rows, cols, figsize=(4.3 * cols, 3.1 * rows), squeeze=False)
  for ax, (w, op, s) in zip(axes.flat, items):
    for impl in ("native_cpp", "rust_wrapped"):
      v = sorted(lat.get((w, op, s, impl), []))
      if v:
        ax.plot(v, np.linspace(0, 1, len(v)), color=C[impl], label=LBL[impl], lw=1.6)
    pr = prod_lat.get((w, op, s))
    if pr:
      for p, ls in (("p50", "--"), ("p99", ":")):
        ax.axvline(pr[p], color=C["prod"], ls=ls, lw=1, label=f"prod {p}")
    ax.set_title(f"{w} · {op} · {SIZE.get(s, s)}", fontsize=8.5)
    ax.set_xlabel("latency ms (x-axis clipped at 1.25 × max p99)")
    ax.set_ylabel("CDF")
    p99s = [pct(lat.get((w, op, s, i), []), 99) for i in ("native_cpp", "rust_wrapped")]
    if pr:
      p99s.append(pr["p99"])
    ax.set_xlim(0, 1.25 * np.nanmax(p99s))
  for ax in list(axes.flat)[n:]:
    ax.axis("off")
  axes[0][0].legend(fontsize=6.5, loc="lower right")
  fig.suptitle(title, fontweight="bold")
  fig.tight_layout()
  save(fig, fname)


cdfs([("open", "open", 100000), ("open", "open", 100000000), ("random8k", "random8k", 2097152),
      ("random8k", "random8k", 100000000), ("read50x2x4", "read50x2x4", 209715200),
      ("read50x2x4", "APPENDABLE", 209715200)],
     "07_cdf_async.png", "Latency distribution: async workloads (RAPID, gRPC)")
cdfs([("w1r3", "READ[1]", 100000), ("w1r3", "READ[1]", 2097152), ("w1r3", "READ[1]", 100000000),
      ("w1r3", "SINGLE-SHOT", 100000000), ("w1r3", "RESUMABLE", 100000000),
      ("w1r3", "W1R3D1-SINGLE-SHOT", 100000000)],
     "08_cdf_w1r3.png", "Latency distribution: w1r3 (STANDARD, JSON, sync)")

# ---- 3. ratio overview: wrapped/native and native/prod, all rows ----------
rows = []
order = [("open", "open"), ("open", "APPENDABLE"), ("random8k", "random8k"), ("random8k", "APPENDABLE"),
         ("read50x2x4", "read50x2x4"), ("read50x2x4", "APPENDABLE"), ("w1r3", "SINGLE-SHOT"),
         ("w1r3", "RESUMABLE"), ("w1r3", "READ[0]"), ("w1r3", "READ[1]"), ("w1r3", "READ[2]"),
         ("w1r3", "DELETE"), ("w1r3", "W1R3D1-SINGLE-SHOT"), ("w1r3", "W1R3D1-RESUMABLE")]
for w, op in order:
  for s in sizes_of(w, op):
    n50, r50, p50 = stat((w, op, s), "native_cpp", 50), stat((w, op, s), "rust_wrapped", 50), stat((w, op, s), "prod", 50)
    rows.append((f"{w} · {op} · {SIZE.get(s, s)}", r50 / n50, n50 / p50 if p50 == p50 else np.nan, w))
fig, ax = plt.subplots(figsize=(9, 0.26 * len(rows) + 1.2))
y = np.arange(len(rows))[::-1]
ax.barh(y + 0.2, [r[1] for r in rows], 0.38, color=C["rust_wrapped"], label="Rust-wrapped ÷ native C++ (p50)")
if HAVE_PROD:
  ax.barh(y - 0.2, [r[2] for r in rows], 0.38, color=C["native_cpp"], label="native C++ (POC) ÷ production (p50)")
for yi, r in zip(y, rows):
  ax.text(r[1] + 0.02, yi + 0.2, f"{r[1]:.2f}×", va="center", fontsize=6.5)
  if r[2] == r[2]:
    ax.text(r[2] + 0.02, yi - 0.2, f"{r[2]:.2f}×", va="center", fontsize=6.5, color="#174ea6")
ax.axvline(1.0, color="black", lw=1)
ax.set_yticks(y, [r[0] for r in rows], fontsize=7)
ax.set_xlabel("ratio (1.0 = same; > 1 = slower)")
ax.set_title("p50 latency ratios, every workload / op / size", fontweight="bold")
ax.legend(loc="lower right", fontsize=7)
fig.tight_layout()
save(fig, "09_ratios.png")

# ---- 4. CPU per byte (SSB definition: process CPU ns / object size) -------
cpu_items = [("open", "open"), ("random8k", "random8k"), ("read50x2x4", "read50x2x4"),
             ("read50x2x4", "APPENDABLE"), ("w1r3", "READ[1]"), ("w1r3", "SINGLE-SHOT")]
fig, axes = plt.subplots(1, len(cpu_items), figsize=(3.1 * len(cpu_items), 3.4), squeeze=False)
for ax, (w, op) in zip(axes[0], cpu_items):
  sz = sizes_of(w, op)
  x = np.arange(len(sz))
  allv = []
  for j, impl in enumerate(IMPLS):
    vals = [prod_cpu.get((w, op, s), {}).get("p50", np.nan) if impl == "prod"
            else pct(cpu.get((w, op, s, impl), []), 50) for s in sz]
    allv += [v for v in vals if v == v and v > 0]
    bw = 0.8 / len(IMPLS)
    b = ax.bar(x + off(j, len(IMPLS), bw), vals, bw, color=C[impl], label=LBL[impl])
    for rect, v in zip(b, vals):
      if v == v:
        ax.annotate(f"{v:.2g}", (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom",
                    fontsize=6, xytext=(0, 1), textcoords="offset points")
  ax.set_xticks(x, [SIZE.get(s, s) for s in sz], fontsize=7)
  wide = allv and max(allv) / min(allv) > 20
  if wide:
    ax.set_yscale("log")
  ax.set_title(f"{w} · {op}", fontsize=8.5)
  ax.set_ylabel("CPU ns / byte (p50" + (", log)" if wide else ")"))
axes[0][0].legend(fontsize=6.5)
fig.suptitle("CPU cost per byte (SSB metric: process CPU ÷ object size)", fontweight="bold")
fig.tight_layout()
save(fig, "10_cpu_per_byte.png")

# ---- 5. memory: heap allocation volume per object byte ----------------------
# SSB's memory metric = operator-new bytes / object size ("B/B"). We show:
#   prod (operator new), native operator new (SSB-equivalent), and the fair
#   all-allocator number (malloc interposition) for native and Rust-wrapped.
if alloc:
  mem_items = [("open", "open"), ("open", "APPENDABLE"), ("random8k", "random8k"),
               ("read50x2x4", "read50x2x4"), ("read50x2x4", "APPENDABLE"),
               ("w1r3", "READ[1]"), ("w1r3", "SINGLE-SHOT"), ("w1r3", "RESUMABLE")]
  series = [("prod", "Production C++: operator new (SSB metric)", C["prod"], None),
            ("native_new", "Native C++: operator new (SSB-equivalent)", "#8ab4f8", "/"),
            ("native_all", "Native C++: all heap allocations", C["native_cpp"], None),
            ("wrapped_all", "Rust-wrapped: all heap allocations", C["rust_wrapped"], None)]
  if not HAVE_PROD:
    series = series[1:]
  cols = 4
  rws = (len(mem_items) + cols - 1) // cols
  fig, axes = plt.subplots(rws, cols, figsize=(4.4 * cols, 3.3 * rws), squeeze=False)
  for ax, (w, op) in zip(axes.flat, mem_items):
    sz = sizes_of(w, op)
    x = np.arange(len(sz))
    allv = []
    for j, (key, label, color, hatch) in enumerate(series):
      if key == "prod":
        vals = [prod_mem.get((w, op, s), {}).get("p50", np.nan) for s in sz]
      elif key == "native_new":
        vals = [pct(newb.get((w, op, s, "native_cpp"), []), 50) for s in sz]
      elif key == "native_all":
        vals = [pct(alloc.get((w, op, s, "native_cpp"), []), 50) for s in sz]
      else:
        vals = [pct(alloc.get((w, op, s, "rust_wrapped"), []), 50) for s in sz]
      allv += [v for v in vals if v == v and v > 0]
      b = ax.bar(x + off(j, len(series), 0.2), vals, 0.2, color=color, label=label, hatch=hatch,
                 edgecolor="white" if hatch else None)
      for rect, v in zip(b, vals):
        if v == v:
          ax.annotate(f"{v:.2g}", (rect.get_x() + rect.get_width() / 2, v), ha="center",
                      va="bottom", fontsize=5.5, xytext=(0, 1), textcoords="offset points")
    ax.set_xticks(x, [SIZE.get(s, s) for s in sz], fontsize=7)
    if allv and max(allv) / min(allv) > 20:
      ax.set_yscale("log")
    ax.set_title(f"{w} · {op}", fontsize=8.5)
    ax.set_ylabel("heap bytes allocated / object byte (p50)", fontsize=7)
  for ax in list(axes.flat)[len(mem_items):]:
    ax.axis("off")
  axes[0][0].legend(fontsize=6, loc="upper right")
  fig.suptitle("Memory: heap allocation volume per op (SSB 'memory' metric = bytes allocated ÷ object size)",
               fontweight="bold")
  fig.tight_layout()
  save(fig, "11_memory_alloc_per_byte.png")

# ---- 6. memory: process RSS per workload run --------------------------------
if proc_rows:
  wls = [w for w in ["w1r3", "open", "random8k", "read50x2x4"]
         if (w, "native_cpp", "RSS_PEAK") in proc_rows]
  fig, axes = plt.subplots(1, 2, figsize=(12, 3.8))
  x = np.arange(len(wls))
  for j, impl in enumerate(["native_cpp", "rust_wrapped"]):
    peak = [int(proc_rows[(w, impl, "RSS_PEAK")]["rss_kib"]) / 1024 for w in wls]
    start = [int(proc_rows[(w, impl, "RSS_START")]["rss_kib"]) / 1024 for w in wls]
    end = [int(proc_rows[(w, impl, "RSS_END")]["rss_kib"]) / 1024 for w in wls]
    b = axes[0].bar(x + (j - 0.5) * 0.38, peak, 0.38, color=C[impl], label=f"{LBL[impl]}: peak RSS")
    axes[0].scatter(x + (j - 0.5) * 0.38, start, color="black", marker="_", s=200, zorder=3,
                    label="RSS at start (incl. 200 MiB payload buffer)" if j == 0 else None)
    for rect, v in zip(b, peak):
      axes[0].annotate(f"{v:.0f}", (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom",
                       fontsize=7, xytext=(0, 1), textcoords="offset points")
    growth = np.array(peak) - np.array(start)
    b2 = axes[1].bar(x + (j - 0.5) * 0.38, growth, 0.38, color=C[impl], label=LBL[impl])
    for rect, v in zip(b2, growth):
      axes[1].annotate(f"{v:.0f}", (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom",
                       fontsize=7, xytext=(0, 1), textcoords="offset points")
  for ax, t, yl in ((axes[0], "Peak RSS of the benchmark process", "MiB"),
                    (axes[1], "RSS growth during the workload (peak − start) = client overhead", "MiB")):
    ax.set_xticks(x, wls)
    ax.set_title(t, fontsize=9)
    ax.set_ylabel(yl)
    ax.legend(fontsize=6.5)
  fig.suptitle("Memory: resident set size (one process per workload, 200 iterations)", fontweight="bold")
  fig.tight_layout()
  save(fig, "12_memory_rss.png")

  # ---- 7. CPU + allocation totals per workload ------------------------------
  fig, axes = plt.subplots(1, 2, figsize=(12, 3.6))
  for j, impl in enumerate(["native_cpp", "rust_wrapped"]):
    cpu_s = [float(proc_rows[(w, impl, "WORKLOAD_TOTAL")]["cpu_ms"]) / 1000 for w in wls]
    alloc_gib = [int(proc_rows[(w, impl, "WORKLOAD_TOTAL")]["alloc_bytes"]) / 2**30 for w in wls]
    for ax, vals, fmt in ((axes[0], cpu_s, "{:.1f}"), (axes[1], alloc_gib, "{:.1f}")):
      b = ax.bar(x + (j - 0.5) * 0.38, vals, 0.38, color=C[impl], label=LBL[impl])
      for rect, v in zip(b, vals):
        ax.annotate(fmt.format(v), (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom",
                    fontsize=7, xytext=(0, 1), textcoords="offset points")
  axes[0].set_title("Total process CPU time for 200 iterations (user + sys, all threads)", fontsize=9)
  axes[0].set_ylabel("CPU seconds")
  axes[1].set_title("Total heap bytes allocated for 200 iterations (all allocators)", fontsize=9)
  axes[1].set_ylabel("GiB allocated")
  for ax in axes:
    ax.set_xticks(x, wls)
    ax.legend(fontsize=7)
  fig.suptitle("CPU and allocation totals per workload", fontweight="bold")
  fig.tight_layout()
  save(fig, "13_cpu_alloc_totals.png")

# ---- 8. headline summary ----------------------------------------------------
head = [("open", "open", 2097152, "open\n(Open)"), ("random8k", "random8k", 2097152, "random8k\n(Open+8KiB)"),
        ("read50x2x4", "read50x2x4", 209715200, "read50x2x4\n(50x2MiB)"),
        ("read50x2x4", "APPENDABLE", 209715200, "upload\n200 MiB"),
        ("w1r3", "READ[1]", 100000000, "w1r3 READ\n100 MB (JSON)"),
        ("w1r3", "SINGLE-SHOT", 100000000, "w1r3 upload\n100 MB (JSON)"),
        ("w1r3", "W1R3D1-SINGLE-SHOT", 100000000, "w1r3 total\n100 MB (JSON)")]
fig, ax = plt.subplots(figsize=(11, 3.8))
x = np.arange(len(head))
BASE = "prod" if HAVE_PROD else "native_cpp"
for j, impl in enumerate(IMPLS):
  vals = [stat((w, op, s), impl, 50) for w, op, s, _ in head]
  # normalise to the baseline p50 so very different magnitudes fit on one axis
  base = [stat((w, op, s), BASE, 50) for w, op, s, _ in head]
  rel = np.array(vals) / np.array(base)
  bw = 0.8 / len(IMPLS)
  b = ax.bar(x + off(j, len(IMPLS), bw), rel, bw, color=C[impl], label=LBL[impl])
  for rect, v, abs_v in zip(b, rel, vals):
    ax.annotate(f"{abs_v:.0f} ms", (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom",
                fontsize=6.5, rotation=90, xytext=(0, 2), textcoords="offset points")
ax.axhline(1.0, color="black", lw=0.8)
ax.set_xticks(x, [h[3] for h in head], fontsize=8)
BASE_LBL = "production" if HAVE_PROD else "native C++ (POC)"
ax.set_ylabel(f"p50 latency relative to {BASE_LBL} (1.0 = baseline)")
ax.set_ylim(0, 2.9)
ax.set_title(f"Headline: p50 latency vs {BASE_LBL} (labels = absolute p50)", fontweight="bold")
ax.legend(fontsize=7, loc="upper right")
fig.tight_layout()
save(fig, "00_headline.png")

print("\n".join(saved))
