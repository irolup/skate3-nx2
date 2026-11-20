#!/usr/bin/env python3
"""Summarise an A/B experiment run (options.txt: ns2_ab_phases=...) or compare several logs.

Usage:
  python3 -I ns2recomp/tools/ab_report.py logs/logs/skate3_001.log [more logs...]
      [--min-draws 1000] [--keep-first]

Each statistics line carries a phase label "<phase>#<window>/c<cycle>" (or "-" without the
cycler; then the log file name is used as the phase). The first window of every phase visit is
skipped (it straddles the switch) unless --keep-first is given; windows with fewer average draws
per frame than --min-draws are treated as menus/loading and skipped.
"""
import os
import re
import sys
from collections import defaultdict


def parse_args(argv):
    logs, min_draws, keep_first = [], 1000, False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--min-draws":
            min_draws = int(argv[i + 1])
            i += 2
            continue
        if a == "--keep-first":
            keep_first = True
        else:
            logs.append(a)
        i += 1
    return logs, min_draws, keep_first


def split_label(label, fallback):
    if label in (None, "", "-"):
        return fallback, 2
    label = label.replace("(hold)", "")
    m = re.match(r"(.*)#(\d+)/c(\d+)$", label)
    if not m:
        return label, 2
    return m.group(1), int(m.group(2))


def mean(values):
    return sum(values) / len(values) if values else float("nan")


def main():
    logs, min_draws, keep_first = parse_args(sys.argv[1:])
    if not logs:
        print(__doc__)
        return 1
    stats = defaultdict(lambda: defaultdict(list))
    order = []

    def bucket(phase):
        if phase not in stats:
            order.append(phase)
        return stats[phase]

    re_stats = re.compile(
        r"GPU stats(?: \[([^\]]*)\])?: (\d+) swaps in (\d+) ms.*?host_gpu_frame_us\[avg=(\d+) "
        r"max=(\d+).*?frames\[>20ms=(\d+) >25ms=(\d+).*?avg_draws=(\d+)\]")
    re_profile = re.compile(r"Vulkan GPU profile(?: \[([^\]]*)\])? .*?\|(.*)$")
    re_rt = re.compile(
        r"RT transfers in (\d+) ms(?: \[([^\]]*)\])?: (\d+) transfers(?:.*?ranges=(\d+) "
        r"tiles=(\d+) valid_skip\[tiles=(\d+) ranges=(\d+)\] overwrite\[like=(\d+) "
        r"skipped_tiles=(\d+)\])?")
    re_tex = re.compile(
        r"Texture loads in (\d+) ms(?: \[([^\]]*)\])?: (\d+) loads(?:.*?resolve_to_texture "
        r"copies=(\d+))?")

    for log in logs:
        fallback = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(log)))) or log
        last_draws_ok = True
        for line in open(log, errors="replace"):
            m = re_stats.search(line)
            if m:
                phase, window = split_label(m.group(1), fallback)
                swaps, ms = int(m.group(2)), int(m.group(3))
                draws = int(m.group(8))
                last_draws_ok = draws >= min_draws
                if not last_draws_ok or (window == 1 and not keep_first) or not ms:
                    continue
                b = bucket(phase)
                b["fps"].append(swaps * 1000.0 / ms)
                b["gpu_frame_ms"].append(int(m.group(4)) / 1000.0)
                b["frames_over_25ms"].append(int(m.group(7)))
                b["draws"].append(draws)
                continue
            m = re_profile.search(line)
            if m:
                phase, window = split_label(m.group(1), fallback)
                if not last_draws_ok or (window == 1 and not keep_first):
                    continue
                b = bucket(phase)
                for name, us in re.findall(r" (\w+)=(\d+)\(\d+\)", m.group(2)):
                    b["prof_" + name].append(int(us) / 1000.0)
                continue
            m = re_rt.search(line)
            if m:
                phase, window = split_label(m.group(2), fallback)
                if not last_draws_ok or (window == 1 and not keep_first):
                    continue
                b = bucket(phase)
                seconds = int(m.group(1)) / 1000.0
                b["transfers_per_s"].append(int(m.group(3)) / seconds)
                if m.group(4) is not None:
                    b["candidate_tiles_per_s"].append(int(m.group(5)) / seconds)
                    b["valid_skipped_tiles_per_s"].append(int(m.group(6)) / seconds)
                    b["overwrite_like_per_s"].append(int(m.group(8)) / seconds)
                    b["overwrite_skipped_tiles_per_s"].append(int(m.group(9)) / seconds)
                continue
            m = re_tex.search(line)
            if m:
                phase, window = split_label(m.group(2), fallback)
                if not last_draws_ok or (window == 1 and not keep_first):
                    continue
                b = bucket(phase)
                seconds = int(m.group(1)) / 1000.0
                b["texture_loads_per_s"].append(int(m.group(3)) / seconds)
                if m.group(4) is not None:
                    b["resolve_copies_per_s"].append(int(m.group(4)) / seconds)

    if not order:
        print("No statistics lines found (needs ns2_debug=true).")
        return 1
    keys = ["fps", "gpu_frame_ms", "frames_over_25ms", "draws", "prof_Main", "prof_RtTransfer",
            "prof_TexUpload", "prof_TexUpCopy", "prof_PassEntry", "prof_Depth",
            "transfers_per_s", "candidate_tiles_per_s", "valid_skipped_tiles_per_s",
            "overwrite_like_per_s", "overwrite_skipped_tiles_per_s", "texture_loads_per_s",
            "resolve_copies_per_s"]
    width = max(10, max(len(p) for p in order) + 1)
    print("phase".ljust(width) + "windows " + " ".join(k[:12].rjust(12) for k in keys))
    for phase in order:
        b = stats[phase]
        row = phase.ljust(width) + str(len(b["fps"])).rjust(7) + " "
        row += " ".join(("%12.2f" % mean(b[k])) if b[k] else "           -" for k in keys)
        print(row)
    base = stats.get("base")
    if base and base["gpu_frame_ms"]:
        print()
        print("vs base (GPU frame ms / fps):")
        for phase in order:
            b = stats[phase]
            if phase == "base" or not b["gpu_frame_ms"]:
                continue
            print("  %-*s %+6.2f ms  %+6.2f fps" % (width, phase,
                                                    mean(b["gpu_frame_ms"]) -
                                                    mean(base["gpu_frame_ms"]),
                                                    mean(b["fps"]) - mean(base["fps"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
