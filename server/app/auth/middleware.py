# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Auth middleware — supports JWT Bearer tokens and legacy API keys.

During migration, both auth methods are accepted. JWT is preferred;
API key is a fallback for backward compatibility.
"""

import time
from collections import defaultdict

from fastapi import Request
from fastapi.responses import JSONResponse
from starlette.middleware.base import BaseHTTPMiddleware

from ..config import settings
from .jwt import decode_token

# Rate limit: track failed auth attempts per IP
_auth_failures: dict[str, list[float]] = defaultdict(list)
AUTH_FAIL_WINDOW = 300  # 5 minutes
AUTH_FAIL_MAX = 10  # max failures before lockout

# Paths that don't require auth
PUBLIC_PATHS = {"/", "/docs", "/openapi.json", "/redoc"}

# Device-facing paths (use device token or no auth for heartbeat)
DEVICE_PATH_PREFIXES = ["/api/device/"]
DEVICE_PATH_SUFFIXES = ["/status", "/download"]


class AuthMiddleware(BaseHTTPMiddleware):
    """Authenticate requests via JWT Bearer token or legacy API key."""

    async def dispatch(self, request: Request, call_next):
        path = request.url.path

        # Always allow: admin panel, static assets, docs
        if path in PUBLIC_PATHS or not path.startswith("/api/"):
            return await call_next(request)

        # Allow device-facing endpoints (they validate device_token internally)
        if any(path.startswith(p) for p in DEVICE_PATH_PREFIXES):
            return await call_next(request)
        if any(path.endswith(s) for s in DEVICE_PATH_SUFFIXES):
            return await call_next(request)

        client_ip = request.client.host if request.client else "unknown"

        # Check rate limit
        now = time.time()
        fails = _auth_failures[client_ip]
        fails[:] = [t for t in fails if now - t < AUTH_FAIL_WINDOW]
        if len(fails) >= AUTH_FAIL_MAX:
            return JSONResponse({"error": "rate limited"}, status_code=429)

        # Try JWT Bearer token first
        auth_header = request.headers.get("Authorization", "")
        if auth_header.startswith("Bearer "):
            token = auth_header[7:]
            claims = decode_token(token)
            if claims and claims.get("type") == "access":
                request.state.user = claims
                return await call_next(request)
            fails.append(now)
            return JSONResponse({"error": "invalid or expired token"}, status_code=401)

        # Fallback: legacy API key
        api_key = settings.api_key
        if api_key:
            key = (request.headers.get("X-API-Key") or
                   request.query_params.get("api_key"))
            if key == api_key:
                # Synthetic user claims for API key auth
                request.state.user = {
                    "sub": "api-key-user",
                    "email": "api@tritium.local",
                    "role": "super_admin",
                    "type": "access",
                }
                return await call_next(request)
            if key:  # Key provided but wrong
                fails.append(now)
                return JSONResponse({"error": "unauthorized"}, status_code=401)

        # No auth configured — allow in dev mode
        if not api_key and not settings.secret_key:
            request.state.user = {
                "sub": "dev-user",
                "email": "dev@tritium.local",
                "role": "super_admin",
                "type": "access",
            }
            return await call_next(request)

        return JSONResponse({"error": "authentication required"}, status_code=401)
