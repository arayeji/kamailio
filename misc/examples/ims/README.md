# IMS example stack — fork extensions

This tree extends upstream [Kamailio](https://github.com/kamailio/kamailio) with
Open5GS-oriented IMS features. Changes since the upstream merge-base (`36dccc2a`)
are listed below.

## Features added in this fork

### 1. Native NMS HTTP API (`ims_nms_api`)

Replaces external `kamcmd`/Python gateways with a **localhost HTTP API** on each
CSCF (Bearer token, Osmo-MSC style).

- **Module:** `src/modules/ims_nms_api/` (C, `xhttp`)
- **Config:** `misc/examples/ims/nms-gateway/` — see [nms-gateway/README.md](nms-gateway/README.md)
- **Endpoints:** `/health`, `/api/stats`, `/api/subscribers/{imsi}/registration`,
  `/api/subscribers/{imsi}/calls/active`, `POST .../disconnect`, `/api/subscribers/live`
- **Per-node role:** S-CSCF `:9280`, P-CSCF `:9281` (`cscf_role` modparam)
- **O(1) per-IMSI call lookup** via `ims_dialog` profile `nmsimsi` on INVITE
- **Subscriber resolution:** IMPI, MSISDN profile keys, `get_subscription` via
  `dlsym` from `ims_usrloc_scscf`
- **Registration fields:** `registeredAt`, `lastSeenAt`, `expiresAt`, P-CSCF contact
- **Hardening:** Bearer auth on xhttp fake SIP messages, `srjson` on pkg heap,
  subscription lock ordering fix (avoids S-CSCF registration deadlock)

Enable with `#!define WITH_NMS_API` and include `nms_api_*.cfg` / `nms_profile_*.cfg`.

### 2. Rx bearer teardown fix (`ims_qos`)

Fixes **QCI 1 bearer leaks** in Open5GS when a call ends before the async AAR/AAA
round-trip completes (e.g. AAR on 183, fast CANCEL/487).

- **`rx_str_teardown_dialog_media()`** — STR for Rx sessions stored on the SIP
  dialog (`originating` / `terminating` dlg vars)
- **`async_aar_callback`:** send STR immediately if AAA arrives after dialog is
  already `DLG_STATE_DELETED`; tear down on AAR failure/timeout
- **`Rx_STR_dialog()`** — config function for in-dialog **BYE** (belt-and-suspenders
  with existing dialog callbacks)
- **P-CSCF routes:** `mo.cfg` / `mt.cfg` call `Rx_STR_dialog()` on BYE

Early-fail (CANCEL / 4xx–6xx **before** answer) is handled in the module only —
config-level `Rx_STR_dialog()` cannot key the Rx session without a to-tag.

Requires **Diameter Rx** (CDP + `pcscf.xml` peer to Open5GS PCRF). See
[pcscf/README.md](pcscf/README.md#rx-volte-bearer-control).

### 3. IMS session timers (`session_timers.cfg`)

Dialog lifetime aligned with **RFC 4028** session timers (Min-SE / Session-Expires).

### 4. S-CSCF contact deduplication (`ims_registrar_scscf`)

Deduplicate contacts by **`+sip.instance`** per **3GPP TS 24.229** (multi-device
REGISTER without duplicate IMPU bindings).

---

## Build notes

```text
# NMS API (per CSCF node)
loadmodule "xhttp"
loadmodule "ims_nms_api"
# + ims_usrloc_scscf OR ims_usrloc_pcscf, ims_dialog

# VoLTE Rx (P-CSCF)
loadmodule "cdp"
loadmodule "cdp_avp"
loadmodule "ims_qos"
# + pcscf.xml Diameter peer to PCRF
```

## Verify Rx teardown (Open5GS)

After hangup:

```bash
grep -i 'Session-Termination\|STR' /var/log/open5gs/pcrfd.log
grep -i 'Charging-Rule-Remove' /var/log/open5gs/pcrfd.log
grep -i 'Delete Bearer' /var/log/open5gs/smf.log /var/log/open5gs/mmed.log
```

Success: STR → Charging-Rule-Remove → Delete Bearer → bearer count back to default + IMS.

## Commit history (fork-only)

| Commit     | Summary |
|------------|---------|
| `01f76ffd` | Add native `ims_nms_api` unified per-CSCF HTTP API |
| `335e1c98` | S-CSCF: deduplicate contacts by `+sip.instance` |
| `b3d86bf8` | IMS session timers (RFC 4028) |
| `c5db5caf` | gitignore: exclude site-specific deploy scripts |
| `4c757a52`–`f41e6f1c` | NMS API hardening, auth, xhttp, srjson, subscriber lookup |
| `657ce368` | `ims_qos`: Rx STR on call teardown (bearer leak fix) |
| `154d88c5` | `ims_nms_api`: subscription lock fix, `nmsimsi` profile |
