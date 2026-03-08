# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Fleet event correlation service.

Detects cross-device event patterns from diagnostic snapshots:
- Synchronized reboots (3+ devices within a time window)
- Cascading failures (errors propagating device-to-device in sequence)
- Environmental correlations (I2C/temperature issues across devices)
- Time-of-day periodic patterns (failures clustering at specific hours)

All functions are stateless — no global state, pure input/output.
"""

from collections import defaultdict
from datetime import datetime, timezone
from typing import Any

# Type alias for diagnostic snapshots
DiagSnapshot = dict[str, Any]
CorrelationEvent = dict[str, Any]

# --- Configuration constants ---

# Synchronized reboot detection
REBOOT_WINDOW_SECONDS = 300  # 5 minutes
REBOOT_MIN_DEVICES = 3

# Cascading failure detection
CASCADE_WINDOW_SECONDS = 600  # 10 minutes
CASCADE_MIN_DEVICES = 2

# Environmental correlation detection
ENVIRONMENTAL_WINDOW_SECONDS = 300  # 5 minutes
ENVIRONMENTAL_MIN_DEVICES = 2
ENVIRONMENTAL_EVENT_KEYWORDS = {"i2c", "temperature", "temp_spike", "sensor_fault", "bus_error"}

# Periodic failure detection
PERIODIC_HOUR_TOLERANCE = 1  # +/- 1 hour counts as "same time"
PERIODIC_MIN_OCCURRENCES = 3  # Need 3+ failures at the same hour


def _parse_timestamp(ts: str | float | datetime) -> datetime:
    """Parse a timestamp into a timezone-aware datetime.

    Accepts ISO 8601 strings, Unix epoch floats/ints, or datetime objects.
    Returns a UTC-aware datetime.
    """
    if isinstance(ts, datetime):
        if ts.tzinfo is None:
            return ts.replace(tzinfo=timezone.utc)
        return ts
    if isinstance(ts, (int, float)):
        return datetime.fromtimestamp(ts, tz=timezone.utc)
    # ISO string
    dt = datetime.fromisoformat(ts)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt


def _extract_events(snapshot: DiagSnapshot) -> list[dict]:
    """Extract the event list from a diagnostic snapshot.

    Snapshots may store events under 'events', 'anomalies', or as a flat
    list. Returns a list of event dicts, each guaranteed to have at least
    a 'type' and 'timestamp' key.
    """
    events = snapshot.get("events") or snapshot.get("anomalies") or []
    result = []
    for evt in events:
        if isinstance(evt, dict) and ("type" in evt or "subsystem" in evt):
            # Normalize: ensure 'type' key exists
            normalized = dict(evt)
            if "type" not in normalized and "subsystem" in normalized:
                normalized["type"] = normalized["subsystem"]
            if "timestamp" not in normalized:
                normalized["timestamp"] = snapshot.get("timestamp", "")
            result.append(normalized)
    return result


def detect_synchronized_reboots(
    snapshots: list[DiagSnapshot],
    window_seconds: int = REBOOT_WINDOW_SECONDS,
    min_devices: int = REBOOT_MIN_DEVICES,
) -> list[CorrelationEvent]:
    """Detect synchronized reboots across multiple devices.

    Scans all snapshots for reboot events and groups them by time window.
    If ``min_devices`` or more devices reboot within ``window_seconds``,
    a ``synchronized_reboot`` correlation event is emitted.

    Args:
        snapshots: List of diagnostic snapshot dicts. Each must contain
            ``device_id`` and ``timestamp``, plus ``events`` containing
            entries with ``type == "reboot"``.
        window_seconds: Maximum seconds between first and last reboot
            to consider them synchronized.
        min_devices: Minimum number of distinct devices required.

    Returns:
        List of CorrelationEvent dicts.
    """
    # Collect all reboot events with device and time
    reboots: list[tuple[str, datetime]] = []

    for snap in snapshots:
        device_id = snap.get("device_id", "unknown")
        events = _extract_events(snap)
        for evt in events:
            evt_type = evt.get("type", "").lower()
            if "reboot" in evt_type or "restart" in evt_type:
                ts = _parse_timestamp(evt.get("timestamp", snap.get("timestamp", "")))
                reboots.append((device_id, ts))

    if len(reboots) < min_devices:
        return []

    # Sort by timestamp
    reboots.sort(key=lambda r: r[1])

    # Sliding window to find clusters
    results: list[CorrelationEvent] = []
    used: set[int] = set()

    for i in range(len(reboots)):
        if i in used:
            continue
        cluster_devices: dict[str, datetime] = {reboots[i][0]: reboots[i][1]}
        cluster_indices = {i}

        for j in range(i + 1, len(reboots)):
            delta = (reboots[j][1] - reboots[i][1]).total_seconds()
            if delta <= window_seconds:
                cluster_devices[reboots[j][0]] = reboots[j][1]
                cluster_indices.add(j)
            else:
                break

        if len(cluster_devices) >= min_devices:
            used.update(cluster_indices)
            devices = sorted(cluster_devices.keys())
            earliest = min(cluster_devices.values())
            spread = (max(cluster_devices.values()) - earliest).total_seconds()
            confidence = min(1.0, len(cluster_devices) / (min_devices + 2))
            # Boost confidence if tightly clustered
            if spread < window_seconds * 0.3:
                confidence = min(1.0, confidence + 0.2)

            results.append({
                "type": "synchronized_reboot",
                "description": (
                    f"{len(cluster_devices)} devices rebooted within "
                    f"{spread:.0f}s window: {', '.join(devices)}"
                ),
                "devices_involved": devices,
                "confidence": round(confidence, 2),
                "timestamp": earliest.isoformat(),
            })

    return results


def detect_cascading_failures(
    snapshots: list[DiagSnapshot],
    window_seconds: int = CASCADE_WINDOW_SECONDS,
    min_devices: int = CASCADE_MIN_DEVICES,
) -> list[CorrelationEvent]:
    """Detect cascading failures propagating across devices.

    Looks for the same error type appearing on multiple devices in
    chronological sequence within ``window_seconds``. The sequential
    ordering (device A errors, then B, then C) distinguishes cascading
    failures from simultaneous environmental events.

    Args:
        snapshots: Diagnostic snapshots with ``device_id``, ``timestamp``,
            and ``events`` (each with ``type`` and ``timestamp``).
        window_seconds: Maximum time between first and last occurrence.
        min_devices: Minimum number of distinct devices in the chain.

    Returns:
        List of CorrelationEvent dicts with type ``cascading_failure``.
    """
    # Group error events by type
    errors_by_type: dict[str, list[tuple[str, datetime]]] = defaultdict(list)

    for snap in snapshots:
        device_id = snap.get("device_id", "unknown")
        events = _extract_events(snap)
        for evt in events:
            evt_type = evt.get("type", "").lower()
            if "error" in evt_type or "fault" in evt_type or "failure" in evt_type:
                ts = _parse_timestamp(evt.get("timestamp", snap.get("timestamp", "")))
                errors_by_type[evt_type].append((device_id, ts))

    results: list[CorrelationEvent] = []

    for error_type, occurrences in errors_by_type.items():
        # Sort by timestamp
        occurrences.sort(key=lambda o: o[1])

        # Deduplicate per device (keep earliest)
        seen_devices: dict[str, datetime] = {}
        ordered: list[tuple[str, datetime]] = []
        for device_id, ts in occurrences:
            if device_id not in seen_devices:
                seen_devices[device_id] = ts
                ordered.append((device_id, ts))

        if len(ordered) < min_devices:
            continue

        # Check if the chain fits within window
        spread = (ordered[-1][1] - ordered[0][1]).total_seconds()
        if spread > window_seconds:
            continue

        devices = [d for d, _ in ordered]
        first_ts = ordered[0][1]
        # Higher confidence when more devices and tighter timing
        confidence = min(1.0, len(devices) / (min_devices + 3))
        if spread > 0:
            avg_gap = spread / (len(devices) - 1)
            if avg_gap < 60:  # Less than 1 minute between devices
                confidence = min(1.0, confidence + 0.2)

        results.append({
            "type": "cascading_failure",
            "description": (
                f"'{error_type}' propagated across {len(devices)} devices "
                f"over {spread:.0f}s: {' -> '.join(devices)}"
            ),
            "devices_involved": devices,
            "confidence": round(confidence, 2),
            "timestamp": first_ts.isoformat(),
        })

    return results


def detect_environmental_correlation(
    snapshots: list[DiagSnapshot],
    window_seconds: int = ENVIRONMENTAL_WINDOW_SECONDS,
    min_devices: int = ENVIRONMENTAL_MIN_DEVICES,
    keywords: set[str] | None = None,
) -> list[CorrelationEvent]:
    """Detect environmental correlations across devices.

    Flags events when I2C errors, temperature spikes, or other
    environment-related issues occur on multiple devices within the
    same time window. Unlike cascading failures, the timing is
    simultaneous rather than sequential.

    Args:
        snapshots: Diagnostic snapshots with events.
        window_seconds: Time window for co-occurrence.
        min_devices: Minimum devices experiencing the same issue.
        keywords: Set of event-type keywords to consider environmental.
            Defaults to I2C, temperature, and sensor-related terms.

    Returns:
        List of CorrelationEvent dicts with type ``environmental``.
    """
    if keywords is None:
        keywords = ENVIRONMENTAL_EVENT_KEYWORDS

    # Collect environment-related events
    env_events: list[tuple[str, str, datetime]] = []  # (device_id, event_type, ts)

    for snap in snapshots:
        device_id = snap.get("device_id", "unknown")
        events = _extract_events(snap)
        for evt in events:
            evt_type = evt.get("type", "").lower()
            if any(kw in evt_type for kw in keywords):
                ts = _parse_timestamp(evt.get("timestamp", snap.get("timestamp", "")))
                env_events.append((device_id, evt_type, ts))

    if not env_events:
        return []

    # Group by event type keyword
    groups: dict[str, list[tuple[str, datetime]]] = defaultdict(list)
    for device_id, evt_type, ts in env_events:
        # Use the matching keyword as the group key
        for kw in keywords:
            if kw in evt_type:
                groups[kw].append((device_id, ts))

    results: list[CorrelationEvent] = []

    for keyword, occurrences in groups.items():
        occurrences.sort(key=lambda o: o[1])

        # Find clusters within window
        for i in range(len(occurrences)):
            cluster: dict[str, datetime] = {occurrences[i][0]: occurrences[i][1]}

            for j in range(i + 1, len(occurrences)):
                delta = (occurrences[j][1] - occurrences[i][1]).total_seconds()
                if delta <= window_seconds:
                    if occurrences[j][0] not in cluster:
                        cluster[occurrences[j][0]] = occurrences[j][1]
                else:
                    break

            if len(cluster) >= min_devices:
                devices = sorted(cluster.keys())
                earliest = min(cluster.values())
                spread = (max(cluster.values()) - earliest).total_seconds()
                # Tighter clustering = higher confidence
                time_ratio = 1.0 - (spread / window_seconds) if window_seconds > 0 else 1.0
                confidence = min(1.0, 0.5 + 0.3 * time_ratio + 0.1 * len(devices) / 5)

                event = {
                    "type": "environmental",
                    "description": (
                        f"'{keyword}' events on {len(devices)} devices within "
                        f"{spread:.0f}s: {', '.join(devices)}"
                    ),
                    "devices_involved": devices,
                    "confidence": round(confidence, 2),
                    "timestamp": earliest.isoformat(),
                }

                # Avoid duplicate detections for same device set
                existing = {
                    tuple(r["devices_involved"]) for r in results
                    if r["description"].startswith(f"'{keyword}'")
                }
                if tuple(devices) not in existing:
                    results.append(event)

    return results


def detect_periodic_failures(
    snapshots: list[DiagSnapshot],
    hour_tolerance: int = PERIODIC_HOUR_TOLERANCE,
    min_occurrences: int = PERIODIC_MIN_OCCURRENCES,
) -> list[CorrelationEvent]:
    """Detect time-of-day periodic failure patterns.

    Groups all error/failure events by hour-of-day and flags when
    a specific hour window accumulates ``min_occurrences`` or more
    failures across any devices. This catches patterns like "always
    fails at noon" or "nightly reboot storms."

    Args:
        snapshots: Diagnostic snapshots with events.
        hour_tolerance: Hours +/- to consider as the same time slot.
        min_occurrences: Minimum failures in an hour bucket to flag.

    Returns:
        List of CorrelationEvent dicts with type ``periodic_failure``.
    """
    # Collect all failure events with timestamps
    failures: list[tuple[str, datetime, str]] = []  # (device_id, ts, event_type)

    for snap in snapshots:
        device_id = snap.get("device_id", "unknown")
        events = _extract_events(snap)
        for evt in events:
            evt_type = evt.get("type", "").lower()
            if any(kw in evt_type for kw in ("error", "fault", "failure", "reboot", "crash")):
                ts = _parse_timestamp(evt.get("timestamp", snap.get("timestamp", "")))
                failures.append((device_id, ts, evt_type))

    if len(failures) < min_occurrences:
        return []

    # Bucket by hour of day (0-23)
    hour_buckets: dict[int, list[tuple[str, datetime, str]]] = defaultdict(list)
    for device_id, ts, evt_type in failures:
        hour_buckets[ts.hour].append((device_id, ts, evt_type))

    # Merge adjacent hours within tolerance
    results: list[CorrelationEvent] = []
    visited_hours: set[int] = set()

    for hour in sorted(hour_buckets.keys()):
        if hour in visited_hours:
            continue

        # Collect events from this hour and neighbors within tolerance
        merged: list[tuple[str, datetime, str]] = []
        hours_included: list[int] = []

        for h in range(hour - hour_tolerance, hour + hour_tolerance + 1):
            normalized = h % 24
            if normalized in hour_buckets:
                merged.extend(hour_buckets[normalized])
                hours_included.append(normalized)
                visited_hours.add(normalized)

        if len(merged) < min_occurrences:
            continue

        devices = sorted(set(d for d, _, _ in merged))
        event_types = sorted(set(t for _, _, t in merged))
        center_hour = hour
        # Confidence based on how many events and how many distinct days
        dates = set(ts.date() for _, ts, _ in merged)
        day_count = len(dates)
        confidence = min(1.0, 0.4 + 0.15 * day_count + 0.05 * len(merged))

        results.append({
            "type": "periodic_failure",
            "description": (
                f"{len(merged)} failures clustered around "
                f"{center_hour:02d}:00 UTC across {len(devices)} device(s) "
                f"over {day_count} day(s): {', '.join(event_types)}"
            ),
            "devices_involved": devices,
            "confidence": round(confidence, 2),
            "timestamp": min(ts for _, ts, _ in merged).isoformat(),
        })

    return results


def correlate_events(snapshots: list[DiagSnapshot]) -> list[CorrelationEvent]:
    """Run all correlation detectors against a set of diagnostic snapshots.

    This is the main entry point. Pass a list of diagnostic snapshot dicts
    (each containing ``device_id``, ``timestamp``, and ``events``) and
    receive back a list of detected cross-device patterns.

    Args:
        snapshots: List of diagnostic snapshot dicts. Each should have:
            - ``device_id`` (str): Unique device identifier.
            - ``timestamp`` (str|float): When the snapshot was captured.
            - ``events`` (list[dict]): Events, each with ``type`` and
              ``timestamp`` keys.

    Returns:
        List of CorrelationEvent dicts, each containing:
            - ``type``: One of ``synchronized_reboot``, ``cascading_failure``,
              ``environmental``, ``periodic_failure``.
            - ``description``: Human-readable summary.
            - ``devices_involved``: List of device IDs.
            - ``confidence``: Float 0.0-1.0.
            - ``timestamp``: ISO 8601 timestamp of earliest related event.
    """
    results: list[CorrelationEvent] = []
    results.extend(detect_synchronized_reboots(snapshots))
    results.extend(detect_cascading_failures(snapshots))
    results.extend(detect_environmental_correlation(snapshots))
    results.extend(detect_periodic_failures(snapshots))

    # Sort by timestamp
    results.sort(key=lambda r: r.get("timestamp", ""))
    return results
