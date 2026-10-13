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
"""Tables + graphs for the w1r3 (JSON) run: native C++ vs Rust-wrapped vs pure Rust.

Usage: analyze_w1r3.py <results-dir> <out-dir> [prod-metrics-dir]
  results-dir : <impl>-w1r3-<size>.csv files and optional build-metrics.txt
  prod-dir    : optional cpp-/rust-{latency,cpu,memory}.json from ssb_prod_metrics.py
Prints Markdown tables to stdout and writes PNGs to out-dir.
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

RES, OUT = sys.argv[1], sys.argv[2]
PROD = sys.argv[3] if len(sys.argv) > 3 else ""
os.makedirs(OUT, exist_ok=True)
IMPLS = ["native_cpp", "rust_wrapped", "rust_pure"]
LBL = {"native_cpp": "Native C++", "rust_wrapped": "Rust-wrapped C++", "rust_pure": "Pure Rust",
       "prod_cpp": "SSB prod C++", "prod_rust": "SSB prod Rust"}
COL = {"native_cpp": "#1a73e8", "rust_wrapped": "#e8710a", "rust_pure": "#34a853",
       "prod_cpp": "#5f6368", "prod_rust": "#a142f4"}
SIZES = [100000, 2097152, 100000000]
SZ = {100000: "100 KB", 2097152: "2 MiB", 100000000: "100 MB"}
plt.rcParams.update({"figure.dpi": 130, "font.size": 9, "axes.grid": True, "grid.alpha": 0.3,
                     "axes.spines.top": False, "axes.spines.right": False})

lat, cpu, alloc = defaultdict(list), defaultdict(list), defaultdict(list)
proc = {}
errors = defaultdict(int)
for f in glob.glob(os.path.join(RES, "*-w1r3-*.csv")):
  base = os.path.basename(f)[:-4]
  impl, size = base.split("-w1r3-")[0], int(base.split("-w1r3-")[1])
  for r in csv.DictReader(open(f)):
    if r["iteration"] == "-1":
      proc[(impl, size, r["op"])] = r
      continue
    if r["ok"] != "1":
      errors[(impl, size, r["op"])] += 1
      continue
    op = r["op"].replace("W1R3D1-", "TOTAL-")
    k = (impl, size, op)
    lat[k].append(float(r["latency_ms"]))
    cpu[k].append(float(r["cpu_ms"]))
    alloc[k].append(int(r["alloc_bytes"] or 0))
    if op.startswith("READ["):  # pooled reads
      kk = (impl, size, "READ")
      lat[kk].append(float(r["latency_ms"]))
      cpu[kk].append(float(r["cpu_ms"]))
      alloc[kk].append(int(r["alloc_bytes"] or 0))
    if op in ("SINGLE-SHOT", "RESUMABLE"):  # pooled uploads
      kk = (impl, size, "UPLOAD")
      lat[kk].append(float(r["latency_ms"]))
      cpu[kk].append(float(r["cpu_ms"]))
      alloc[kk].append(int(r["alloc_bytes"] or 0))


def P(v, p):
  return float(np.percentile(v, p)) if v else float("nan")


prod = {}
if PROD:
  for lang in ("cpp", "rust"):
    for m in ("latency", "cpu", "memory"):
      p = os.path.join(PROD, f"{lang}-{m}.json")
      if os.path.exists(p):
        for r in json.load(open(p))["rows"]:
          prod[(lang, m, r["ssb_op"], int(r["ssb_object_size"]))] = r


def fmt(v, d=1):
  return "–" if v != v else f"{v:,.{d}f}"


# ---------------------------------------------------------------- tables
OPS = ["SINGLE-SHOT", "RESUMABLE", "READ[0]", "READ[1]", "READ[2]", "DELETE", "TOTAL-SINGLE-SHOT", "TOTAL-RESUMABLE"]
print("## Latency (ms)\n")
print("| size | op | " + " | ".join(f"{LBL[i]} p50 / p99" for i in IMPLS) + " | wrapped ÷ native (p50) | pure ÷ native (p50) | n |")
print("|---|---|" + "--:|" * (len(IMPLS) + 3))
for s in SIZES:
  for op in OPS:
    cells = [f"{fmt(P(lat[(i, s, op)], 50))} / {fmt(P(lat[(i, s, op)], 99))}" for i in IMPLS]
    n = P(lat[("native_cpp", s, op)], 50)
    w = P(lat[("rust_wrapped", s, op)], 50)
    pr = P(lat[("rust_pure", s, op)], 50)
    cnt = "/".join(str(len(lat[(i, s, op)])) for i in IMPLS)
    print(f"| {SZ[s]} | {op} | " + " | ".join(cells) + f" | {fmt(w / n, 2)}× | {fmt(pr / n, 2)}× | {cnt} |")

print("\n## CPU per op (ms, p50, process CPU all threads)\n")
print("| size | op | " + " | ".join(LBL[i] for i in IMPLS) + " |")
print("|---|---|" + "--:|" * len(IMPLS))
for s in SIZES:
  for op in ["SINGLE-SHOT", "RESUMABLE", "READ", "DELETE", "TOTAL-SINGLE-SHOT"]:
    print(f"| {SZ[s]} | {op} | " + " | ".join(fmt(P(cpu[(i, s, op)], 50), 2) for i in IMPLS) + " |")

print("\n## Heap bytes allocated per object byte (B/B, p50, every malloc in the process)\n")
print("| size | op | " + " | ".join(LBL[i] for i in IMPLS) + " |")
print("|---|---|" + "--:|" * len(IMPLS))
for s in SIZES:
  for op in ["SINGLE-SHOT", "RESUMABLE", "READ", "DELETE"]:
    print(f"| {SZ[s]} | {op} | " + " | ".join(fmt(P(alloc[(i, s, op)], 50) / s, 3) for i in IMPLS) + " |")

print("\n## Whole run per size (200 iterations): wall, CPU, total allocated, RSS\n")
print("| size | impl | wall s | CPU s | allocated GiB | RSS start MiB | RSS peak MiB | RSS end MiB | peak − start MiB | errors |")
print("|---|---|--:|--:|--:|--:|--:|--:|--:|--:|")
for s in SIZES:
  for i in IMPLS:
    t, a, b, e = (proc.get((i, s, k)) for k in ("WORKLOAD_TOTAL", "RSS_START", "RSS_PEAK", "RSS_END"))
    if not t:
      continue
    err = sum(v for (ii, ss, _), v in errors.items() if ii == i and ss == s)
    rs, rp, re_ = (int(x["rss_kib"]) / 1024 for x in (a, b, e))
    print(f"| {SZ[s]} | {LBL[i]} | {float(t['latency_ms']) / 1000:.1f} | {float(t['cpu_ms']) / 1000:.1f} | "
          f"{int(t['alloc_bytes']) / 2**30:.2f} | {rs:.0f} | {rp:.0f} | {re_:.0f} | {rp - rs:.0f} | {err} |")

if prod:
  print("\n## SSB production (24 h, release, JSON, STANDARD, us-central1) vs this POC (us-west4) — latency p50 ms\n")
  print("| size | op | SSB prod C++ | SSB prod Rust | POC native C++ | POC Rust-wrapped | POC pure Rust |")
  print("|---|---|--:|--:|--:|--:|--:|")
  for s in SIZES:
    for op in ["SINGLE-SHOT", "RESUMABLE", "READ[1]", "DELETE"]:
      pc = prod.get(("cpp", "latency", op, s), {}).get("p50", float("nan")) * 1000
      pr = prod.get(("rust", "latency", op, s), {}).get("p50", float("nan")) * 1000
      print(f"| {SZ[s]} | {op} | {fmt(pc)} | {fmt(pr)} | " + " | ".join(fmt(P(lat[(i, s, op)], 50)) for i in IMPLS) + " |")

saved = []


def save(fig, name):
  fig.tight_layout()
  p = os.path.join(OUT, name)
  fig.savefig(p, bbox_inches="tight")
  plt.close(fig)
  saved.append(p)


def bars(ax, groups, getter, impls, fmtv="{:.0f}", logy=False):
  x = np.arange(len(groups))
  w = 0.8 / len(impls)
  for j, i in enumerate(impls):
    vals = [getter(i, g) for g in groups]
    b = ax.bar(x + (j - (len(impls) - 1) / 2) * w, vals, w, color=COL[i], label=LBL[i])
    for rect, v in zip(b, vals):
      if v == v:
        ax.annotate(fmtv.format(v), (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom",
                    fontsize=6.5, xytext=(0, 1), textcoords="offset points")
  ax.set_xticks(x, [SZ.get(g, g) if not isinstance(g, tuple) else g[1] for g in groups])
  if logy:
    ax.set_yscale("log")


# 1. latency p50 (bar) + p99 (whisker) per op, one panel per op, x = size
def lat_panel(ops, fname, title):
  fig, axes = plt.subplots(1, len(ops), figsize=(4.3 * len(ops), 3.6), squeeze=False)
  for ax, op in zip(axes[0], ops):
    x = np.arange(len(SIZES))
    w = 0.8 / len(IMPLS)
    for j, i in enumerate(IMPLS):
      p50 = [P(lat[(i, s, op)], 50) for s in SIZES]
      p99 = [P(lat[(i, s, op)], 99) for s in SIZES]
      b = ax.bar(x + (j - 1) * w, p50, w, color=COL[i], label=LBL[i],
                 yerr=[np.zeros(3), np.nan_to_num(np.array(p99) - np.array(p50))], capsize=2,
                 error_kw={"elinewidth": 0.8, "alpha": 0.6})
      for rect, v in zip(b, p50):
        if v == v:
          ax.annotate(f"{v:.0f}", (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom",
                      fontsize=6.5, xytext=(0, 1), textcoords="offset points")
    ax.set_xticks(x, [SZ[s] for s in SIZES])
    ax.set_yscale("log")
    ax.set_title(op.replace("TOTAL-", "whole iteration, "))
    ax.set_ylabel("ms (bar = p50, whisker = p99, log)")
  axes[0][0].legend(fontsize=7, loc="upper left")
  fig.suptitle(title, fontweight="bold")
  save(fig, fname)


lat_panel(["SINGLE-SHOT", "RESUMABLE", "READ", "DELETE"], "w1_latency_ops.png", "w1r3 latency by operation (JSON, 200 iterations per size)")
lat_panel(["TOTAL-SINGLE-SHOT", "TOTAL-RESUMABLE"], "w2_latency_total.png",
          "w1r3 whole iteration (write + 3 reads + delete)")

# 2. ratio vs native
fig, ax = plt.subplots(figsize=(12, 3.6))
groups = [(s, op) for s in SIZES for op in ["SINGLE-SHOT", "RESUMABLE", "READ", "DELETE", "TOTAL-SINGLE-SHOT"]]
x = np.arange(len(groups))
for j, i in enumerate(["rust_wrapped", "rust_pure"]):
  vals = [P(lat[(i, s, op)], 50) / P(lat[("native_cpp", s, op)], 50) for s, op in groups]
  b = ax.bar(x + (j - 0.5) * 0.38, vals, 0.38, color=COL[i], label=f"{LBL[i]} ÷ native C++")
  for rect, v in zip(b, vals):
    ax.annotate(f"{v:.2f}", (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom", fontsize=6.5,
                xytext=(0, 1), textcoords="offset points")
ax.axhline(1, color="black", lw=0.8)
ax.set_xticks(x, [f"{SZ[s]}\n{op.replace('TOTAL-SINGLE-SHOT', 'total').replace('SINGLE-SHOT', 'single').replace('RESUMABLE', 'resum.')}" for s, op in groups], fontsize=6.5)
ax.set_ylabel("p50 latency ratio (< 1 = faster than native)")
ax.set_title("Latency relative to native C++ (p50)", fontweight="bold")
ax.legend(fontsize=7)
save(fig, "w3_latency_ratio.png")

# 3. CDFs for 100 MB
fig, axes = plt.subplots(1, 3, figsize=(13, 3.4))
for ax, op in zip(axes, ["SINGLE-SHOT", "READ", "TOTAL-SINGLE-SHOT"]):
  for i in IMPLS:
    v = sorted(lat[(i, 100000000, op)])
    if v:
      ax.plot(v, np.linspace(0, 1, len(v)), color=COL[i], label=LBL[i], lw=1.6)
  ax.set_title(f"100 MB · {op.replace('TOTAL-SINGLE-SHOT', 'whole iteration')}")
  ax.set_xlabel("latency ms")
  ax.set_ylabel("fraction of requests")
axes[0].legend(fontsize=7, loc="lower right")
fig.suptitle("Latency distribution at 100 MB", fontweight="bold")
save(fig, "w4_cdf_100mb.png")

# 4. CPU per op
fig, axes = plt.subplots(1, 3, figsize=(13, 3.5))
for ax, op in zip(axes, ["SINGLE-SHOT", "READ", "DELETE"]):
  bars(ax, SIZES, lambda i, s: P(cpu[(i, s, op)], 50), IMPLS, "{:.1f}", logy=True)
  ax.set_title(op)
  ax.set_ylabel("CPU ms per op (p50, log)")
axes[0].legend(fontsize=7)
fig.suptitle("CPU per operation (process CPU, all threads)", fontweight="bold")
save(fig, "w5_cpu_per_op.png")

# 5. allocation per byte
fig, axes = plt.subplots(1, 2, figsize=(10, 3.5))
for ax, op in zip(axes, ["SINGLE-SHOT", "READ"]):
  bars(ax, SIZES, lambda i, s: P(alloc[(i, s, op)], 50) / s, IMPLS, "{:.2f}", logy=True)
  ax.set_title(op)
  ax.set_ylabel("heap bytes allocated ÷ object bytes (log)")
axes[0].legend(fontsize=7)
fig.suptitle("Total memory allocated per byte transferred (all mallocs; short-lived, freed right away)",
             fontweight="bold")
save(fig, "w6_alloc_per_byte.png")

# 6. RSS + totals
fig, axes = plt.subplots(1, 3, figsize=(14, 3.6))
get = lambda i, s, k, f: f(proc[(i, s, k)]) if (i, s, k) in proc else float("nan")
bars(axes[0], SIZES, lambda i, s: get(i, s, "RSS_PEAK", lambda r: int(r["rss_kib"]) / 1024)
     - get(i, s, "RSS_START", lambda r: int(r["rss_kib"]) / 1024), IMPLS, "{:.0f}")
axes[0].set_title("Memory held: RSS growth (peak − start)")
axes[0].set_ylabel("MiB")
bars(axes[1], SIZES, lambda i, s: get(i, s, "WORKLOAD_TOTAL", lambda r: int(r["alloc_bytes"]) / 2**30), IMPLS,
     "{:.1f}", logy=True)
axes[1].set_title("Total allocated in 200 iterations")
axes[1].set_ylabel("GiB (log)")
bars(axes[2], SIZES, lambda i, s: get(i, s, "WORKLOAD_TOTAL", lambda r: float(r["cpu_ms"]) / 1000), IMPLS, "{:.1f}")
axes[2].set_title("Total CPU in 200 iterations")
axes[2].set_ylabel("CPU seconds")
axes[0].legend(fontsize=7)
fig.suptitle("Memory held vs memory allocated, and CPU — per object size", fontweight="bold")
save(fig, "w7_rss_alloc_cpu_totals.png")

# 7. prod vs POC latency
if prod:
  fig, axes = plt.subplots(1, 3, figsize=(14, 3.6))
  allimpl = ["prod_cpp", "prod_rust"] + IMPLS
  for ax, op in zip(axes, ["SINGLE-SHOT", "READ[1]", "DELETE"]):
    def g(i, s, op=op):
      if i.startswith("prod_"):
        return prod.get((i[5:], "latency", op, s), {}).get("p50", float("nan")) * 1000
      return P(lat[(i, s, op)], 50)
    bars(ax, SIZES, g, allimpl, "{:.0f}", logy=True)
    ax.set_title(op)
    ax.set_ylabel("p50 ms (log)")
  axes[0].legend(fontsize=6.5)
  fig.suptitle("SSB production (24 h, us-central1) vs POC (us-west4) — p50 latency", fontweight="bold")
  save(fig, "w8_prod_vs_poc.png")

# 8. build metrics
bm = {}
bp = os.path.join(RES, "build-metrics.txt")
if os.path.exists(bp):
  for line in open(bp):
    if "=" in line:
      k, v = line.rstrip("\n").split("=", 1)
      bm[k] = v

  def num(k):
    try:
      return float(bm.get(k, "nan"))
    except ValueError:
      return float("nan")

  fig, axes = plt.subplots(1, 2, figsize=(13, 3.8))
  native_json = [("vcpkg tool", num("time_s.vcpkg.clone_bootstrap"), "#d2e3fc"),
                 ("C/C++ deps", num("time_s.native.deps_json") + num("time_s.native.deps_otel"), "#8ab4f8"),
                 ("storage lib", num("time_s.native.gcc_storage_json"), "#1a73e8"),
                 ("app", num("time_s.native.json_min_app"), "#174ea6")]
  native_grpc = native_json[:3] + [("+ gRPC/protobuf deps", num("time_s.native.deps_grpc_extra"), "#c6dafc"),
                                   ("+ storage_grpc lib", num("time_s.native.gcc_storage_grpc"), "#174ea6")]
  wrapped = [("Rust toolchain", num("time_s.toolchain.rust_install"), "#fdd663"),
             ("cbindgen", num("time_s.toolchain.cbindgen_install"), "#fbbc04"),
             ("crates", num("time_s.rust.cargo_fetch"), "#fcc934"),
             ("Rust core (cargo)", num("time_s.rust.cargo_build_release"), "#e8710a"),
             ("C++ wrapper + app", num("time_s.wrapper.cmake_configure") + num("time_s.wrapper.cmake_build"), "#b06000")]
  rows = [("Native C++\nJSON (storage)", native_json), ("Rust-wrapped C++\nJSON", wrapped),
          ("Native C++\n+ gRPC (storage_grpc)", native_grpc)]
  ax = axes[0]
  for y, (name, parts) in enumerate(rows):
    left = 0
    for lbl, v, c in parts:
      if v == v:
        ax.barh(y, v / 60, left=left, color=c, edgecolor="white")
        if v / 60 > 0.45:
          ax.text(left + v / 120, y, f"{lbl}\n{v / 60:.1f}m", ha="center", va="center", fontsize=6)
        left += v / 60
    ax.text(left + 0.2, y, f"{left:.1f} min", va="center", fontsize=8, fontweight="bold")
  ax.set_yticks(range(len(rows)), [r[0] for r in rows])
  ax.invert_yaxis()
  ax.set_xlabel(f"minutes from scratch on {bm.get('cpus', '?')} vCPUs")
  ax.set_title("Build time from scratch (empty caches)")

  ax = axes[1]
  items = [("json app native\n(stripped)", num("size.json_min_native_stripped")),
           ("json app wrapped\n(stripped)", num("size.json_min_wrapped_stripped")),
           ("libgcs_core.so\n(stripped)", num("size.libgcs_core_so_stripped")),
           ("libgcs_core.a\n(no debug)", num("size.libgcs_core_a_nodebug")),
           ("libgoogle_cloud_cpp\n_storage.a", num("size.libgoogle_cloud_cpp_storage_a"))]
  vals = [v / 2**20 for _, v in items]
  b = ax.bar(range(len(items)), vals, color=["#1a73e8", "#e8710a", "#e8710a", "#e8710a", "#1a73e8"])
  for rect, v in zip(b, vals):
    ax.annotate(f"{v:.1f}", (rect.get_x() + rect.get_width() / 2, v), ha="center", va="bottom", fontsize=7)
  ax.set_xticks(range(len(items)), [n for n, _ in items], fontsize=7)
  ax.set_ylabel("MiB")
  ax.set_title("Binary and library sizes")
  save(fig, "w9_build_time_size.png")

# 9. interleaved 100 MB A/B (optional)
ab = defaultdict(list)
for f in glob.glob(os.path.join(RES, "ab100mb", "*-r*.csv")):
  impl = os.path.basename(f).split("-w1r3-")[0]
  rnd = os.path.basename(f).split("-r")[-1][:-4]
  for r in csv.DictReader(open(f)):
    if r["iteration"] != "-1" and r["ok"] == "1" and r["op"] in ("READ[0]", "READ[1]", "READ[2]", "SINGLE-SHOT"):
      ab[(impl, rnd, "READ" if r["op"].startswith("READ") else "UPLOAD")].append(float(r["latency_ms"]))
if ab:
  fig, axes = plt.subplots(1, 2, figsize=(11, 3.4))
  for ax, op in zip(axes, ["READ", "UPLOAD"]):
    groups = ["1", "2", "3", "4", "all"]
    def g(i, rd, op=op):
      v = sum((ab[(i, r, op)] for r in "1234"), []) if rd == "all" else ab[(i, rd, op)]
      return P(v, 50)
    bars(ax, groups, g, IMPLS, "{:.0f}")
    ax.set_xticks(range(5), ["round 1", "round 2", "round 3", "round 4", "all rounds"])
    ax.set_title(f"100 MB {op.lower()} p50 (25 iterations per impl per round)")
    ax.set_ylabel("ms")
  axes[0].legend(fontsize=7, loc="lower right")
  fig.suptitle("Interleaved A/B check: same time window for all three", fontweight="bold")
  save(fig, "w10_ab_100mb.png")

print("\n" + "\n".join(saved), file=sys.stderr)
