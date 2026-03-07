# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""WebSocket router — real-time event streaming to admin portal."""

import asyncio
import json
from datetime import datetime, timezone
from typing import Set

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

router = APIRouter()

# Connected WebSocket clients
_clients: Set[WebSocket] = set()


async def broadcast(event_type: str, data: dict):
    """Broadcast an event to all connected WebSocket clients."""
    global _clients
    if not _clients:
        return
    message = json.dumps({
        "type": event_type,
        "data": data,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    })
    dead = set()
    for ws in _clients:
        try:
            await ws.send_text(message)
        except Exception:
            dead.add(ws)
    _clients -= dead


@router.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    """WebSocket connection for real-time admin updates.

    Events pushed to clients:
      - device_heartbeat: A device sent a heartbeat
      - device_registered: A new device auto-registered
      - device_offline: A device went offline (missed heartbeats)
      - ota_started: OTA push initiated
      - ota_result: OTA completed (success/fail)
      - firmware_uploaded: New firmware uploaded
      - command_sent: Command dispatched to device
    """
    await websocket.accept()
    _clients.add(websocket)
    try:
        while True:
            # Keep connection alive; ignore client messages for now
            data = await websocket.receive_text()
            if data == "ping":
                await websocket.send_text(json.dumps({"type": "pong"}))
    except WebSocketDisconnect:
        pass
    finally:
        _clients.discard(websocket)


@router.websocket("/ws/serial")
async def serial_monitor(websocket: WebSocket, port: str = "/dev/ttyACM1", baud: int = 115200):
    """WebSocket serial monitor — streams USB serial output to browser."""
    import serial

    await websocket.accept()
    ser = None
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
        await websocket.send_text(json.dumps({
            "type": "connected", "port": port, "baud": baud
        }))

        while True:
            # Read from serial if available
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                try:
                    text = data.decode("utf-8", errors="replace")
                except Exception:
                    text = data.hex()
                await websocket.send_text(json.dumps({
                    "type": "data", "text": text
                }))

            # Check for commands from browser
            try:
                msg = await asyncio.wait_for(websocket.receive_text(), timeout=0.05)
                if msg == "ping":
                    await websocket.send_text(json.dumps({"type": "pong"}))
                else:
                    # Send text to serial port
                    ser.write((msg + "\n").encode())
            except asyncio.TimeoutError:
                pass

    except WebSocketDisconnect:
        pass
    except serial.SerialException as e:
        try:
            await websocket.send_text(json.dumps({
                "type": "error", "message": str(e)
            }))
        except Exception:
            pass
    finally:
        if ser and ser.is_open:
            ser.close()
