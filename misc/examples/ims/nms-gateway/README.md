# Kamailio IMS NMS HTTP API (native C module)

Every CSCF node exposes the **same API paths and JSON schema**. NMS picks the host by service role and reads the matching `cscf.{role}` block.

## Architecture

```text
NMS agent (role filter)
  ├─ scscf → curl :9280 → S-CSCF
  │           response.cscf.scscf  → available, data
  │           response.cscf.pcscf  → not_available
  └─ pcscf → curl :9281 → P-CSCF
              response.cscf.pcscf  → available, data
              response.cscf.scscf  → not_available
```

## Configuration

**S-CSCF** — copy `nms-api.cfg.sample` → `nms-api.cfg`:

```text
listen=tcp:127.0.0.1:9280
modparam("ims_nms_api", "cscf_role", "scscf")
modparam("ims_nms_api", "api_token", "CHANGE_ME")
```

**P-CSCF** — copy `nms-api-pcscf.cfg.sample` → `nms-api.cfg`:

```text
listen=tcp:127.0.0.1:9281
modparam("ims_nms_api", "cscf_role", "pcscf")
modparam("ims_nms_api", "api_token", "CHANGE_ME")
```

Enable on both: `#!define WITH_NMS_API`

## Endpoints (same on every node)

| Path | Description |
|------|-------------|
| `GET /health` | Liveness |
| `GET /api/stats` | Per-role counters |
| `GET /api/stats/{role}` | Same body as `/api/stats` (role hint ignored) |
| `GET /api/subscribers/{imsi}/registration` | Per-role registration |
| `GET /api/subscribers/{imsi}/calls/active` | Per-role active calls |
| `POST /api/subscribers/{imsi}/disconnect` | Terminate IMSI dialogs |
| `GET /api/subscribers/live?imsis=...` | Bulk registered + active |

All `/api/*` routes require `Authorization: Bearer <api_token>`.

## Unified response shape

Every response includes `nodeRole` (this node's configured role) and a `cscf` object with **scscf**, **pcscf**, and **icscf** keys.

**Available role** (service loaded on this node):

```json
{
  "nodeRole": "scscf",
  "cscf": {
    "scscf": {
      "available": true,
      "up": true,
      "registered": 42,
      "activeCalls": 3
    },
    "pcscf": {
      "available": false,
      "up": false,
      "status": "not_available"
    },
    "icscf": {
      "available": false,
      "up": false,
      "status": "not_available"
    }
  }
}
```

**Registration** (`/api/subscribers/{imsi}/registration`):

```json
{
  "imsi": "001010000000001",
  "nodeRole": "scscf",
  "registered": true,
  "cscf": {
    "scscf": {
      "available": true,
      "up": true,
      "registered": true,
      "impu": "sip:...",
      "contacts": [],
      "registration": { "registeredAt": "...", "state": "registered" }
    },
    "pcscf": {
      "available": false,
      "up": false,
      "status": "not_available"
    }
  }
}
```

**Active calls** — each available role has `activeCalls` and `calls[]`; top-level `activeCalls` is the sum across available roles.

## NMS usage

```bash
# S-CSCF host — read cscf.scscf
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:9280/api/stats
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:9280/api/subscribers/001010000000001/registration
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:9280/api/subscribers/001010000000001/calls/active

# P-CSCF host — read cscf.pcscf
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:9281/api/stats
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:9281/api/subscribers/001010000000001/registration
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:9281/api/subscribers/001010000000001/calls/active
```

NMS logic:

1. Map service role → API host/port (e.g. `scscf` → `:9280`, `pcscf` → `:9281`).
2. Call the same path on that host.
3. Use `response.cscf[role]`; if `available` is false or `up` is false, treat the service as down on that node.
4. Never expect 404 for a missing role — unavailable roles return HTTP 200 with `status: "not_available"`.

## Build

Requires `xhttp`, `ims_dialog`, and the matching usrloc module on each node (`ims_usrloc_scscf` or `ims_usrloc_pcscf`).

## Dialog tagging

Both CSCFs need `set_dlg_profile("nms_imsi", $imsi)` on INVITE (`nms_profile_*.cfg`).
