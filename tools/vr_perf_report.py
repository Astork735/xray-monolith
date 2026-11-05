"""Summarize an explicit vr_perf_capture dump from an existing engine log."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import sys


# Current (version 4) stage order. Each capture's own header decides its
# stages, so version 1 logs (one "mirror" stage) and version 2 logs (no
# post/light parts) and version 3 logs (no volumetric/indirect) still parse.
STAGES = (
    "wait_frame", "begin_frame", "tracking", "input", "ui_wait", "simulation",
    "frame_work", "visibility", "geometry", "query_wait", "sun", "local_lights",
    "post", "left_eye", "right_eye", "scope", "eye_wait", "depth_copy", "eye_mirror",
    "ui_mirror", "end_frame", "present", "tail",
    # Consecutive parts of post (phase_combine) and of local_lights.
    "post_ao", "post_sky", "post_combine", "post_water", "post_forward", "post_bloom",
    "post_scatter", "post_blur", "post_grade", "post_aa", "post_final",
    "light_shadow", "light_accum", "light_volumetric", "light_indirect",
)
# Stages never nested in another; with wait_frame left out they partition a
# frame's active time. Needs the version 2 mirror split.
TOP_LEVEL = (
    "begin_frame", "tracking", "input", "ui_wait", "simulation", "frame_work",
    "left_eye", "right_eye", "scope", "eye_wait", "ui_mirror", "end_frame", "present", "tail",
)
CORE_STAGES = {"wait_frame", "simulation", "left_eye", "right_eye", "scope"}
VERSIONS = (1, 2, 3, 4)
GPU_STATES = ("unavailable", "pending", "ready", "busy", "disjoint", "error", "overflow", "device_lost")
GPU_USABLE = (2, 6)  # ready, overflow (partial: the events recorded before it are valid)
PREFIX = "[VR perf] "


def stages_of(row):
    """A capture's stages in mask-bit order, from its cpu_*_ms columns."""
    return [key[4:-3] for key in row if key.startswith("cpu_") and key.endswith("_ms")]


def read_captures(lines):
    captures = []
    columns = None
    version = None
    for number, line in enumerate(lines, 1):
        if PREFIX not in line:
            continue
        payload = line.split(PREFIX, 1)[1].strip()
        if payload.startswith("capture,"):
            values = payload.split(",")
            try:
                version = int(values[values.index("version") + 1])
            except (ValueError, IndexError) as error:
                raise ValueError(f"line {number}: capture line has no version") from error
            if version not in VERSIONS:
                raise ValueError(f"line {number}: unsupported capture version {version}")
            continue
        if not payload.startswith(("columns,", "frame,")):
            continue
        values = next(csv.reader([payload]))
        if values[0] == "columns":
            columns = values[1:]
            required = {"sequence", "elapsed_ms", "interval_ms", "period_ms", "predicted_time", "cpu_mask",
                        "gpu_mask", "gpu_state", "gpu_total_ms", "aborted", "should_render", "scene_rendered",
                        "wait_result", "begin_result", "end_result", "desktop_active", "draws_scope"}
            cpu = [c[4:-3] for c in columns if c.startswith("cpu_") and c.endswith("_ms")]
            gpu = [c[4:-3] for c in columns if c.startswith("gpu_") and c.endswith("_ms") and c != "gpu_total_ms"]
            if (len(columns) != len(set(columns)) or not required.issubset(columns) or cpu != gpu
                    or not CORE_STAGES.issubset(cpu) or len(cpu) > 64):
                raise ValueError(f"line {number}: unsupported or incomplete performance columns")
            captures.append([])
            continue
        if columns is None or len(values) - 1 != len(columns):
            raise ValueError(f"line {number}: performance row has no matching header or is truncated")
        row = dict(zip(columns, values[1:]))
        try:
            # Integer timestamps/masks must retain their precision.
            for key, value in row.items():
                row[key] = float(value) if key.endswith("_ms") else int(value)
                if isinstance(row[key], float) and not math.isfinite(row[key]):
                    raise ValueError("non-finite duration")
            if not 0 <= row["gpu_state"] < len(GPU_STATES):
                raise ValueError("unknown GPU state")
            if captures[-1] and row["sequence"] <= captures[-1][-1]["sequence"]:
                raise ValueError("non-increasing sequence")
        except ValueError as error:
            raise ValueError(f"line {number}: invalid performance sample ({error})") from error
        captures[-1].append(row)
    if not captures:
        raise ValueError("no capture dump found; run vr_perf_capture stop, then use the resulting log")
    return captures


def distribution(values):
    values = sorted(values)
    if not values:
        return None

    def percentile(p):
        position = (len(values) - 1) * p
        lower = math.floor(position)
        upper = math.ceil(position)
        return values[lower] + (values[upper] - values[lower]) * (position - lower)

    return {"samples": len(values), "p50_ms": percentile(.5), "p95_ms": percentile(.95),
            "p99_ms": percentile(.99), "max_ms": values[-1]}


def xr_ok(row):
    # Qualified-success XR results are non-negative. INT_MIN means not called.
    return all(row[name] >= 0 for name in ("wait_result", "begin_result", "end_result"))


def derive(rows):
    """Adds active_ms, unattributed_ms and display_periods to copies of rows."""
    stages = stages_of(rows[0]) if rows else []
    split = all(name in stages for name in TOP_LEVEL)
    derived = []
    previous = None
    for source in rows:
        row = dict(source)
        mask = row["cpu_mask"]

        def cpu(name):
            index = stages.index(name)
            return row[f"cpu_{name}_ms"] if mask & (1 << index) else 0.

        # The runtime's own throttle is not the application's work.
        row["active_ms"] = row["elapsed_ms"] - cpu("wait_frame")
        row["unattributed_ms"] = row["active_ms"] - sum(cpu(name) for name in TOP_LEVEL) if split else None
        # Display periods this frame advanced the runtime's prediction by:
        # 1 at the display rate, 3 at a third of it; more than the usual
        # cadence is a frame the runtime had to cover. Adjacent valid frames only.
        row["display_periods"] = None
        if (previous is not None and row["sequence"] == previous["sequence"] + 1 and xr_ok(row) and xr_ok(previous)
                and row["predicted_time"] > 0 and previous["predicted_time"] > 0 and row["period_ms"] > 0):
            row["display_periods"] = (row["predicted_time"] - previous["predicted_time"]) / (row["period_ms"] * 1e6)
        previous = source
        derived.append(row)
    return derived


def summarize(rows):
    def valid_scene(row):
        return not row["aborted"] and row["should_render"] and row["scene_rendered"] and xr_ok(row)

    rows = derive(rows)
    stages = stages_of(rows[0]) if rows else list(STAGES)
    scope_bit = 1 << stages.index("scope")
    valid = [row for row in rows if valid_scene(row)]
    groups = {"all_rendered": valid,
              "scope_frame": [r for r in valid if r["cpu_mask"] & scope_bit],
              "no_scope_frame": [r for r in valid if not r["cpu_mask"] & scope_bit],
              "desktop_inactive": [r for r in valid if not r["desktop_active"]]}
    result = {"captured_frames": len(rows), "rendered_frames": len(valid),
              "excluded_frames": len(rows) - len(valid),
              "gpu_states": {name: sum(r["gpu_state"] == i for r in rows)
                             for i, name in enumerate(GPU_STATES)}, "groups": {}}
    for name, samples in groups.items():
        metrics = {}
        for field in ("elapsed_ms", "active_ms", "interval_ms", "period_ms", "gpu_total_ms"):
            values = [r[field] for r in samples if r[field] >= 0
                      and (field != "gpu_total_ms" or r["gpu_state"] in GPU_USABLE)]
            metrics[field] = distribution(values)
        # May be slightly negative from timer granularity; kept as measured.
        unattributed = [r["unattributed_ms"] for r in samples if r["unattributed_ms"] is not None]
        metrics["unattributed_ms"] = distribution(unattributed)
        for kind in ("cpu", "gpu"):
            for index, stage in enumerate(stages):
                field = f"{kind}_{stage}_ms"
                values = [r[field] for r in samples if r[f"{kind}_mask"] & (1 << index)
                          and r[field] >= 0 and (kind != "gpu" or r["gpu_state"] in GPU_USABLE)]
                if values:
                    metrics[field] = distribution(values)
        periods = [r["display_periods"] for r in samples if r["display_periods"] is not None]
        cadence = statistics.median_low([round(p) for p in periods]) if periods else None
        result["groups"][name] = {
            "frames": len(samples), "metrics": metrics,
            "display_periods": distribution(periods), "cadence_periods": cadence,
            "late_frames": sum(round(p) > cadence for p in periods) if periods else 0}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--capture", type=int, default=-1, help="1-based capture number; default is the last")
    parser.add_argument("--json", action="store_true", help="emit machine-readable statistics")
    args = parser.parse_args(argv)
    try:
        with args.log.open(encoding="utf-8-sig", errors="replace") as stream:
            captures = read_captures(stream)
        index = len(captures) - 1 if args.capture == -1 else args.capture - 1
        if not 0 <= index < len(captures):
            raise ValueError("capture number is outside the log's capture range")
        report = summarize(captures[index])
        report["capture"] = index + 1
        report["captures_in_log"] = len(captures)
    except (OSError, ValueError) as error:
        parser.exit(2, f"vr_perf_report: {error}\n")
    if args.json:
        print(json.dumps(report, indent=2))
        return 0
    print(f"Capture {index + 1}/{len(captures)}: {report['rendered_frames']} rendered / "
          f"{report['captured_frames']} captured; {report['excluded_frames']} excluded")
    print("GPU samples: " + ", ".join(f"{key}={value}" for key, value in report["gpu_states"].items() if value))
    for name, group in report["groups"].items():
        if not group["frames"]:
            continue
        print(f"\n{name}: {group['frames']} frames")
        periods = group["display_periods"]
        if periods:
            print(f"display periods per frame: usual {group['cadence_periods']}, p50 {periods['p50_ms']:.2f}, "
                  f"p99 {periods['p99_ms']:.2f}, max {periods['max_ms']:.2f}; "
                  f"{group['late_frames']} of {periods['samples']} frames above the usual cadence")
        print(f"{'inclusive duration':28} {'count':>6} {'p50':>9} {'p95':>9} {'p99':>9} {'max':>9} ms")
        for metric, stats in group["metrics"].items():
            if stats is not None:
                print(f"{metric:28} {stats['samples']:6} {stats['p50_ms']:9.3f} {stats['p95_ms']:9.3f} "
                      f"{stats['p99_ms']:9.3f} {stats['max_ms']:9.3f}")
    print("\nCPU stages are inclusive; GPU intervals include queue idle gaps. Do not sum nested spans.")
    print("active = elapsed - wait_frame; unattributed = active - top-level stages (untimed engine work).")
    print("Display periods come from the runtime's predicted display times: a proxy, not its missed-frame count,")
    print("and not motion-to-photon latency.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
