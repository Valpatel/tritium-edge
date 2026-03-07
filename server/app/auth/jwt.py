# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""JWT token creation and validation.

Designed for extraction into tritium-lib as shared auth utilities.
"""

import time
import uuid
from typing import Optional

import jwt

from ..config import settings

# Algorithm
ALGORITHM = "HS256"


def _get_secret() -> str:
    """Get or generate JWT signing secret."""
    if settings.secret_key:
        return settings.secret_key
    # Fallback: derive from api_key or generate ephemeral
    return settings.api_key or "tritium-edge-dev-secret"


def create_access_token(
    user_id: str,
    email: str,
    role: str = "user",
    org_id: Optional[str] = None,
) -> str:
    """Create short-lived access token (default 15 min)."""
    now = int(time.time())
    payload = {
        "sub": user_id,
        "email": email,
        "role": role,
        "type": "access",
        "iat": now,
        "exp": now + (settings.access_token_expire_minutes * 60),
        "jti": uuid.uuid4().hex[:12],
    }
    if org_id:
        payload["org_id"] = org_id
    return jwt.encode(payload, _get_secret(), algorithm=ALGORITHM)


def create_refresh_token(user_id: str) -> str:
    """Create long-lived refresh token (default 7 days)."""
    now = int(time.time())
    payload = {
        "sub": user_id,
        "type": "refresh",
        "iat": now,
        "exp": now + (settings.refresh_token_expire_days * 86400),
        "jti": uuid.uuid4().hex[:12],
    }
    return jwt.encode(payload, _get_secret(), algorithm=ALGORITHM)


def create_device_token(device_id: str, org_id: str) -> str:
    """Create device token (default 365 days)."""
    now = int(time.time())
    payload = {
        "sub": device_id,
        "org_id": org_id,
        "type": "device",
        "iat": now,
        "exp": now + (settings.device_token_expire_days * 86400),
        "jti": uuid.uuid4().hex[:12],
    }
    return jwt.encode(payload, _get_secret(), algorithm=ALGORITHM)


def decode_token(token: str) -> Optional[dict]:
    """Decode and validate a JWT token. Returns claims or None."""
    try:
        return jwt.decode(token, _get_secret(), algorithms=[ALGORITHM])
    except (jwt.ExpiredSignatureError, jwt.InvalidTokenError):
        return None
