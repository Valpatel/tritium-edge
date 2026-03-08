# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for fleet event correlation service."""

from datetime import datetime, timezone, timedelta

from app.services.correlation_service import (
    correlate_events,
    detect_synchronized_reboots,
    detect_cascading_failures,
    detect_environmental_correlation,
    detect_periodic_failures,
)


# --- Helpers ---

def _ts(base: datetime, offset_seconds: int = 0) -> str:
    """Return ISO timestamp offset from base by N seconds."""
    return (base + timedelta(seconds=offset_seconds)).isoformat()


BASE_TIME = datetime(2026, 3, 8, 12, 0, 0, tzinfo=timezone.utc)


def _make_snapshot(device_id: str, events: list[dict], ts: str | None = None) -> dict:
    """Build a diagnostic snapshot dict."""
    return {
        "device_id": device_id,
        "timestamp": ts or BASE_TIME.isoformat(),
        "events": events,
    }


# --- Synchronized Reboot Tests ---

class TestSynchronizedReboots:
    """Tests for detect_synchronized_reboots."""

    def test_three_devices_reboot_within_window(self):
        """3 devices rebooting within 5 minutes triggers detection."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 60)},
            ]),
            _make_snapshot("node-C", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 120)},
            ]),
        ]
        results = detect_synchronized_reboots(snapshots)
        assert len(results) == 1
        assert results[0]["type"] == "synchronized_reboot"
        assert sorted(results[0]["devices_involved"]) == ["node-A", "node-B", "node-C"]
        assert 0 < results[0]["confidence"] <= 1.0

    def test_two_devices_below_threshold(self):
        """Only 2 devices rebooting does not trigger (min is 3)."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 30)},
            ]),
        ]
        results = detect_synchronized_reboots(snapshots)
        assert len(results) == 0

    def test_reboots_outside_window(self):
        """Reboots spread over hours do not trigger."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 3600)},
            ]),
            _make_snapshot("node-C", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 7200)},
            ]),
        ]
        results = detect_synchronized_reboots(snapshots)
        assert len(results) == 0

    def test_restart_variant_detected(self):
        """Events with 'restart' in type are also caught."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "device_restart", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 10)},
            ]),
            _make_snapshot("node-C", [
                {"type": "system_restart", "timestamp": _ts(BASE_TIME, 20)},
            ]),
        ]
        results = detect_synchronized_reboots(snapshots)
        assert len(results) == 1

    def test_tight_cluster_higher_confidence(self):
        """Reboots within seconds should have higher confidence than spread ones."""
        tight = [
            _make_snapshot("node-A", [{"type": "reboot", "timestamp": _ts(BASE_TIME, 0)}]),
            _make_snapshot("node-B", [{"type": "reboot", "timestamp": _ts(BASE_TIME, 2)}]),
            _make_snapshot("node-C", [{"type": "reboot", "timestamp": _ts(BASE_TIME, 5)}]),
        ]
        spread = [
            _make_snapshot("node-A", [{"type": "reboot", "timestamp": _ts(BASE_TIME, 0)}]),
            _make_snapshot("node-B", [{"type": "reboot", "timestamp": _ts(BASE_TIME, 200)}]),
            _make_snapshot("node-C", [{"type": "reboot", "timestamp": _ts(BASE_TIME, 290)}]),
        ]
        tight_results = detect_synchronized_reboots(tight)
        spread_results = detect_synchronized_reboots(spread)
        assert tight_results[0]["confidence"] >= spread_results[0]["confidence"]

    def test_no_events(self):
        """Empty snapshots produce no results."""
        results = detect_synchronized_reboots([])
        assert results == []


# --- Cascading Failure Tests ---

class TestCascadingFailures:
    """Tests for detect_cascading_failures."""

    def test_error_propagates_across_devices(self):
        """Same error type appearing on devices in sequence is detected."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 30)},
            ]),
            _make_snapshot("node-C", [
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 90)},
            ]),
        ]
        results = detect_cascading_failures(snapshots)
        assert len(results) == 1
        assert results[0]["type"] == "cascading_failure"
        # Order should reflect propagation sequence
        assert results[0]["devices_involved"] == ["node-A", "node-B", "node-C"]

    def test_different_error_types_not_merged(self):
        """Different error types are treated separately."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "wifi_failure", "timestamp": _ts(BASE_TIME, 30)},
            ]),
        ]
        results = detect_cascading_failures(snapshots)
        assert len(results) == 0

    def test_single_device_no_cascade(self):
        """Same error on one device is not a cascade."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 0)},
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 30)},
            ]),
        ]
        results = detect_cascading_failures(snapshots)
        assert len(results) == 0

    def test_errors_outside_window(self):
        """Errors spread beyond the window are not flagged."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "bus_error", "timestamp": _ts(BASE_TIME, 7200)},
            ]),
        ]
        results = detect_cascading_failures(snapshots)
        assert len(results) == 0


# --- Environmental Correlation Tests ---

class TestEnvironmentalCorrelation:
    """Tests for detect_environmental_correlation."""

    def test_i2c_errors_across_devices(self):
        """I2C errors on multiple devices within window are flagged."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "i2c_error", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "i2c_error", "timestamp": _ts(BASE_TIME, 10)},
            ]),
        ]
        results = detect_environmental_correlation(snapshots)
        assert len(results) == 1
        assert results[0]["type"] == "environmental"
        assert sorted(results[0]["devices_involved"]) == ["node-A", "node-B"]

    def test_temperature_spike_detected(self):
        """Temperature spike events across devices are flagged."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "temp_spike", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "temp_spike", "timestamp": _ts(BASE_TIME, 5)},
            ]),
            _make_snapshot("node-C", [
                {"type": "temp_spike", "timestamp": _ts(BASE_TIME, 15)},
            ]),
        ]
        results = detect_environmental_correlation(snapshots)
        assert len(results) >= 1
        assert results[0]["type"] == "environmental"
        assert len(results[0]["devices_involved"]) >= 2

    def test_non_environmental_events_ignored(self):
        """Regular errors are not flagged as environmental."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "wifi_disconnect", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "wifi_disconnect", "timestamp": _ts(BASE_TIME, 5)},
            ]),
        ]
        results = detect_environmental_correlation(snapshots)
        assert len(results) == 0

    def test_single_device_not_flagged(self):
        """Environmental event on one device alone is not correlated."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "i2c_error", "timestamp": _ts(BASE_TIME, 0)},
            ]),
        ]
        results = detect_environmental_correlation(snapshots)
        assert len(results) == 0

    def test_custom_keywords(self):
        """Custom keyword set is respected."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "humidity_alert", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "humidity_alert", "timestamp": _ts(BASE_TIME, 5)},
            ]),
        ]
        # Default keywords won't match
        assert detect_environmental_correlation(snapshots) == []
        # Custom keywords will
        results = detect_environmental_correlation(snapshots, keywords={"humidity"})
        assert len(results) == 1


# --- Periodic Failure Tests ---

class TestPeriodicFailures:
    """Tests for detect_periodic_failures."""

    def test_failures_at_same_hour(self):
        """Failures clustering at noon across multiple days are flagged."""
        noon_day1 = datetime(2026, 3, 5, 12, 15, 0, tzinfo=timezone.utc)
        noon_day2 = datetime(2026, 3, 6, 12, 30, 0, tzinfo=timezone.utc)
        noon_day3 = datetime(2026, 3, 7, 12, 5, 0, tzinfo=timezone.utc)

        snapshots = [
            _make_snapshot("node-A", [
                {"type": "crash_error", "timestamp": noon_day1.isoformat()},
            ], ts=noon_day1.isoformat()),
            _make_snapshot("node-B", [
                {"type": "crash_error", "timestamp": noon_day2.isoformat()},
            ], ts=noon_day2.isoformat()),
            _make_snapshot("node-A", [
                {"type": "crash_error", "timestamp": noon_day3.isoformat()},
            ], ts=noon_day3.isoformat()),
        ]
        results = detect_periodic_failures(snapshots)
        assert len(results) >= 1
        assert results[0]["type"] == "periodic_failure"
        assert "12" in results[0]["description"]

    def test_scattered_failures_not_periodic(self):
        """Failures at very different hours are not flagged."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "crash_error",
                 "timestamp": datetime(2026, 3, 5, 3, 0, tzinfo=timezone.utc).isoformat()},
            ]),
            _make_snapshot("node-B", [
                {"type": "crash_error",
                 "timestamp": datetime(2026, 3, 6, 15, 0, tzinfo=timezone.utc).isoformat()},
            ]),
        ]
        results = detect_periodic_failures(snapshots)
        assert len(results) == 0

    def test_below_occurrence_threshold(self):
        """Fewer than min_occurrences failures at same hour are ignored."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "crash_error",
                 "timestamp": datetime(2026, 3, 5, 8, 0, tzinfo=timezone.utc).isoformat()},
            ]),
            _make_snapshot("node-B", [
                {"type": "crash_error",
                 "timestamp": datetime(2026, 3, 6, 8, 30, tzinfo=timezone.utc).isoformat()},
            ]),
        ]
        results = detect_periodic_failures(snapshots, min_occurrences=3)
        assert len(results) == 0

    def test_subsystem_as_type_fallback(self):
        """Events with 'subsystem' key instead of 'type' are normalized."""
        noon_day1 = datetime(2026, 3, 5, 14, 0, 0, tzinfo=timezone.utc)
        noon_day2 = datetime(2026, 3, 6, 14, 0, 0, tzinfo=timezone.utc)
        noon_day3 = datetime(2026, 3, 7, 14, 0, 0, tzinfo=timezone.utc)

        snapshots = [
            _make_snapshot("node-A", [
                {"subsystem": "reboot_error", "timestamp": noon_day1.isoformat()},
            ], ts=noon_day1.isoformat()),
            _make_snapshot("node-B", [
                {"subsystem": "reboot_error", "timestamp": noon_day2.isoformat()},
            ], ts=noon_day2.isoformat()),
            _make_snapshot("node-A", [
                {"subsystem": "reboot_error", "timestamp": noon_day3.isoformat()},
            ], ts=noon_day3.isoformat()),
        ]
        results = detect_periodic_failures(snapshots)
        assert len(results) >= 1


# --- Integration: correlate_events ---

class TestCorrelateEvents:
    """Tests for the top-level correlate_events function."""

    def test_returns_all_types(self):
        """correlate_events runs all detectors and returns combined results."""
        # Build snapshots that trigger multiple detectors
        snapshots = [
            # Synchronized reboots (3 within seconds)
            _make_snapshot("node-A", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 5)},
            ]),
            _make_snapshot("node-C", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 10)},
            ]),
            # Environmental (I2C errors)
            _make_snapshot("node-D", [
                {"type": "i2c_error", "timestamp": _ts(BASE_TIME, 100)},
            ]),
            _make_snapshot("node-E", [
                {"type": "i2c_error", "timestamp": _ts(BASE_TIME, 105)},
            ]),
        ]
        results = correlate_events(snapshots)
        types_found = {r["type"] for r in results}
        assert "synchronized_reboot" in types_found
        assert "environmental" in types_found

    def test_empty_input(self):
        """correlate_events with no snapshots returns empty list."""
        assert correlate_events([]) == []

    def test_result_structure(self):
        """Every result has the required keys."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 5)},
            ]),
            _make_snapshot("node-C", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 10)},
            ]),
        ]
        results = correlate_events(snapshots)
        for r in results:
            assert "type" in r
            assert "description" in r
            assert "devices_involved" in r
            assert "confidence" in r
            assert "timestamp" in r
            assert 0 <= r["confidence"] <= 1.0
            assert isinstance(r["devices_involved"], list)

    def test_results_sorted_by_timestamp(self):
        """Results are sorted by timestamp."""
        snapshots = [
            _make_snapshot("node-A", [
                {"type": "i2c_error", "timestamp": _ts(BASE_TIME, 500)},
            ]),
            _make_snapshot("node-B", [
                {"type": "i2c_error", "timestamp": _ts(BASE_TIME, 510)},
            ]),
            _make_snapshot("node-A", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 0)},
            ]),
            _make_snapshot("node-B", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 5)},
            ]),
            _make_snapshot("node-C", [
                {"type": "reboot", "timestamp": _ts(BASE_TIME, 10)},
            ]),
        ]
        results = correlate_events(snapshots)
        timestamps = [r["timestamp"] for r in results]
        assert timestamps == sorted(timestamps)
