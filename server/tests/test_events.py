# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the event log endpoints."""


def test_list_events_empty(client):
    """Events returns empty list when no events exist."""
    r = client.get("/api/events")
    assert r.status_code == 200
    assert r.json() == []


def test_list_events_after_action(client, sample_device):
    """Events are recorded after device actions."""
    # Delete a device to generate an event
    client.delete("/api/devices/test-node-001")

    r = client.get("/api/events")
    assert r.status_code == 200
    events = r.json()
    assert len(events) >= 1
    assert any(e["type"] == "device_deleted" for e in events)


def test_list_events_filter_by_device(client, sample_device, store):
    """Events can be filtered by device_id."""
    store.add_event("test_event", "test-node-001", "something happened")
    store.add_event("other_event", "other-device", "unrelated")

    r = client.get("/api/events?device_id=test-node-001")
    assert r.status_code == 200
    events = r.json()
    assert all(e["device_id"] == "test-node-001" for e in events)


def test_list_events_with_limit(client, store):
    """Events limit parameter caps results."""
    for i in range(10):
        store.add_event("test_event", detail=f"event {i}")

    r = client.get("/api/events?limit=3")
    assert r.status_code == 200
    assert len(r.json()) == 3
