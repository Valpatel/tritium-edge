# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Configuration management using Pydantic settings."""

from pathlib import Path
from typing import Optional

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Application settings loaded from environment variables."""

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        case_sensitive=False,
        extra="ignore",
    )

    # Application
    app_name: str = "Tritium-Edge"
    debug: bool = False

    # Server
    host: str = "localhost"
    port: int = 8080

    # Data
    data_dir: Path = Path("./fleet_data")

    # Auth
    secret_key: str = ""  # JWT signing key — auto-generated if empty
    api_key: str = ""  # Legacy API key — empty disables
    admin_email: str = "admin@tritium.local"
    admin_password: str = ""  # Set on first boot

    # JWT
    access_token_expire_minutes: int = 15
    refresh_token_expire_days: int = 7
    device_token_expire_days: int = 365

    # TLS
    ssl_cert: Optional[str] = None
    ssl_key: Optional[str] = None

    # Plugins
    plugins_enabled: bool = True
    plugins_dir: str = "plugins"

    # Fleet
    max_devices_per_org: int = 1000
    heartbeat_timeout_s: int = 120
    device_registration_limit: int = 100

    # MQTT bridge (for tritium-sc integration)
    mqtt_enabled: bool = False
    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_site_id: str = "home"


settings = Settings()
