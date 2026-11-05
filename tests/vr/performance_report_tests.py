"""Regression checks for capture integrity and missing-GPU handling."""
import importlib.util
import os
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location("vr_perf_report", Path(__file__).parents[2] / "tools/vr_perf_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)
# Written by vr_performance_tests (the collector itself); CTest passes its path.
CAPTURE_LOG = os.environ.get("VR_PERF_CAPTURE_LOG")


def bit(stage):
    return 1 << report.STAGES.index(stage)


def row(sequence=1, **changes):
    value = dict(sequence=sequence, elapsed_ms=10., interval_ms=33., period_ms=11.11, predicted_time=0,
                 gpu_state=2, gpu_total_ms=8., cpu_mask=bit("simulation"), gpu_mask=0,
                 aborted=0, should_render=1, scene_rendered=1, wait_result=0, begin_result=0,
                 end_result=0, desktop_active=1, draws_scope=0)
    value.update({f"{kind}_{stage}_ms": 0. for kind in ("cpu", "gpu") for stage in report.STAGES})
    value.update(changes)
    return value


def dump(rows, version=4):
    names = list(rows[0])
    return [f"* [VR perf] capture,1,frames,{len(rows)},version,{version}",
            "* [VR perf] columns," + ",".join(names)] + [
        "* [VR perf] frame," + ",".join(str(item[name]) for name in names) for item in rows]


class PerformanceReportTests(unittest.TestCase):
    def test_multiple_captures_and_timestamp_precision(self):
        first = row(predicted_time=987654321012345678)
        captures = report.read_captures(dump([first]) + ["unrelated engine log"] + dump([row(7)]))
        self.assertEqual(len(captures), 2)
        self.assertEqual(captures[0][0]["predicted_time"], first["predicted_time"])

    def test_truncated_row_rejected(self):
        lines = dump([row()])
        lines[-1] = lines[-1].rsplit(",", 1)[0]
        with self.assertRaises(ValueError):
            report.read_captures(lines)

    def test_no_header_or_duplicate_sequence_rejected(self):
        with self.assertRaises(ValueError):
            report.read_captures(dump([row()])[2:])
        with self.assertRaises(ValueError):
            report.read_captures(dump([row(), row()]))

    def test_nonfinite_duration_rejected(self):
        with self.assertRaises(ValueError):
            report.read_captures(dump([row(elapsed_ms=float("nan"))]))

    def test_unknown_version_rejected(self):
        with self.assertRaises(ValueError):
            report.read_captures(dump([row()], version=5))

    def test_version_1_mirror_parses_without_residual(self):
        # Version 1 had one "mirror" stage, nested and top level at once.
        v1 = [s for s in report.STAGES if s not in ("eye_mirror", "ui_mirror")]
        v1.insert(v1.index("end_frame"), "mirror")
        old = {k: v for k, v in row().items()
               if not (k.startswith(("cpu_", "gpu_")) and k.endswith("_ms") and k != "gpu_total_ms")}
        old.update({f"{kind}_{stage}_ms": 0. for kind in ("cpu", "gpu") for stage in v1})
        old["cpu_mask"] = 1 << v1.index("simulation")
        rows = report.read_captures(dump([old], version=1))[0]
        result = report.summarize(rows)
        self.assertEqual(result["rendered_frames"], 1)
        self.assertIsNone(result["groups"]["all_rendered"]["metrics"]["unattributed_ms"])
        self.assertIn("cpu_simulation_ms", result["groups"]["all_rendered"]["metrics"])

    def test_invalid_and_incomplete_frames_excluded(self):
        rows = [row(), row(2, aborted=1), row(3, scene_rendered=0), row(4, end_result=-2147483648),
                row(5, wait_result=-1), row(6, end_result=3)]
        result = report.summarize(rows)
        self.assertEqual(result["rendered_frames"], 2)
        self.assertEqual(result["excluded_frames"], 4)

    def test_missing_gpu_is_not_zero(self):
        result = report.summarize([row(gpu_state=3, gpu_total_ms=0.)])
        self.assertIsNone(result["groups"]["all_rendered"]["metrics"]["gpu_total_ms"])
        self.assertEqual(result["gpu_states"]["busy"], 1)

    def test_stage_masks_scope_groups_and_partial_gpu(self):
        # Overflow (state 6) keeps the GPU events recorded before it.
        rows = [row(cpu_mask=bit("scope"), cpu_scope_ms=6., gpu_state=6,
                    gpu_mask=bit("scope"), gpu_scope_ms=4.), row(2)]
        result = report.summarize(rows)
        self.assertEqual(result["groups"]["scope_frame"]["frames"], 1)
        stats = result["groups"]["scope_frame"]["metrics"]
        self.assertEqual(stats["gpu_scope_ms"]["max_ms"], 4.)
        self.assertNotIn("cpu_simulation_ms", stats)

    def test_active_and_unattributed_use_top_level_stages_only(self):
        mask = bit("wait_frame") | bit("simulation") | bit("left_eye") | bit("geometry") | bit("eye_mirror") | \
            bit("ui_mirror")
        # 30 ms elapsed: 20 waiting, 4 simulation, 3 left eye (with nested
        # geometry and eye mirror that must not count again), 1 UI mirror.
        sample = row(elapsed_ms=30., cpu_mask=mask, cpu_wait_frame_ms=20., cpu_simulation_ms=4.,
                     cpu_left_eye_ms=3., cpu_geometry_ms=2., cpu_eye_mirror_ms=.5, cpu_ui_mirror_ms=1.)
        metrics = report.summarize([sample])["groups"]["all_rendered"]["metrics"]
        self.assertAlmostEqual(metrics["active_ms"]["max_ms"], 10.)
        self.assertAlmostEqual(metrics["unattributed_ms"]["max_ms"], 2.)

    def test_display_periods_cadence_and_late_frames(self):
        period = 11.11
        step = int(period * 1e6)
        times = [0, 3, 6, 9, 13, 16]  # in display periods: one frame took four
        rows = [row(i + 1, predicted_time=10**15 + t * step, period_ms=period) for i, t in enumerate(times)]
        group = report.summarize(rows)["groups"]["all_rendered"]
        self.assertEqual(group["cadence_periods"], 3)
        self.assertEqual(group["late_frames"], 1)
        self.assertEqual(group["display_periods"]["samples"], 5)
        self.assertAlmostEqual(group["display_periods"]["max_ms"], 4., places=3)

    def test_display_periods_skip_gaps_and_failed_frames(self):
        step = int(11.11e6)
        rows = [row(1, predicted_time=step), row(2, predicted_time=2 * step, wait_result=-1),
                row(3, predicted_time=3 * step), row(5, predicted_time=9 * step)]
        derived = report.derive(rows)
        self.assertEqual([r["display_periods"] for r in derived], [None, None, None, None])

    def test_percentiles_and_empty_distribution(self):
        self.assertIsNone(report.distribution([]))
        self.assertAlmostEqual(report.distribution([3, 1])["p99_ms"], 2.98)
        self.assertEqual(report.distribution([5])["max_ms"], 5)

    @unittest.skipUnless(CAPTURE_LOG, "VR_PERF_CAPTURE_LOG not set (CTest sets it)")
    def test_collector_dump_end_to_end(self):
        path = Path(CAPTURE_LOG)
        text = path.read_text(encoding="utf-8")
        self.assertIn(",version,4", text)
        captures = report.read_captures(text.splitlines())
        self.assertEqual(len(captures), 1)
        self.assertEqual(report.stages_of(captures[0][0]), list(report.STAGES))
        result = report.summarize(captures[0])
        # GpuLifecycleAndDump: 32 rendered frames, then one aborted by a detach.
        self.assertEqual(result["captured_frames"], 33)
        self.assertEqual(result["rendered_frames"], 32)
        self.assertGreater(result["gpu_states"]["ready"], 0)
        self.assertIn("gpu_geometry_ms", result["groups"]["all_rendered"]["metrics"])
        self.assertEqual(report.main([str(path)]), 0)


if __name__ == "__main__":
    unittest.main()
