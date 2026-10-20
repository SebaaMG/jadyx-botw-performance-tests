import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("frame_chain", ROOT / "tools/jadyx/analyze_frame_chain.py")
CHAIN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHAIN
SPEC.loader.exec_module(CHAIN)


def captured(rows):
    events = [CHAIN.Event(*row, order=index) for index, row in enumerate(rows)]
    return CHAIN.Capture(1, 120, "frame-chain-v1", events, len(events), True)


def queue_rows(guest=501, request_host=10, end_host=None):
    if end_host is None:
        end_host = request_host
    metadata = (1 << 32) | 3
    return [
        (0, request_host, "ipc-request-begin", 90, guest),
        (1, 70, "guest-thread-ready", guest, 16),
        (2, 71, "guest-dispatch", guest, 1),
        (3, request_host, "gpu-queue-push-begin", 7, metadata),
        (4, request_host, "gpu-queue-push-end", 7, metadata),
        (5, 20, "gpu-queue-wait", 3000, 7),
        (6, 20, "gpu-work-begin", 7, metadata),
        (7, 20, "gpu-flush-recorded", 30, 7),
        (8, 99, "gpu-submit", 30, 7),
        (9, 20, "gpu-work-end", 7, metadata),
        (10, end_host, "ipc-request-end", 90, guest),
    ]


class FrameChainTests(unittest.TestCase):
    def test_consumer_fence_matches_point_and_threshold_across_hosts(self):
        rows = [
            (0, 20, "consumer-fence", 5, 100),
            (1, 30, "consumer-fence-queued", 5, 100),
            (2, 20, "consumer-fence", 5, 101),
            (3, 30, "consumer-fence-queued", 5, 101),
            (4, 40, "consumer-fence-ready", 5, 100),
            (8, 40, "consumer-fence-ready", 5, 101),
        ]
        result = CHAIN.analyze(captured(rows))["synchronization"]
        self.assertEqual([row["queued_to_ready_ms"] for row in result["consumer_fences"]], [3, 5])

    def test_consumer_capture_boundary_keeps_unknown_timing(self):
        rows = [(0, 40, "consumer-fence-ready", 5, 99),
                (1, 30, "consumer-fence-queued", 5, 100)]
        result = CHAIN.analyze(captured(rows))["synchronization"]
        self.assertTrue(all(row["queued_to_ready_ms"] is None for row in result["consumer_fences"]))

    def test_sync_callback_scopes_distinguish_global_lock_and_recovery_timeout(self):
        rows = [(1, 30, "syncpoint-callback-long", 500, 100),
                (2, 30, "syncpoint-callback-long", 2000, (1 << 63) | 101),
                (3, 40, "syncpoint-lock-long", 700, 103),
                (4, 50, "nv-event-recovery", (80 << 32) | 9, 1000)]
        result = CHAIN.analyze(captured(rows))["synchronization"]
        self.assertEqual(result["long_scopes"]["callback_inside_global_lock"]["sum_ms"], 0.5)
        self.assertEqual(result["long_scopes"]["callback_outside_global_lock"]["sum_ms"], 2)
        self.assertEqual(result["async_recovery_requests"][0]["syncpoint"], 9)
        self.assertEqual(result["async_recovery_requests"][0]["threshold"], 80)
        self.assertEqual(result["async_recovery_requests"][0]["timeout"], 1000)

    def test_submission_sizes_costs_and_drained_scope(self):
        rows = queue_rows() + [
            (6.1, 20, "gpu-frame-boundary", 100, 30),
            (7.1, 99, "gpu-list-close", 30, 25),
            (7.2, 99, "gpu-list-previous-submitted", 30, 29),
            (7.3, 99, "gpu-list-previous-completed", 30, 29),
            (8.1, 99, "gpu-list-work", 30, 192),
            (8.2, 99, "gpu-list-upload", 30, 4096),
            (8.3, 99, "gpu-list-reset", 30, 50),
            (9.1, 20, "gpu-frame-boundary", 101, 31),
        ]
        report = CHAIN.analyze(captured(rows))
        tick = report["native_ticks"][0]
        self.assertEqual(tick["completed_operations"], 192)
        self.assertEqual(tick["upload_bytes"], 4096)
        self.assertTrue(tick["scheduler_drained_before_submit"])
        interval = report["scheduler_frame_intervals"][0]
        self.assertEqual(interval["native_lists"], 1)
        self.assertTrue(interval["complete_native_lists"])
        self.assertEqual(interval["close_us"], 25)
        self.assertEqual(interval["allocator_and_reset_us"], 50)
        self.assertEqual(interval["scheduler_drained_submissions"], 1)
        self.assertEqual(report["summary"]["submission_close"]["sum_ms"], .025)

    def test_submission_metadata_missing_is_not_an_empty_list(self):
        rows = queue_rows() + [
            (6.1, 20, "gpu-frame-boundary", 100, 30),
            (9.1, 20, "gpu-frame-boundary", 101, 32),
        ]
        report = CHAIN.analyze(captured(rows))
        self.assertIsNone(report["native_ticks"][0]["completed_operations"])
        self.assertIsNone(report["native_ticks"][0]["scheduler_drained_before_submit"])
        interval = report["scheduler_frame_intervals"][0]
        self.assertFalse(interval["complete_native_lists"])
        self.assertEqual(interval["issued_lists"], 2)
        self.assertIsNone(interval["upload_bytes"])
        self.assertIsNone(interval["scheduler_drained_submissions"])

    def test_upload_only_list_and_recorder_delay_remain_visible(self):
        rows = queue_rows() + [
            (6.1, 20, "gpu-frame-boundary", 100, 30),
            (7.1, 99, "gpu-list-previous-submitted", 30, 29),
            (7.2, 99, "gpu-list-previous-completed", 30, 28),
            (8.1, 99, "gpu-list-work", 30, 0),
            (8.2, 99, "gpu-list-upload", 30, 4096),
            (7.9, 20, "gpu-frame-boundary", 101, 31),
        ]
        report = CHAIN.analyze(captured(rows))
        tick = report["native_ticks"][0]
        self.assertEqual(tick["completed_operations"], 0)
        self.assertEqual(tick["upload_bytes"], 4096)
        self.assertFalse(tick["scheduler_drained_before_submit"])
        self.assertEqual(report["scheduler_frame_intervals"][0]["native_lists"], 1)

    def test_arbitrary_producer_and_worker_submit_are_linked(self):
        report = CHAIN.analyze(captured(queue_rows()))
        job = report["jobs"][0]
        self.assertEqual(job["producer_guest"], 501)
        self.assertEqual(job["channel"], 3)
        self.assertEqual(job["enqueue_to_start_lower_ms"], 2)
        self.assertEqual(job["enqueue_to_start_upper_ms"], 3)
        self.assertEqual(report["native_ticks"][0]["work"], 7)
        self.assertEqual(report["native_ticks"][0]["record_to_submit_ms"], 1)
        self.assertEqual(job["first_native_submit_lag_ms"], 2)
        self.assertAlmostEqual(job["first_native_submit_fraction"], 2 / 3)
        self.assertEqual(report["ready_to_dispatch"][0]["ready_to_dispatch_ms"], 1)

    def test_consumer_can_start_before_push_end_is_logged(self):
        rows = queue_rows()
        rows[4] = (8, 10, "gpu-queue-push-end", 7, (1 << 32) | 3)
        job = CHAIN.analyze(captured(rows))["jobs"][0]
        self.assertEqual(job["enqueue_to_start_lower_ms"], 0)
        self.assertEqual(job["enqueue_to_start_upper_ms"], 3)

    def test_missing_boundary_is_unknown_not_zero(self):
        report = CHAIN.analyze(captured([row for row in queue_rows() if row[2] not in (
            "gpu-queue-push-begin", "gpu-queue-push-end", "gpu-work-end")]))
        job = report["jobs"][0]
        self.assertIsNone(job["execution_elapsed_ms"])
        self.assertIsNone(job["enqueue_to_start_lower_ms"])
        self.assertIsNone(job["producer_guest"])

    def test_original_requester_not_hle_worker_identity(self):
        report = CHAIN.analyze(captured(queue_rows(guest=9999, request_host=6111)))
        self.assertEqual(report["jobs"][0]["producer_guest"], 9999)
        self.assertNotIn("6111", report["producer_guests"])

    def test_migrated_ipc_host_does_not_invent_an_origin(self):
        report = CHAIN.analyze(captured(queue_rows(end_host=11)))
        self.assertIsNone(report["jobs"][0]["producer_guest"])

    def test_frame_numbers_are_scoped_by_queue(self):
        rows = [
            (0, 10, "ipc-request-begin", 90, 501),
            (1, 10, "buffer-wait-begin", 1, 0),
            (2, 20, "buffer-queued-frame", 2, 5),
            (3, 20, "buffer-acquired-frame", 2, 5),
            (4, 20, "buffer-released-frame", 2, 5),
            (5, 20, "buffer-released-frame", 1, 7),
            (6, 20, "buffer-dequeue-signal", 1, 0),
            (7, 10, "buffer-wait-end", 1, 6000),
            (8, 10, "ipc-request-end", 90, 501),
        ]
        report = CHAIN.analyze(captured(rows))
        wait = report["framebuffer_waits"][0]
        self.assertEqual(wait["frames_released_during_wait"], [7])
        self.assertEqual(wait["signals_during_wait"], 1)
        self.assertEqual(wait["guest"], 501)
        partial_frame = next(row for row in report["frames"] if row["queue"] == 1)
        self.assertIsNone(partial_frame["acquire_to_release_ms"])

    def test_svc_pairing_survives_guest_host_migration(self):
        rows = queue_rows() + [
            (2, 80, "guest-svc-begin", 501, 33),
            (5, 81, "guest-svc-end", 501, 33),
        ]
        report = CHAIN.analyze(captured(rows))
        self.assertEqual(report["jobs"][0]["producer_svc_overlap_ms"], 3)

    def test_elapsed_run_is_not_reported_as_cpu_execution(self):
        rows = queue_rows() + [
            (5, 80, "guest-run-long", 3000, (501 << 8) | 1),
            (10, 80, "guest-host-cpu-window", 16000, (1 << 56) | 100000),
        ]
        report = CHAIN.analyze(captured(rows))
        self.assertEqual(report["jobs"][0]["producer_run_elapsed_overlap_ms"], 3)
        self.assertEqual(report["host_cpu_windows"][0]["cpu_ms"], 16)
        self.assertEqual(report["host_cpu_windows"][0]["wall_ms"], 100)

    def test_pre_capture_wait_is_clipped_for_capture_totals(self):
        rows = queue_rows()
        rows[5] = (5, 20, "gpu-queue-wait", 20000, 7)
        report = CHAIN.analyze(captured(rows))
        self.assertEqual(report["jobs"][0]["preceding_queue_wait_ms"], 20)
        self.assertEqual(report["summary"]["software_queue_wait"]["sum_ms"], 5)

    def test_native_origin_mismatch_is_reported(self):
        rows = queue_rows()
        rows[8] = (8, 99, "gpu-submit", 30, 999)
        report = CHAIN.analyze(captured(rows))
        self.assertEqual(report["issues"]["native_submit_origin_mismatch"], 1)

    def test_overlapping_spans_are_merged_not_added(self):
        self.assertEqual(CHAIN.overlap_ms(0, 10, [(1, 5), (3, 7), (8, 20)]), 8)

    def test_native_fence_to_sync_wake_chain_is_linked_by_identity(self):
        rows = queue_rows() + [
            (0, 20, "fence-backend-tick", 29, 5000),
            (1, 70, "sync-wait-begin", 1, 79),
            (1.1, 70, "sync-wait-object", 8000, 79 << 32),
            (4, 30, "fence-gpu-wait-long", 2000, 29),
            (4.1, 30, "fence-callbacks-begin", 5000, 1),
            (4.2, 30, "nv-event-signal", 8000, 99),
            (4.3, 30, "sync-wake-object", 8000, 79),
            (4.4, 30, "fence-done", 5000, 0),
            (4.5, 71, "sync-wait-end", 0, 79),
        ]
        report = CHAIN.analyze(captured(rows))
        wait = report["sync_waits"][0]
        self.assertEqual(wait["native_tick"], 29)
        self.assertEqual(wait["native_fence_wait_elapsed_ms"], 2)
        self.assertAlmostEqual(wait["wake_to_resume_ms"], .2)
        self.assertNotEqual(wait["begin_host"], wait["end_host"])
        self.assertEqual(report["jobs"][0]["native_signalled_sync_wait_overlap_ms"], 2.5)
        self.assertNotEqual(wait["guest"], report["jobs"][0]["producer_guest"])

    def test_signal_on_unrelated_object_does_not_invent_dependency(self):
        rows = [
            (0, 20, "fence-backend-tick", 29, 5000),
            (1, 70, "sync-wait-begin", 1, 79),
            (1.1, 70, "sync-wait-object", 8000, 79 << 32),
            (2, 30, "fence-callbacks-begin", 5000, 1),
            (3, 30, "nv-event-signal", 9000, 99),
            (4, 30, "sync-wake-object", 8000, 79),
            (5, 71, "sync-wait-end", 0, 79),
        ]
        wait = CHAIN.analyze(captured(rows))["sync_waits"][0]
        self.assertIsNone(wait["native_tick"])
        self.assertIsNone(wait["native_signal_ms"])

    def test_open_sync_wait_is_unknown_and_stale_callback_is_not_reused(self):
        rows = [
            (0, 20, "fence-backend-tick", 29, 5000),
            (1, 30, "fence-callbacks-begin", 5000, 1),
            (2, 30, "fence-done", 5000, 0),
            (3, 70, "sync-wait-begin", 1, 79),
            (3.1, 70, "sync-wait-object", 8000, 79 << 32),
            (4, 30, "nv-event-signal", 8000, 99),
            (5, 30, "sync-wake-object", 8000, 79),
        ]
        wait = CHAIN.analyze(captured(rows))["sync_waits"][0]
        self.assertIsNone(wait["native_tick"])
        self.assertIsNone(wait["end_ms"])
        self.assertIsNone(wait["elapsed_ms"])

    def test_reused_fence_identity_does_not_borrow_an_old_tick(self):
        rows = [
            (0, 20, "fence-queued", 5000, 1),
            (1, 20, "fence-backend-tick", 29, 5000),
            (2, 30, "fence-callbacks-begin", 5000, 1),
            (3, 30, "fence-done", 5000, 0),
            (4, 20, "fence-queued", 5000, 1),
            (5, 70, "sync-wait-begin", 1, 79),
            (6, 70, "sync-wait-object", 8000, 79 << 32),
            (7, 30, "fence-callbacks-begin", 5000, 1),
            (8, 30, "nv-event-signal", 8000, 99),
            (9, 30, "sync-wake-object", 8000, 79),
            (10, 71, "sync-wait-end", 0, 79),
        ]
        wait = CHAIN.analyze(captured(rows))["sync_waits"][0]
        self.assertIsNone(wait["native_tick"])

    def test_capture_parser_separates_runs_and_rejects_incomplete_dump(self):
        text = "Frame trace: recording 120 vsyncs (capture 1), frame-chain-v1\n"
        text += "Frame trace: 1 events\n[0.0] FT 1.000 80 gpu-submit 1 7\nFrame trace: end\n"
        text += "Frame trace: recording 120 vsyncs (capture 2), frame-chain-v1\n"
        text += "Frame trace: 2 events\n[0.0] FT 2.000 80 gpu-submit 1 8\n"
        parsed = CHAIN.parse_captures(text)
        self.assertEqual(len(parsed), 2)
        self.assertTrue(CHAIN.analyze(parsed[0])["complete_dump"])
        self.assertFalse(CHAIN.analyze(parsed[1])["complete_dump"])


if __name__ == "__main__":
    unittest.main()
