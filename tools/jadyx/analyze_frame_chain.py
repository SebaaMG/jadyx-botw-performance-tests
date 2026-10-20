"""Correlate bounded frame-chain captures without interpreting elapsed time as CPU usage."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import hashlib
import json
import math
from pathlib import Path
import re


ENTRY = re.compile(r"\bFT\s+([\d.]+)\s+(\d+)\s+([\w-]+)\s+(\d+)\s+(\d+)\s*$")
START = re.compile(r"Frame trace: recording (\d+) vsyncs \(capture (\d+)\), (.*)")
CAPACITY = 524288


@dataclass(frozen=True)
class Event:
    ms: float
    host: int
    kind: str
    a: int
    b: int
    order: int = 0


@dataclass
class Capture:
    identity: int
    vsyncs: int
    mode: str
    events: list[Event] = field(default_factory=list)
    declared: int | None = None
    ended: bool = False


def parse_captures(text):
    captures = []
    current = None
    for line in text.splitlines():
        match = START.search(line)
        if match:
            current = Capture(int(match[2]), int(match[1]), match[3])
            captures.append(current)
        if current is None:
            continue
        count = re.search(r"Frame trace: (\d+) events", line)
        if count:
            current.declared = int(count[1])
        match = ENTRY.search(line)
        if match:
            current.events.append(Event(float(match[1]), int(match[2]), match[3],
                                        int(match[4]), int(match[5]), len(current.events)))
        if "Frame trace: end" in line:
            current.ended = True
    return captures


def distribution(values):
    values = sorted(value for value in values if value is not None)
    if not values:
        return {"count": 0, "sum_ms": 0.0}
    return {"count": len(values), "sum_ms": sum(values), "min_ms": values[0],
            "p50_ms": values[max(0, math.ceil(len(values) * .5) - 1)],
            "p95_ms": values[max(0, math.ceil(len(values) * .95) - 1)],
            "max_ms": values[-1]}


def overlap_ms(begin, end, intervals):
    clipped = sorted((max(begin, left), min(end, right)) for left, right in intervals
                     if right > begin and left < end)
    total = 0.0
    last = begin
    for left, right in clipped:
        left = max(left, last)
        if right > left:
            total += right - left
            last = right
    return total


def correlate_sync_waits(events):
    open_waits = {}
    spans = []
    fence_ticks = {}
    callbacks = {}
    native_waits = {}
    signals = {}
    for event in events:
        if event.kind == "fence-queued":
            fence_ticks.pop(event.a, None)
        elif event.kind == "fence-backend-tick":
            fence_ticks[event.b] = event.a
        elif event.kind == "fence-callbacks-begin":
            callbacks[event.host] = (event.a, fence_ticks.get(event.a), event.ms)
        elif event.kind == "fence-done":
            if callbacks.get(event.host, (None,))[0] == event.a:
                callbacks.pop(event.host)
        elif event.kind == "fence-gpu-wait-long":
            native_waits[(event.host, event.b)] = event
        elif event.kind == "nv-event-signal":
            callback = callbacks.get(event.host)
            tick = callback[1] if callback else None
            signals[(event.host, event.a)] = {
                "signal": event, "tick": tick,
                "callback_begin_ms": callback[2] if callback else None,
                "native_wait": native_waits.get((event.host, tick)),
            }
        elif event.kind == "sync-wait-begin":
            open_waits[event.b] = {"begin": event, "guest": event.b, "objects": {}}
        elif event.kind == "sync-wait-object":
            wait = open_waits.get(event.b >> 32)
            if wait is not None:
                wait["objects"][event.b & 0xffffffff] = event.a
        elif event.kind == "sync-wake-object":
            wait = open_waits.get(event.b)
            if wait is not None and event.a in wait["objects"].values() and "wake" not in wait:
                wait["wake"] = event
                signal = signals.get((event.host, event.a))
                if signal is not None and signal["signal"].ms >= wait["begin"].ms:
                    wait["native_signal"] = signal
        elif event.kind == "sync-wait-end":
            wait = open_waits.pop(event.b, None)
            if wait is not None:
                wait["end"] = event
                spans.append(wait)
    spans.extend(open_waits.values())
    rows = []
    for wait in spans:
        begin, end, wake = wait["begin"], wait.get("end"), wait.get("wake")
        signal = wait.get("native_signal", {})
        native_wait = signal.get("native_wait")
        rows.append({
            "guest": wait["guest"], "begin_ms": begin.ms,
            "end_ms": end.ms if end else None,
            "elapsed_ms": end.ms - begin.ms if end else None,
            "begin_host": begin.host, "end_host": end.host if end else None,
            "object_count": begin.a, "captured_objects": list(wait["objects"].values()),
            "wake_ms": wake.ms if wake else None,
            "wake_object": wake.a if wake else None,
            "wake_to_resume_ms": end.ms - wake.ms if end and wake else None,
            "native_tick": signal.get("tick"),
            "native_signal_ms": signal["signal"].ms if signal else None,
            "native_signal_host": signal["signal"].host if signal else None,
            "callback_begin_ms": signal.get("callback_begin_ms"),
            "native_fence_wait_end_ms": native_wait.ms if native_wait else None,
            "native_fence_wait_elapsed_ms": native_wait.a / 1000 if native_wait else None,
        })
    return rows


def synchronization_report(events):
    consumers = defaultdict(dict)
    recoveries = []
    long_scopes = defaultdict(list)
    for event in events:
        if event.kind in ("consumer-fence", "consumer-fence-queued", "consumer-fence-ready"):
            stage = {"consumer-fence": "requested_ms", "consumer-fence-queued": "queued_ms",
                     "consumer-fence-ready": "ready_ms"}[event.kind]
            consumers[(event.a, event.b)][stage] = event.ms
        elif event.kind == "nv-event-recovery":
            recoveries.append({"ms": event.ms, "syncpoint": event.a & 0xffffffff,
                               "threshold": event.a >> 32, "timeout": event.b})
        elif event.kind in ("syncpoint-lock-long", "syncpoint-callback-long", "nv-event-recovery-long"):
            category = event.kind
            if category == "syncpoint-callback-long":
                category = ("callback_outside_global_lock" if event.b >> 63
                            else "callback_inside_global_lock")
            long_scopes[category].append(event.a / 1000)
    rows = []
    for (syncpoint, threshold), stages in sorted(consumers.items()):
        row = {"syncpoint": syncpoint, "threshold": threshold,
               **{stage: stages.get(stage) for stage in ("requested_ms", "queued_ms", "ready_ms")}}
        row["queued_to_ready_ms"] = (row["ready_ms"] - row["queued_ms"]
                                     if row["ready_ms"] is not None and row["queued_ms"] is not None
                                     else None)
        rows.append(row)
    return {"consumer_fences": rows, "async_recovery_requests": recoveries,
            "consumer_queue_to_ready": distribution(row["queued_to_ready_ms"] for row in rows),
            "long_scopes": {name: distribution(values) for name, values in sorted(long_scopes.items())},
            "scope": "T only; scopes shorter than 200 us and spans crossing capture boundaries omitted; scopes may overlap"}


def analyze(capture):
    events = sorted(capture.events, key=lambda event: (event.ms, event.order))
    if not events:
        raise ValueError("Capture has no events")
    final_ms = events[-1].ms
    sync_waits = correlate_sync_waits(events)
    issues = Counter()
    requests = {}
    jobs = defaultdict(dict)
    ticks = defaultdict(dict)
    frames = defaultdict(dict)
    last_frame = {}
    buffer_waits = []
    open_buffer_waits = {}
    signals = defaultdict(list)
    ready = {}
    wake_delays = []
    open_svcs = {}
    svc_intervals = defaultdict(list)
    run_intervals = defaultdict(list)
    cpu_windows = []
    consumers = {}
    leases = []
    dequeue_results = []
    fences = []
    submission_boundaries = []
    submission_fields = {
        "gpu-list-work": "completed_operations",
        "gpu-list-upload": "upload_bytes",
        "gpu-list-close": "close_us",
        "gpu-list-reset": "allocator_and_reset_us",
        "gpu-list-previous-submitted": "previous_submitted_tick",
        "gpu-list-previous-completed": "previous_completed_tick",
    }

    for event in events:
        kind = event.kind
        if kind in ("ipc-request-begin", "buffer-dequeue-begin"):
            if event.a in requests:
                issues["duplicate_request_begin"] += 1
            requests[event.a] = {"begin": event, "category": kind, "end": None}
        elif kind in ("ipc-request-end", "buffer-dequeue-end"):
            if event.a in requests:
                requests[event.a]["end"] = event
            else:
                issues["request_end_without_begin"] += 1
        elif kind in ("gpu-queue-push-begin", "gpu-queue-push-end", "gpu-work-begin", "gpu-work-end"):
            name = {"gpu-queue-push-begin": "push_begin", "gpu-queue-push-end": "push_end",
                    "gpu-work-begin": "work_begin", "gpu-work-end": "work_end"}[kind]
            if name in jobs[event.a]:
                issues["duplicate_work_boundary"] += 1
            jobs[event.a][name] = event
        elif kind == "gpu-queue-wait":
            jobs[event.b]["queue_wait"] = event
        elif kind in ("gpu-flush-recorded", "gpu-submit"):
            ticks[event.a]["recorded" if kind == "gpu-flush-recorded" else "submitted"] = event
        elif kind in submission_fields:
            key = submission_fields[kind]
            if key in ticks[event.a]:
                issues["duplicate_submission_metadata"] += 1
            ticks[event.a][key] = event.b
        elif kind == "gpu-frame-boundary":
            submission_boundaries.append(event)
        elif kind in ("buffer-queued-frame", "buffer-acquired-frame", "buffer-released-frame"):
            stage = kind.split("-")[1]
            key = (event.a, event.b)
            if stage in frames[key]:
                issues["duplicate_frame_stage"] += 1
            frames[key][stage] = event
            last_frame[(event.host, event.a, stage)] = key
        elif kind in ("buffer-queued-slot", "buffer-acquired-slot", "buffer-released-slot"):
            stage = kind.split("-")[1]
            key = last_frame.get((event.host, event.a, stage))
            if key is None:
                issues["slot_without_frame_boundary"] += 1
            else:
                frames[key][stage + "_slot"] = event.b & 0xffffffff
                if stage == "queued":
                    frames[key]["swap_interval"] = event.b >> 32
        elif kind == "buffer-wait-begin":
            key = (event.host, event.a)
            if key in open_buffer_waits:
                issues["unmatched_buffer_wait_begin"] += 1
            open_buffer_waits[key] = event
        elif kind == "buffer-wait-end":
            begin = open_buffer_waits.pop((event.host, event.a), None)
            if begin:
                buffer_waits.append({"begin": begin, "end": event})
            else:
                issues["buffer_wait_end_without_begin"] += 1
        elif kind == "buffer-dequeue-signal":
            signals[event.a].append(event)
        elif kind == "buffer-dequeued":
            dequeue_results.append(event)
        elif kind in ("buffer-returned-fence", "buffer-queued-fence"):
            fences.append({"ms": event.ms, "queue": event.a, "kind": kind,
                           "syncpoint": event.b & 0xffffffff, "target": event.b >> 32})
        elif kind == "guest-thread-ready":
            ready[event.a] = event
        elif kind == "guest-dispatch":
            begin = ready.pop(event.a, None)
            if begin:
                wake_delays.append({"guest": event.a, "begin_ms": begin.ms,
                                    "end_ms": event.ms, "ready_to_dispatch_ms": event.ms - begin.ms,
                                    "core": event.b})
        elif kind == "guest-svc-begin":
            open_svcs[(event.a, event.b)] = event
        elif kind == "guest-svc-end":
            begin = open_svcs.pop((event.a, event.b), None)
            if begin:
                svc_intervals[event.a].append((begin.ms, event.ms))
        elif kind == "guest-run-long":
            run_intervals[event.b >> 8].append((event.ms - event.a / 1000, event.ms))
        elif kind == "guest-host-cpu-window":
            cpu_windows.append({"host": event.host, "core": event.b >> 56,
                                "cpu_ms": None if event.a == 2**64 - 1 else event.a / 1000,
                                "wall_ms": (event.b & ((1 << 56) - 1)) / 1000})
        elif kind == "frame-lease-queue":
            consumers[event.a] = event.b
        elif kind in ("frame-lease-acquire", "frame-lease-hold", "frame-lease-release"):
            consumer = event.b >> 32
            leases.append({"ms": event.ms, "kind": kind, "consumer": consumer,
                           "queue": consumers.get(consumer), "frame": event.a,
                           "vsyncs": event.b & 0xffffffff})

    issues["unmatched_buffer_wait_begin"] += len(open_buffer_waits)
    issues["svc_spans_open_at_capture_end"] += len(open_svcs)
    issues["ready_without_dispatch_at_capture_end"] += len(ready)
    for (guest, svc), begin in open_svcs.items():
        svc_intervals[guest].append((begin.ms, final_ms))
    ipc_requests = [row for row in requests.values() if row["category"] == "ipc-request-begin"]
    requests_by_host = defaultdict(list)
    names_by_host = defaultdict(list)
    for request in ipc_requests:
        requests_by_host[request["begin"].host].append(request)
    for rows in requests_by_host.values():
        rows.sort(key=lambda row: row["begin"].ms)
    starts_by_host = {host: [row["begin"].ms for row in rows]
                      for host, rows in requests_by_host.items()}
    for event in events:
        if event.kind.startswith("guest-ipc-name"):
            names_by_host[event.host].append(event)
    name_times = {host: [event.ms for event in rows] for host, rows in names_by_host.items()}

    def owner_at(ms, host):
        rows = requests_by_host[host]
        position = bisect_right(starts_by_host.get(host, []), ms)
        for index in range(position - 1, -1, -1):
            match = rows[index]
            if ms <= (match["end"].ms if match["end"] else final_ms):
                return match if match["end"] is None or match["end"].host == host else None
        return None

    request_rows = []
    for identity, request in requests.items():
        begin, end = request["begin"], request["end"]
        row = {"identity": identity, "category": request["category"], "context": begin.b,
               "host": begin.host, "begin_ms": begin.ms,
               "end_ms": end.ms if end else None,
               "elapsed_ms": end.ms - begin.ms if end else None,
               "same_host": None if end is None else end.host == begin.host}
        if request["category"] == "ipc-request-begin":
            parts = {}
            names = names_by_host[begin.host]
            positions = name_times.get(begin.host, [])
            for event in names[bisect_left(positions, begin.ms):
                               bisect_right(positions, end.ms if end else final_ms)]:
                if event.b == begin.b:
                    parts[event.kind] = event.a.to_bytes(8, "little")
            row["service"] = b"".join(value for key, value in sorted(parts.items())).split(b"\0", 1)[0].decode(
                "ascii", errors="replace")
        request_rows.append(row)

    wait_rows = []
    for wait in buffer_waits:
        begin, end = wait["begin"], wait["end"]
        owner = owner_at(begin.ms, begin.host)
        releases = [key[1] for key, frame in frames.items() if key[0] == begin.a and
                    "released" in frame and begin.ms <= frame["released"].ms <= end.ms]
        acquires = [key[1] for key, frame in frames.items() if key[0] == begin.a and
                    "acquired" in frame and begin.ms <= frame["acquired"].ms <= end.ms]
        wait_rows.append({"queue": begin.a, "host": begin.host,
                          "guest": owner["begin"].b if owner else None,
                          "begin_ms": begin.ms, "end_ms": end.ms,
                          "elapsed_ms": end.b / 1000,
                          "signals_during_wait": sum(begin.ms <= event.ms <= end.ms
                                                     for event in signals[begin.a]),
                          "frames_released_during_wait": releases,
                          "frames_acquired_during_wait": acquires})

    job_rows = []
    for identity, job in sorted(jobs.items()):
        publish = job.get("push_begin")
        published = job.get("push_end")
        start = job.get("work_begin")
        end = job.get("work_end")
        waiting = job.get("queue_wait")
        owner = owner_at(publish.ms, publish.host) if publish else None
        guest = owner["begin"].b if owner else None
        metadata = publish.b if publish else start.b if start else None
        row = {"identity": identity, "kind": metadata >> 32 if metadata is not None else None,
               "channel": metadata & 0xffffffff if metadata is not None else None,
               "producer_guest": guest,
               "ipc_request": owner["begin"].a if owner else None,
               "publish_ms": publish.ms if publish else None,
               "published_ms": published.ms if published else None,
               "begin_ms": start.ms if start else None, "end_ms": end.ms if end else None,
               "execution_elapsed_ms": end.ms - start.ms if start and end else None,
               "publish_duration_ms": published.ms - publish.ms if publish and published else None,
               "enqueue_to_start_lower_ms": None, "enqueue_to_start_upper_ms": None}
        if publish and published and start:
            if start.ms < publish.ms or published.ms < publish.ms:
                issues["inconsistent_queue_boundary_order"] += 1
            else:
                row["enqueue_to_start_lower_ms"] = max(0, start.ms - published.ms)
                row["enqueue_to_start_upper_ms"] = start.ms - publish.ms
        if waiting:
            begin_ms = waiting.ms - waiting.a / 1000
            row["queue_wait_begin_ms"] = max(0, begin_ms)
            row["queue_wait_end_ms"] = waiting.ms
            row["preceding_queue_wait_ms"] = waiting.a / 1000
            row["queue_wait_in_capture_ms"] = waiting.ms - max(0, begin_ms)
            row["queue_wait_started_before_capture"] = begin_ms < 0
            row["producer_svc_overlap_ms"] = overlap_ms(begin_ms, waiting.ms, svc_intervals[guest]) if guest else None
            row["producer_run_elapsed_overlap_ms"] = overlap_ms(begin_ms, waiting.ms, run_intervals[guest]) if guest else None
            row["producer_framebuffer_wait_overlap_ms"] = overlap_ms(begin_ms, waiting.ms,
                [(wait["begin_ms"], wait["end_ms"]) for wait in wait_rows if wait["guest"] == guest]) if guest else None
        job_rows.append(row)

    linked_intervals = [(wait["begin_ms"], wait["end_ms"])
                        for wait in sync_waits if wait["native_tick"] is not None
                        and wait["end_ms"] is not None]
    for row in job_rows:
        if "queue_wait_begin_ms" in row:
            row["native_signalled_sync_wait_overlap_ms"] = overlap_ms(
                row["queue_wait_begin_ms"], row["queue_wait_end_ms"], linked_intervals)

    tick_rows = []
    for tick, info in sorted(ticks.items()):
        recorded, submitted = info.get("recorded"), info.get("submitted")
        work = recorded.b if recorded else submitted.b if submitted else 0
        if recorded and submitted and recorded.b != submitted.b:
            issues["native_submit_origin_mismatch"] += 1
        row = {"tick": tick, "work": work or None,
                          "recorded_ms": recorded.ms if recorded else None,
                          "submitted_ms": submitted.ms if submitted else None,
                          "record_to_submit_ms": submitted.ms - recorded.ms
                          if recorded and submitted else None}
        row.update({key: info.get(key) for key in submission_fields.values()})
        prior, completed = row["previous_submitted_tick"], row["previous_completed_tick"]
        row["scheduler_drained_before_submit"] = completed >= prior if prior is not None and completed is not None else None
        tick_rows.append(row)

    submission_frames = []
    for begin, end in zip(submission_boundaries, submission_boundaries[1:]):
        if end.a != begin.a + 1 or end.b < begin.b:
            issues["inconsistent_submission_frame_boundary"] += 1
            continue
        lists = [row for row in tick_rows if begin.b <= row["tick"] < end.b]
        row = {"begin_frame": begin.a, "end_frame": end.a,
               "begin_ms": begin.ms, "end_ms": end.ms,
               "begin_tick": begin.b, "end_tick": end.b,
               "issued_lists": end.b - begin.b,
               "native_lists": sum(item["submitted_ms"] is not None for item in lists)}
        row["complete_native_lists"] = row["native_lists"] == row["issued_lists"]
        for key in ("completed_operations", "upload_bytes", "close_us", "allocator_and_reset_us"):
            known = row["complete_native_lists"] and all(item[key] is not None for item in lists)
            row[key] = sum(item[key] for item in lists) if known else None
        row["scheduler_drained_submissions"] = (
            sum(item["scheduler_drained_before_submit"] for item in lists)
            if row["complete_native_lists"] and all(item["scheduler_drained_before_submit"] is not None for item in lists)
            else None)
        submission_frames.append(row)

    for job in job_rows:
        submitted = [tick["submitted_ms"] for tick in tick_rows
                     if tick["work"] == job["identity"] and tick["submitted_ms"] is not None
                     and job["begin_ms"] is not None and job["end_ms"] is not None
                     and job["begin_ms"] <= tick["submitted_ms"] <= job["end_ms"]]
        first = min(submitted) if submitted else None
        lag = first - job["begin_ms"] if first is not None else None
        job["first_native_submit_ms"] = first
        job["first_native_submit_lag_ms"] = lag
        job["first_native_submit_fraction"] = lag / job["execution_elapsed_ms"] if lag is not None and job["execution_elapsed_ms"] else None

    frame_rows = []
    for (queue, frame_number), info in sorted(frames.items()):
        row = {"queue": queue, "frame": frame_number}
        for stage in ("queued", "acquired", "released"):
            row[stage + "_ms"] = info[stage].ms if stage in info else None
            row[stage + "_slot"] = info.get(stage + "_slot")
        row["queue_to_acquire_ms"] = info["acquired"].ms - info["queued"].ms if "queued" in info and "acquired" in info else None
        row["acquire_to_release_ms"] = info["released"].ms - info["acquired"].ms if "acquired" in info and "released" in info else None
        frame_rows.append(row)

    producer_counts = Counter(row["producer_guest"] for row in job_rows if row["kind"] == 1)
    return {
        "schema": "seriesx-frame-chain-report-v1", "capture": capture.identity,
        "mode": capture.mode, "requested_vsyncs": capture.vsyncs,
        "events": len(events), "declared_events": capture.declared,
        "complete_dump": capture.ended and capture.declared == len(events),
        "capacity_limit_reached": len(events) >= CAPACITY,
        "duration_ms": final_ms, "issues": dict(issues),
        "producer_guests": {str(key): value for key, value in producer_counts.items()},
        "summary": {
            "software_queue_wait": distribution(row.get("queue_wait_in_capture_ms") for row in job_rows),
            "queue_start_lower_bound": distribution(row["enqueue_to_start_lower_ms"] for row in job_rows),
            "queue_start_upper_bound": distribution(row["enqueue_to_start_upper_ms"] for row in job_rows),
            "gpu_work_elapsed": distribution(row["execution_elapsed_ms"] for row in job_rows),
            "framebuffer_wait": distribution(row["elapsed_ms"] for row in wait_rows),
            "guest_ready_to_dispatch": distribution(row["ready_to_dispatch_ms"] for row in wake_delays),
            "record_to_native_submit": distribution(row["record_to_submit_ms"] for row in tick_rows),
            "submission_close": distribution(row["close_us"] / 1000 for row in tick_rows
                                              if row["close_us"] is not None),
            "submission_allocator_and_reset": distribution(row["allocator_and_reset_us"] / 1000
                                                            for row in tick_rows if row["allocator_and_reset_us"] is not None),
            "idle_overlap_native_signalled_sync_wait": distribution(
                row.get("native_signalled_sync_wait_overlap_ms") for row in job_rows),
            "submit_lists_over_5ms_first_native_submit_lag": distribution(
                row["first_native_submit_lag_ms"] for row in job_rows if row["kind"] == 1
                and row["execution_elapsed_ms"] is not None and row["execution_elapsed_ms"] > 5),
        },
        "jobs": job_rows, "native_ticks": tick_rows, "frames": frame_rows,
        "scheduler_frame_intervals": submission_frames,
        "framebuffer_waits": wait_rows, "ready_to_dispatch": wake_delays,
        "ipc_and_dequeue_requests": request_rows, "host_cpu_windows": cpu_windows,
        "frame_leases": leases, "framebuffer_fences": fences,
        "sync_waits": sync_waits,
        "synchronization": synchronization_report(events),
        "partial_submissions": [{"ms": event.ms, "host": event.host, "tick": event.a,
                                 "work_since_submit": event.b} for event in events
                                if event.kind == "gpu-partial-submit"],
        "dequeued_slots": [{"ms": event.ms, "queue": event.a, "slot": event.b} for event in dequeue_results],
        "limits": [
            "Instrumented diagnostic, not an FPS benchmark.",
            "Queue publication is bracketed; the consumer may run before push-end is logged.",
            "An unmatched boundary is unknown, not a zero-duration operation.",
            "The origin guest is the enclosing original IPC requester, not the HLE worker thread.",
            "The next batch origin does not prove it was the only possible earlier producer.",
            "Run and SVC elapsed spans are not exclusive host CPU execution time.",
            "OS CPU samples aggregate host/core windows, not individual guest functions.",
            "Overlapping waits, native work and frame stages must not be added together.",
            "Framebuffer release/acquire inside a wait are candidate edges, not automatic causal proof.",
            "Native submit timestamps are queue submission, not GPU completion timestamps.",
            "List work counts completed rasterizer operations, not all D3D12 calls or opaque native command-list bytes.",
            "Upload size is staging allocation traffic associated with the list's tick, not native command-list size.",
            "Scheduler-drained means no previous scheduler tick was unfinished at the sample; other queue users can still have work.",
            "Scheduler frame intervals group issued ticks between TickFrame boundaries, not native display presentation times.",
            "A native-signalled wait is linked by actual object wake and fence callback, not by timestamp proximity alone.",
            "Queue-idle overlap with such a wait is not automatically time that can safely be removed.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--capture-id", type=int)
    args = parser.parse_args()
    data = args.log.read_bytes()
    captures = [capture for capture in parse_captures(data.decode("utf-8-sig", errors="replace"))
                if "frame-chain-v1" in capture.mode and capture.events and
                (args.capture_id is None or capture.identity == args.capture_id)]
    if not captures:
        raise SystemExit("No matching frame-chain capture; focused legacy traces are not interchangeable")
    report = analyze(captures[-1])
    report["log_sha256"] = hashlib.sha256(data).hexdigest()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("capture", "events", "complete_dump",
        "capacity_limit_reached", "producer_guests", "issues", "summary")}, indent=2))


if __name__ == "__main__":
    main()
