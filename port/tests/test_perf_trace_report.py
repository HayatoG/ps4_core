# SPDX-License-Identifier: GPL-2.0-or-later
import sys, unittest, json, tempfile
import contextlib
import io
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import perf_trace_report as r


class Report(unittest.TestCase):
    def event(self, k, t, **kw):
        return dict(
            kind=k,
            ts_ns=t,
            frame=1,
            thread=1,
            op=0,
            object=0,
            generation=0,
            a=0,
            b=0,
            role=0,
            **kw,
        )

    def test_compile_overlap_is_not_added_twice(self):
        events = [
            self.event(5, 0),
            self.event(6, 100),
            self.event(5, 50),
            self.event(6, 150),
        ]
        for i, e in enumerate(events):
            e["op"] = 1 if i < 2 else 2
        self.assertEqual(r.analyze(events)["span_union_ns"]["compile"], 150)

    def test_incomplete_span_is_unknown(self):
        self.assertTrue(r.analyze([self.event(7, 0)])["incomplete_spans"])

    def test_unmatched_end_is_unknown(self):
        self.assertTrue(r.analyze([self.event(8, 0)])["incomplete_spans"])

    def test_negative_duration_rejected(self):
        a = self.event(5, 20)
        b = self.event(6, 10)
        a["op"] = b["op"] = 2
        incomplete = r.analyze([a, b])["incomplete_spans"]
        self.assertIn(a, incomplete)  # the begin is reported, not dropped
        self.assertIn(b, incomplete)

    def test_lifetime_reuse_is_not_same_texture(self):
        es = [self.event(9, 1), self.event(10, 2), self.event(9, 3)]
        for i, e in enumerate(es):
            e["object"] = 1
            e["generation"] = 1 if i < 2 else 2
        d = r.analyze(es)
        self.assertEqual(d["same_origin_recreations"], 1)
        self.assertFalse(d["proven_texture_churn"])

    def test_no_frames_no_fps_claim(self):
        self.assertIsNone(r.analyze([])["capture_frame_p99_ms"])

    def test_marker_category(self):
        e = self.event(2, 1)
        e["a"] = 1
        self.assertEqual(r.analyze([e])["markers"][0]["category"], "absorb_smoke")

    def read(self, lines):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "trace.jsonl"
            p.write_text("\n".join(lines), encoding="utf-8")
            return r.read_trace(p)

    def test_unknown_schema_rejected(self):
        self.assertTrue(self.read(['{"session":1,"schema":99}'])["warnings"])

    def test_truncation_visible(self):
        self.assertTrue(self.read(['{"session":1,"schema":1}', "{"])["warnings"])

    def test_loss_visible(self):
        self.assertTrue(
            self.read(
                ['{"session":1,"schema":1}', '{"capture_end":true,"dropped_total":4}']
            )["warnings"]
        )

    def test_missing_header_rejected(self):
        self.assertFalse(self.read([json.dumps(self.event(1, 1))])["events"])

    def test_feedback_linked_to_own_operation(self):
        e = self.event(18, 100)
        e["op"] = 7
        e["b"] = 1
        e["a"] = 400
        d = r.analyze([e])
        self.assertEqual(d["pipeline_feedback"][0]["cache"], "application_hit")
        self.assertEqual(d["pipeline_feedback"][0]["op"], 7)

    def test_invalid_feedback_stays_unknown(self):
        e = self.event(18, 100)
        e["a"] = 1234
        self.assertEqual(r.analyze([e])["pipeline_feedback"][0]["cache"], "unknown")
        self.assertIsNone(r.analyze([e])["pipeline_feedback"][0]["driver_duration_ns"])

    def test_complete_frame_series(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            p.write_text(
                "frame,ts_ns,interval_ns\n1,10,0\n2,20000010,20000000\n3,60000010,40000000\n# end,dropped_total=0,truncated=0\n"
            )
            a = r.read_frames(p)
            self.assertTrue(a["complete"])
            self.assertAlmostEqual(a["fps"], 2 / 0.06)
            self.assertEqual(a["p99_ms"], 40)

    def test_frame_gap_does_not_claim_complete(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            p.write_text(
                "frame,ts_ns,interval_ns\n1,10,0\n3,30,20\n# end,dropped_total=0,truncated=0\n"
            )
            self.assertFalse(r.read_frames(p)["complete"])

    def test_frame_crash_has_missing_footer(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            p.write_text("frame,ts_ns,interval_ns\n1,10,0\n")
            self.assertFalse(r.read_frames(p)["complete"])

    def test_frame_only_cli_report(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            p.write_text(
                "frame,ts_ns,interval_ns\n1,10,20000000\n# end,dropped_total=0,truncated=0\n",
                encoding="utf-8",
            )
            out = Path(d) / "report"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(r.main(["--frames", str(p), "--out", str(out)]), 0)
            report = json.loads(out.with_suffix(".json").read_text(encoding="utf-8"))
            self.assertTrue(report["frame_series"]["complete"])
            self.assertEqual(report["frame_series"]["p99_ms"], 20)

    def test_frame_series_warnings_reach_report_warnings(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            p.write_text("frame,ts_ns,interval_ns\n1,10,20000000\n", encoding="utf-8")
            out = Path(d) / "report"
            with contextlib.redirect_stdout(io.StringIO()):
                r.main(["--frames", str(p), "--out", str(out)])
            report = json.loads(out.with_suffix(".json").read_text(encoding="utf-8"))
            self.assertIn("frames:missing_frame_footer", report["warnings"])

    def test_queue_time_is_separate_from_work(self):
        enq = self.event(19, 10)
        begin = self.event(20, 60)
        end = self.event(21, 100)
        for e in (enq, begin, end):
            e["op"] = 9
        begin["thread"] = end["thread"] = 2
        d = r.analyze([enq, begin, end])
        self.assertEqual(d["queues"][0]["wait_ns"], 50)
        self.assertEqual(d["span_union_ns"]["queue_work"], 40)

    def test_buffer_request_is_linked_across_threads(self):
        request = self.event(22, 10)
        apply = self.event(23, 70)
        request["op"] = apply["op"] = 8
        apply["thread"] = 2
        d = r.analyze([request, apply])
        self.assertEqual(d["buffer_reads"][0]["request_to_apply_start_ns"], 60)

    def test_stall_retains_resource_evidence(self):
        e = self.event(9, 100_000_000)
        f = self.event(1, 200_000_000)
        f["a"] = 150_000_000
        d = r.analyze([e, f])
        self.assertEqual(d["stalls"][0]["events"][0]["kind"], 9)

    def test_valid_empty_is_not_performance_proof(self):
        self.assertIn("no_frame_data", r.analyze([])["limitations"])


if __name__ == "__main__":
    unittest.main()
