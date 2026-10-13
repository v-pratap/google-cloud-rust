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
"""Summarises SSB CSVs (rust_wrapped vs native_cpp vs native_cpp-dp).

Usage: analyze_ssb.py <results-dir>   -> prints Markdown tables
"""
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

IMPLS = ["rust_wrapped", "native_cpp"]
WORKLOADS = ["w1r3", "open", "random8k", "read50x2x4"]
SIZE_LABEL = {100000: "100KB", 2097152: "2MiB", 100000000: "100MB", 209715200: "200MiB"}
OP_ORDER = ["SINGLE-SHOT", "RESUMABLE", "APPENDABLE", "UPLOAD", "READ[0]", "READ[1]", "READ[2]",
            "open", "random8k", "read50x2x4", "DOWNLOAD", "DELETE", "W1R3D1-SINGLE-SHOT",
            "W1R3D1-RESUMABLE"]


def pct(v, p):
  if not v:
    return float("nan")
  v = sorted(v)
  return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def load(d):
  rows = []
  for f in glob.glob(os.path.join(d, "*.csv")):
    with open(f) as fh:
      rows += list(csv.DictReader(fh))
  return rows


def main():
  d = sys.argv[1]
  rows = load(d)
  g = defaultdict(lambda: {"lat": [], "cpu": [], "tput": [], "err": 0, "n": 0})
  for r in rows:
    if r.get("iteration") == "-1":
      continue  # process-level rows (RSS / workload totals)
    size = int(r["object_size"])
    for key in [(r["workload"], r["op"], size, r["impl"]), (r["workload"], r["op"], "all", r["impl"])]:
      s = g[key]
      s["n"] += 1
      if r["ok"] != "1":
        s["err"] += 1
        continue
      lat = float(r["latency_ms"])
      s["lat"].append(lat)
      s["cpu"].append(float(r["cpu_ms"]))
      b = int(r["bytes"])
      if b > 0 and lat > 0:
        s["tput"].append(b / 1048576 / (lat / 1000))

  def fmt(x, nd=1):
    return "–" if x != x else f"{x:.{nd}f}"

  for w in WORKLOADS:
    ops = sorted({k[1] for k in g if k[0] == w}, key=lambda o: OP_ORDER.index(o) if o in OP_ORDER else 99)
    sizes = sorted({k[2] for k in g if k[0] == w and k[2] != "all"})
    if not ops:
      continue
    print(f"\n### {w}\n")
    print("| op | size | impl | n | err | p50 ms | p90 ms | p99 ms | mean ms | p50 MiB/s | p50 CPU ms |")
    print("|---|---|---|--:|--:|--:|--:|--:|--:|--:|--:|")
    for op in ops:
      for size in sizes:
        for impl in IMPLS:
          s = g.get((w, op, size, impl))
          if not s:
            continue
          no_tp = op in ("DELETE", "open") or op.startswith("W1R3D1")
          tp = fmt(pct(s["tput"], 50)) if s["tput"] and not no_tp else "–"
          print(f"| {op} | {SIZE_LABEL.get(size, size)} | {impl} | {s['n']} | {s['err']} | "
                f"{fmt(pct(s['lat'], 50))} | {fmt(pct(s['lat'], 90))} | {fmt(pct(s['lat'], 99))} | "
                f"{fmt(statistics.mean(s['lat']) if s['lat'] else float('nan'))} | {tp} | {fmt(pct(s['cpu'], 50))} |")

  # Optional: production (SSB release, C++) for the same (workload, op, size).
  prod = {}
  if len(sys.argv) > 2:
    import json
    for f in glob.glob(os.path.join(sys.argv[2], "*-latency.json")):
      d = json.load(open(f))
      w = os.path.basename(f).split("-")[0]
      for r in d["rows"]:
        prod[(w, r["ssb_op"], int(r["ssb_object_size"]))] = r

  print("\n### Per (workload, op, size): p50 / p99 ms" + (" vs production" if prod else "") + "\n")
  hdr = "| workload | op | size | rust_wrapped p50 | native_cpp p50 | wrapped ÷ native | rust_wrapped p99 | native_cpp p99 |"
  sep = "|---|---|---|--:|--:|--:|--:|--:|"
  if prod:
    hdr += " prod C++ p50 | prod C++ p99 | native ÷ prod | wrapped ÷ prod |"
    sep += "--:|--:|--:|--:|"
  print(hdr)
  print(sep)
  for w in WORKLOADS:
    ops = sorted({k[1] for k in g if k[0] == w}, key=lambda o: OP_ORDER.index(o) if o in OP_ORDER else 99)
    sizes = sorted({k[2] for k in g if k[0] == w and k[2] != "all"})
    for op in ops:
      if op == "DELETE" and w != "w1r3":
        continue  # SSB doesn't record delete metrics for the async workloads
      for size in sizes:
        v = {i: g.get((w, op, size, i)) for i in IMPLS}
        p50 = {i: pct(x["lat"], 50) if x else float("nan") for i, x in v.items()}
        p99 = {i: pct(x["lat"], 99) if x else float("nan") for i, x in v.items()}
        ratio = p50["rust_wrapped"] / p50["native_cpp"] if p50["native_cpp"] else float("nan")
        row = (f"| {w} | {op} | {SIZE_LABEL.get(size, size)} | {fmt(p50['rust_wrapped'])} | "
               f"{fmt(p50['native_cpp'])} | {fmt(ratio, 2)}× | {fmt(p99['rust_wrapped'])} | {fmt(p99['native_cpp'])} |")
        if prod:
          pr = prod.get((w, op, size))
          if pr:
            pp50, pp99 = pr["p50"] * 1000, pr["p99"] * 1000
            row += (f" {fmt(pp50)} | {fmt(pp99)} | {fmt(p50['native_cpp'] / pp50, 2)}× | "
                    f"{fmt(p50['rust_wrapped'] / pp50, 2)}× |")
          else:
            row += " – | – | – | – |"
        print(row)


if __name__ == "__main__":
  main()
