# Multi-Tenant Architecture

This document defines the multi-tenant model for Tritium-Edge: organizations, users,
roles, and access control. The design supports a single admin managing the entire
platform while friends and collaborators each get isolated environments to manage
their own ESP32 devices independently.

---

## 1. Tenant Model

The **organization** is the top-level tenant container. All domain objects are scoped
to exactly one organization:

- Devices
- Firmware images
- Product profiles
- Events and telemetry
- Configuration and settings

Isolation rules:

| Principle | Description |
|---|---|
| Org-scoped data | Every record belongs to exactly one org. No cross-org data leakage. |
| Super admin override | A super admin can read and manage all orgs transparently. |
| Org admin autonomy | An org admin has full control within their own org boundary. |
| User visibility | Regular users see only the org(s) they belong to. |

The same physical hardware (e.g., an ESP32-S3-Touch-LCD-3.5B-C) can be registered in
different orgs at different times and configured with completely different product
profiles. A device belongs to exactly one org at any given time.

---

## 2. Organization

### Schema

| Field | Type | Description |
|---|---|---|
| `id` | UUID | Primary key |
| `name` | string | Human-readable name (e.g., "Bob's Garage") |
| `slug` | string | URL-safe identifier, unique (e.g., `bobs-garage`) |
| `description` | string | Optional description |
| `settings` | object | Org-level configuration (see below) |
| `created_at` | datetime | Creation timestamp |
| `updated_at` | datetime | Last modification timestamp |

### Organization Settings

```json
{
  "require_signed_firmware": false,
  "heartbeat_interval_s": 60,
  "auto_rollback": true,
  "max_devices": 50
}
```

| Setting | Default | Description |
|---|---|---|
| `require_signed_firmware` | `false` | Reject unsigned firmware uploads |
| `heartbeat_interval_s` | `60` | Expected device check-in interval in seconds |
| `auto_rollback` | `true` | Automatically roll back failed OTA updates |
| `max_devices` | `50` | Maximum devices allowed in this org |

### Default Organization

On first boot (fresh install or migration from flat data), a `default` organization
is created automatically. All pre-existing devices are assigned to it. See
Section 8 for the full migration procedure.

---

## 3. Users

### Schema

| Field | Type | Description |
|---|---|---|
| `id` | UUID | Primary key |
| `email` | string | Unique, used for login |
| `name` | string | Display name |
| `password_hash` | string | bcrypt hash, minimum cost factor 12 |
| `memberships` | array | List of org memberships (see below) |
| `is_super_admin` | boolean | Platform-wide admin flag |
| `created_at` | datetime | Account creation timestamp |
| `last_login` | datetime | Most recent login timestamp |

### Membership

A user can belong to multiple organizations, each with a distinct role:

```json
{
  "org_id": "uuid-of-org",
  "role": "org_admin",
  "joined_at": "2026-03-07T12:00:00Z"
}
```

| Field | Type | Description |
|---|---|---|
| `org_id` | UUID | Reference to organization |
| `role` | enum | One of: `org_admin`, `user` |
| `joined_at` | datetime | When the membership was created |

### First User

The first user created on the platform is automatically granted `is_super_admin = true`.
This user is seeded from environment variables during initial setup:

```
ADMIN_EMAIL=admin@example.com
ADMIN_PASSWORD=<strong-password>
ADMIN_NAME=Admin
```

---

## 4. Roles

Three roles exist in the system. `super_admin` is a platform-level flag on the user
record. `org_admin` and `user` are per-org membership roles.

| Role | Scope | Summary |
|---|---|---|
| `super_admin` | Platform-wide | Full access to all orgs, users, and system settings |
| `org_admin` | Single org | Full access within their org |
| `user` | Single org | Read-only access with limited actions |

### Permissions Matrix

| Action | `super_admin` | `org_admin` | `user` |
|---|:---:|:---:|:---:|
| **Organizations** | | | |
| Create org | Yes | No | No |
| Delete org | Yes | No | No |
| Edit org settings | Yes | Own org | No |
| List all orgs | Yes | No | No |
| View own org | Yes | Yes | Yes |
| **Users** | | | |
| Create user (any org) | Yes | No | No |
| Create user (own org) | Yes | Yes | No |
| Remove user from org | Yes | Own org | No |
| Change user role | Yes | Own org | No |
| Delete user | Yes | No | No |
| View org members | Yes | Own org | Own org |
| **Devices** | | | |
| Register device | Yes | Own org | No |
| Remove device | Yes | Own org | No |
| View devices | Yes | Own org | Own org |
| Send command to device | Yes | Own org | No |
| Move device between orgs | Yes | No | No |
| **Firmware** | | | |
| Upload firmware | Yes | Own org | No |
| Delete firmware | Yes | Own org | No |
| Push firmware to device | Yes | Own org | No |
| View firmware list | Yes | Own org | Own org |
| Broadcast firmware (all orgs) | Yes | No | No |
| **Product Profiles** | | | |
| Create/edit profile | Yes | Own org | No |
| Delete profile | Yes | Own org | No |
| Assign profile to device | Yes | Own org | No |
| View profiles | Yes | Own org | Own org |
| **Events and Telemetry** | | | |
| View events | Yes | Own org | Own org |
| Export events | Yes | Own org | No |
| View cross-org dashboard | Yes | No | No |

---

## 5. Data Isolation

### API Route Structure

All resource endpoints are scoped under the organization:

```
GET    /api/orgs                          # List orgs (super_admin: all, others: own)
POST   /api/orgs                          # Create org (super_admin only)
GET    /api/orgs/{org_id}                 # Org details
PUT    /api/orgs/{org_id}                 # Update org settings
DELETE /api/orgs/{org_id}                 # Delete org (super_admin only)

GET    /api/orgs/{org_id}/devices         # List devices
POST   /api/orgs/{org_id}/devices         # Register device
GET    /api/orgs/{org_id}/devices/{id}    # Device details
DELETE /api/orgs/{org_id}/devices/{id}    # Remove device

POST   /api/orgs/{org_id}/firmware        # Upload firmware
GET    /api/orgs/{org_id}/firmware         # List firmware
POST   /api/orgs/{org_id}/firmware/{id}/push  # Push to device(s)

GET    /api/orgs/{org_id}/profiles        # List product profiles
POST   /api/orgs/{org_id}/profiles        # Create profile
GET    /api/orgs/{org_id}/events          # Query events

GET    /api/orgs/{org_id}/members         # List org members
POST   /api/orgs/{org_id}/members         # Add member (invite or direct)
DELETE /api/orgs/{org_id}/members/{id}    # Remove member
```

### Middleware Enforcement

Every org-scoped request passes through authorization middleware that validates:

1. The authenticated user has a membership in the target `org_id`, OR the user
   is a `super_admin`.
2. The user's role in that org meets the minimum required role for the action.
3. The requested resource (device, firmware, profile) actually belongs to `org_id`.

If any check fails, the server returns `403 Forbidden` with no data leakage.

### Isolation Guarantees

- A device belongs to exactly one org. It cannot be queried or commanded from
  another org.
- Firmware uploaded to org A cannot be pushed to devices in org B. Cross-org
  firmware broadcast is a super_admin-only operation that copies firmware into
  each target org.
- Events generated by a device are stored under the device's org. Org B cannot
  query org A's event stream.
- Product profiles are org-scoped. The same profile name can exist in different
  orgs with entirely different configurations.

---

## 6. Invitation Flow

### Full Flow (Future)

1. Org admin creates an invitation specifying email address and role.
2. Server generates a single-use token with 72-hour expiry.
3. Invitee receives an email with a link: `/invite/{token}`.
4. Invitee creates a new account or links an existing account.
5. Membership is added to the org with the specified role.
6. Token is consumed and cannot be reused.

### Simplified Flow (Initial Implementation)

For the initial release, org admins create users directly:

```
POST /api/orgs/{org_id}/members
{
  "email": "bob@example.com",
  "name": "Bob",
  "password": "temporary-password",
  "role": "user"
}
```

The server creates the user account (if the email is new) and adds an org
membership. The user should change their password on first login.

If the email already belongs to an existing user, only the membership is added.
The password field is ignored in that case.

---

## 7. User Scenarios

### Scenario A: Admin with Multiple Projects

The admin manages three organizations, each representing a different project:

| Org | Devices | Product Profile | Use Case |
|---|---|---|---|
| Home Security | 2x 3.5B-C, 1x 4.3C-BOX | Camera + motion detection | Surveillance cameras |
| Workshop Sensors | 2x AMOLED-2.41-B, 1x LCD-3.49 | Temp/humidity + dashboard | Environmental monitoring |
| Kids' Science | 1x AMOLED-1.8, 1x LCD-3.49 | LED effects + IMU games | Educational experiments |

The same physical boards can be reassigned between orgs as projects change. The
admin switches between orgs in the portal using an org selector in the navigation
bar. Each org has independent firmware, profiles, and event history.

### Scenario B: Friend with Their Own Fleet

1. Admin creates org "Bob's Garage" via the portal.
2. Admin creates a user account for Bob with the `user` role.
3. Bob logs in and sees only "Bob's Garage" in his org list.
4. Bob can view his devices, check configurations, and browse event history.
5. Bob cannot upload firmware or modify profiles (read-only `user` role).
6. Bob asks the admin to upload new firmware for his devices.
7. When Bob is ready to self-manage, the admin upgrades him to `org_admin`.
8. Bob can now upload firmware, create profiles, and manage his own devices.

### Scenario C: Super Admin Overview

The super admin dashboard provides a cross-org view:

- **Org summary**: Total orgs, total devices, devices online/offline per org.
- **Fleet health**: Firmware version distribution across all orgs, devices with
  failed OTA, devices missing heartbeats.
- **Drill-down**: Click any org to view it as if logged in as an org admin.
- **Broadcast operations**: Push a firmware update to all orgs simultaneously.
  The firmware is copied into each org's namespace and pushed to their devices.
- **Compliance**: Identify orgs running outdated firmware or with unsigned images
  when `require_signed_firmware` is enforced.

---

## 8. Migration from Flat Data

The current `fleet_data/` directory stores devices, firmware, and events with no
org concept. The migration runs automatically on first boot when the server detects
no organizations exist.

### Migration Steps

| Step | Action | Details |
|---|---|---|
| 1 | Create default org | Name: "Default", slug: `default`. All settings use defaults. |
| 2 | Move devices | All existing device records get `org_id` set to the default org. |
| 3 | Move firmware | All firmware images are assigned to the default org. |
| 4 | Move events | All event records are assigned to the default org. |
| 5 | Create admin user | Seeded from `ADMIN_EMAIL` and `ADMIN_PASSWORD` env vars. Granted `super_admin`. |
| 6 | Map API key | The existing `API_KEY` env var continues to authenticate as the super admin user. |
| 7 | Device tokens | Devices without a `device_token` auto-provision into the default org on first check-in. |

### Backward Compatibility

- The legacy flat API (`/api/devices`, `/api/firmware`) continues to work during a
  transition period by implicitly targeting the default org.
- Devices using the old provisioning flow (no org awareness) are accepted into the
  default org.
- The migration is idempotent. Running it again on an already-migrated database
  is a no-op.

---

## 9. Future Considerations

- **API keys per org**: Org-scoped API keys for service-to-service integrations,
  replacing the single global key.
- **Audit log**: Track all mutations (who changed what, when) per org for
  accountability.
- **Org transfer**: Move a device from one org to another with full history
  migration or clean handoff.
- **Rate limiting per org**: Prevent a single org from overwhelming shared
  infrastructure.
- **SSO/OAuth**: Replace password-based auth with OAuth2 providers for larger
  deployments.
