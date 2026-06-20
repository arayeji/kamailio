# Terminating VoLTE Call Handling — UE Reachability & IMS Restoration

Standards-compliant handling of **terminating** calls (from a PSTN/MGCF gateway or
another IMS network) when the called subscriber is **not currently IMS-registered**.
The S-CSCF performs **Terminating Access Domain Selection (T-ADS)** and an
**IMS restoration / UE-reachability** procedure before falling back to the CS
domain (MSC/VLR) or applying operator unreachable-subscriber policy.

> Reference implementation for this fork. The routing logic lives in
> [`scscf/route/term_reachability.cfg`](../scscf/route/term_reachability.cfg),
> schema in [`scscf/db/term_reachability.sql`](../scscf/db/term_reachability.sql).
> Enable with `#!define WITH_TERM_REACHABILITY`. It is **disabled by default** and
> does not change the existing call flow unless the define is set.

---

## 1. Scope and 3GPP node placement

In a textbook IMS/EPC network this behaviour is split across two logical
functions:

| Logical function | Owns | Spec |
|---|---|---|
| **S-CSCF** | terminating subscriber lookup, iFC evaluation, restoration trigger | TS 23.228 §5.6.x, TS 24.229 |
| **SCC AS** (Service Centralization & Continuity AS) | T-ADS decision (PS vs CS), CS routing | TS 23.237 §6.3 (T-ADS), §8 |
| **HSS** | subscriber state, S6a to MME, CS state (VLR/MSC) | TS 29.228/29.229 (Cx), TS 29.272 (S6a), TS 29.328/29.329 (Sh) |
| **MME** | EPS attach state, paging the UE | TS 23.401 §5.3 |
| **MSC/VLR** | CS registration state, CSFB | TS 23.272 |

This implementation **co-locates the T-ADS + restoration logic in the S-CSCF**
routing script (the simplest deployment for this fork). The same logic can be
relocated unchanged to a standalone **SCC AS** reached over the **ISC** interface
(`ims_isc`) if operator policy requires a separate AS — the decision algorithm
and HSS contract are identical; only the SIP triggering point changes.

```
        PSTN ─── MGCF ───┐
                          │  INVITE (terminating)
 other IMS ── IBCF/I-CSCF ┤
                          ▼
                    ┌───────────┐   Cx/Sh/S6a (Diameter)   ┌──────┐  S6a  ┌─────┐
                    │  S-CSCF   │◄──────── Sh (Diameter) ──►│ HSS  │◄─────►│ MME │
                    │ + T-ADS / │                          └──────┘       └─────┘
                    │ restora-  │                              │ MAP/Diameter
                    │ tion      │                              ▼
                    └─────┬─────┘                          ┌────────┐
                          │ IMS (PS)                        │ MSC/VLR│
                          ▼                                 └────────┘
                  registered UE (VoLTE)        CS fallback (CSFB/ICS) ▲
                                                                       │
                          └──────────── route to MSC ──────────────────┘
```

---

## 2. High-level decision algorithm

```
on terminating INVITE for subscriber IMPU:
  1. terminating lookup + unregistered-services assignment (existing flow, Cx SAR)
  2. if IMS-registered (usrloc contact exists):
        -> route INVITE to contact  (standard VoLTE)  [DONE]
  3. else query HSS subscriber state (Sh UDR):
        { imsState, servingMme, vlrNumber, ueReachable, ratType, lastActivity }
  4. T-ADS decision:
        a. servingMme present  AND  ueReachable != true
              -> UE-Reachability procedure (alert/page) + start paging timer
                 -> wait for IMS REGISTER (restoration)
                       -> if REGISTER before timeout: resume + route via IMS
                       -> if timeout: go to step 5
        b. servingMme present AND ueReachable == true
              -> short restoration wait (UE is up but no IMS reg yet) then step 5
        c. no servingMme AND vlrNumber present
              -> skip paging, route to CS (MSC) per operator policy
        d. neither MME nor VLR
              -> unreachable-subscriber policy (VM / fwd / 480/404/486)
  5. timeout / fallback:
        if vlrNumber present -> route to CS domain (TS 23.272)
        else                 -> unreachable-subscriber policy
```

State machine (per terminating transaction):

```
                 INVITE
                   │
         ┌─────────▼─────────┐  contact found
         │   T_LOOKUP        ├───────────────► ROUTE_IMS ──► (relay) ► TERMINATED
         └─────────┬─────────┘
                   │ not registered
          ┌────────▼────────┐ HSS error/timeout
          │   HSS_QUERY     ├───────────────► FALLBACK_POLICY
          └────────┬────────┘
       MME &      │      no MME, VLR set
    !reachable    │
        ┌─────────┼───────────────► ROUTE_CS (MSC)
        ▼         │
   ┌─────────┐    │ no MME, no VLR
   │ ALERT/  │    └───────────────► UNREACHABLE_POLICY
   │ PAGING  │
   └────┬────┘
        │ t_suspend(); start paging_timer
   ┌────▼─────────────────┐ REGISTER arrives (restoration)
   │ WAIT_RESTORATION     ├───────────────► t_continue ─► ROUTE_IMS ► TERMINATED
   └────┬─────────────────┘
        │ paging_timer expiry
        ▼
   ┌──────────────┐ VLR set ─► ROUTE_CS (MSC)
   │ TIMEOUT      │
   └──────┬───────┘ no VLR ─► UNREACHABLE_POLICY
```

---

## 3. Diameter / HSS interaction design

**The transport to the HSS is standard Diameter Sh — there is no proprietary
API.** (An earlier draft offered a REST adapter; it has been removed now that the
HSS implements Sh per §3.5.)

### 3.1 Native Diameter **Sh** (TS 29.328/29.329)

Kamailio (as S-CSCF / co-located SCC AS) speaks **Sh** to the HSS using
`ims_diameter_server` (`diameter_request()` over `cdp`). It never speaks S6a to
the MME — the S6a/URRP-MME leg is internal to the HSS (§3.3).

| Operation | App-Id | Cmd | AVPs (vendor 10415) | Purpose |
|---|---|---|---|---|
| **UDR** read state | 16777217 | 306 | User-Identity(700)>Public-Identity(601), Data-Reference(703)=LocationInformation(14)/UserState(15), Server-Name(602), Send-Data-Indication(710) | get serving MME (PS), VLR/MSC (CS), IMS user state |
| **SNR** subscribe | 16777217 | 308 | User-Identity(700), Subs-Req-Type(705)=0, Data-Reference(703) | subscribe to UE-reachability; HSS arms MME via S6a IDR(URRP-MME) |
| **PNR** receive | 16777217 | 309 | User-Identity(700), User-Data(606) | HSS push "UE reachable" → S-CSCF resumes the INVITE; reply **PNA** 2001 |

The **UDA** carries the answer as an **Sh-Data XML document inside User-Data
(AVP 606)** (TS 29.328 Annex schema), not as discrete AVPs — so the config
extracts that XML and parses it with `xmlops` (XPaths are HSS/release-specific).
The PNR is an **HSS-initiated** request, handled in
`event_route[diameter:request]` (Kamailio acts as an Sh server for PNR/PNA).

Routing/realm and Origin-Host/Realm/Session-Id are supplied by `cdp` (per
`scscf.xml`); the config adds Destination-Realm + Vendor-Specific-Application-Id.

**Coexistence with Cx.** `ims_diameter_server` installs a *global* CDP request
handler, but it only emits an answer when the route sets `$diameter_response`.
Our `event_route[diameter:request]` sets it **only** for Sh PNR
(App 16777217 / cmd 309); for everything else it leaves it unset, so the handler
returns no response and CDP falls through to the existing Cx handlers
(`ims_registrar_scscf` PPR/RTR, etc.). Keep the handler selective if you extend
it. The HSS must be declared as an Sh `Auth` application in `scscf.xml`:
`<Auth id="16777217" vendor="10415"/>` plus a `Peer`/route to the HSS.

### 3.2 The S6a leg is HSS-internal (not Kamailio)

| Interface | App-Id | Command | Purpose | Spec |
|---|---|---|---|---|
| **S6a** | 16777251 | IDR/IDA 319 | Insert-Subscriber-Data, **IDR-Flags bit 0 = URRP** (request reachability report) | TS 29.272 §5.2.2, §7.3 |
| **S6a** | 16777251 | NOR/NOA 323 | MME **Notify** "UE reachable" | TS 29.272 §5.2.3 |

These are sent **by the HSS to the MME**, triggered by the Sh SNR subscription
from Kamailio. Kamailio does not implement them.

> **Terminology note (important).** The request text references
> *"Alert-Request / Alert-Answer (ALR/ALA)"* on S6a. **TS 29.272 S6a/S6d has no
> Alert-Request command.** The UE-reachability function is realised by
> **IDR(URRP-MME flag)** → MME → **NOR(UE-Reachability)**. (The "Alert" verb comes
> from legacy MAP — *Alert-Service-Centre* / *Ready-for-SM* — used for SMS, and over
> Diameter SGd it is *Alert-Service-Centre-Request* ALR 8388648, **not** S6a.)
> The config exposes an internal `alert` step (the **Sh SNR**) mapped to this
> correct **HSS-side IDR(URRP-MME) → NOR** chain.

### 3.3 Standards-correct UE-reachability chain (what the HSS does)

```
SCC AS/S-CSCF ──Sh SNR(UE-Reachability-for-IP)──► HSS
HSS ──S6a IDR(Insert-Subscriber-Data, IDR-Flags: URRP-MME)──► MME      [TS 29.272 §5.2.2.1.2]
MME: sets URRP-MME; when UE becomes ECM-CONNECTED / pages UE             [TS 23.401 §5.3.4]
MME ──S6a NOR(Notify, UE-Reachable)──► HSS                               [TS 29.272 §5.2.3]
HSS ──Sh PNR(UE-Reachability-for-IP)──► SCC AS/S-CSCF                    [TS 29.328 §6.3]
(in parallel) UE performs IMS re-REGISTER ──► S-CSCF (restoration)       [TS 24.229 §5.4.1]
```

The S-CSCF restoration trigger uses **either** the Sh PNR notification **or** the
incoming IMS REGISTER (whichever arrives first) to resume the suspended INVITE.
This implementation handles **both**: `event_route[diameter:request]` resumes on
Sh PNR, and the REGISTER save hook resumes on IMS re-registration — whichever
fires first wins (race-safe via the htable claim counter).

### 3.5 What the HSS (pretty5gs) must implement — to standard

For Kamailio to run this **purely in Diameter**, the HSS must provide:

| # | HSS capability | Interface / primitive | Spec | Notes |
|---|---|---|---|---|
| H1 | **Sh server**: accept UDR, return UDA | Sh 16777217, UDR/UDA 306 | TS 29.328 §6.1.1; TS 29.329 §6.1 | answer `User-Data` (606) as **Sh-Data XML** |
| H2 | Sh data: **IMS user state** | Data-Reference 11 (IMSUserState) | TS 29.328 §6.3 | REGISTERED / UNREGISTERED / etc. |
| H3 | Sh data: **CS + PS location / serving nodes** | Data-Reference 14 (LocationInformation), Requested-Domain 706 (CS/PS) | TS 29.328 §6.3 | must expose **serving MME** (PS) and **VLR/MSC number** (CS) |
| H4 | Sh data: **registration/UE state** | Data-Reference 15 (UserState) | TS 29.328 §6.3 | CS/PS attach state |
| H5 | **Sh notifications**: accept SNR, send PNR | Sh SNR/SNA 308, PNR/PNA 309, Subs-Req-Type 705 | TS 29.328 §6.1.2/6.1.3 | subscribe + push for **UE-Reachability-for-IP** |
| H6 | **S6a → MME URRP**: arm reachability on SNR | S6a IDR/IDA 319, **IDR-Flags(1490) bit 0 = URRP** | TS 29.272 §5.2.2, §7.3.30 | HSS-internal; UE-Reachability-Request |
| H7 | **S6a ← MME Notify**: accept NOR, map to Sh PNR | S6a NOR/NOA 323 | TS 29.272 §5.2.3 | on "UE reachable", push Sh PNR to the subscribed S-CSCF |
| H8 | Diameter base: Vendor-Id 10415, Vendor-Specific-Application-Id, Auth-Session-State, realm routing | RFC 6733 | — | standard peer/realm setup with the Kamailio CDP peer |

If pretty5gs already implements **S6a (MME)** and **Cx (I/S-CSCF)** but **not Sh**,
then **H1–H5 (the Sh server) is the gap to implement** — the S6a side (H6–H7,
URRP-MME/NOR) it likely already has or can extend. Once the Sh server exists,
Kamailio talks to it natively and no adapter/API is involved.

---

## 4. SIP transaction handling (suspend / resume)

To wait for restoration without blocking a SIP worker, the terminating INVITE
transaction is **suspended** (`t_suspend()`), and the suspend handle
(`tindex:tlabel`) is stored in an `htable` entry keyed by IMPU together with the
paging deadline.

Two independent events can resume it:

1. **Restoration hit** — the S-CSCF `REGISTER` save route checks the pending table
   for the just-registered IMPU; on a match it calls
   `t_continue(tindex, tlabel, "TERM_REACH_RESUME")`.
2. **Paging timeout** — an `rtimer` route periodically scans pending entries; for
   any past its deadline it calls `t_continue(tindex, tlabel, "TERM_REACH_TIMEOUT")`.

Both resume routes delete the pending entry under the htable slot lock to avoid a
**double-resume race** (REGISTER and timeout firing simultaneously) — only the
process that successfully removes the entry is allowed to continue the
transaction.

```mermaid
sequenceDiagram
    autonumber
    participant GW as MGCF / peer IMS
    participant S as S-CSCF (T-ADS)
    participant H as HSS (Sh/S6a)
    participant M as MME
    participant UE as UE
    GW->>S: INVITE (terminating)
    S->>S: terminating lookup (Cx SAR unreg) — no contact
    S->>H: Sh UDR (306) read LocationInformation/UserState
    H-->>S: Sh UDA (User-Data XML): servingMme set, ueReachable=false, vlrNumber set
    S->>H: Sh SNR (308) Subscribe UE-Reachability
    H->>M: S6a IDR (319, IDR-Flags URRP-MME)
    M->>UE: Paging / NAS
    S->>S: t_suspend(); store pending(IMPU,tindex:tlabel,deadline)
    par restoration
        Note over M,H: UE reachable → S6a NOR (323)
        H->>S: Sh PNR (309) UE reachable
        UE->>S: REGISTER (IMS restoration)
        S->>S: PNR handler / REGISTER save → t_continue(RESUME) [race-safe]
        S->>S: lookup(location) → contact now present
        S-->>UE: INVITE delivered via IMS (VoLTE)
    and timeout
        S->>S: rtimer: deadline passed → t_continue(TIMEOUT)
        alt vlrNumber present
            S->>GW: route to CS domain / MSC (TS 23.272)
        else no CS
            S-->>GW: 480/404/486 or VM/forward (operator policy)
        end
    end
```

---

## 5. Configuration parameters

Defined in `term_reachability.cfg` via `htable` "config" entries (runtime
tunable through RPC `htable.set`) and `#!define`s:

| Parameter | Default | Meaning | Req. |
|---|---|---|---|
| `cfg=>paging_timeout_s` | `5` | Max wait for IMS restoration after alert | 8 |
| `cfg=>max_paging_attempts` | `2` | Re-issue alert up to N times within the window | 8 |
| `cfg=>reg_monitor_interval_s` | `1` | rtimer scan interval for restoration/timeout | 8 |
| `cfg=>ue_up_wait_s` | `2` | Short wait when MME says UE already reachable | 8 |
| HSS Diameter (Sh) timeout | `cdp.xml` `TransactionTimeout` | controls UDR/SNR wait | 10 |
| `HSS_REALM` / `SCSCF_SERVER_NAME` | defines | Sh Destination-Realm + Server-Name AVP | 3 |
| `cfg=>unreachable_action` | `voicemail` | `voicemail`\|`forward`\|`reject` | 6,8 |
| `cfg=>unreachable_reject_code` | `480` | SIP code when `reject` | 6 |

---

## 6. Logging (requirement 9)

Every step emits a structured `xlog` line with a correlation id (`$ci`) and IMPU.
Log tags (grep-able):

```
TERM-REACH INVITE        incoming terminating INVITE
TERM-REACH IMSREG        IMS registration lookup result (registered/unregistered)
TERM-REACH HSSQ          HSS reachability query result (full state)
TERM-REACH MME           serving MME identity
TERM-REACH VLR           serving MSC/VLR identity
TERM-REACH ALERT         Alert/IDR(URRP-MME) transmitted
TERM-REACH PAGING        paging started (attempt N, deadline)
TERM-REACH PAGEEXP       paging timeout
TERM-REACH RESTORE       IMS re-registration detected (restoration)
TERM-REACH CSFB          CS fallback decision
TERM-REACH ROUTE         final routing destination
```

---

## 7. Failure scenarios (requirement 10)

| Scenario | Detection | Handling |
|---|---|---|
| HSS unavailable | `diameter_request` returns 0 / CDP no peer / timeout | log `HSSQ err`; conservative fallback: if last-known VLR cached → CS, else `unreachable_action` |
| MME unavailable | alert returns `pagingTriggered=false` / IDR error | skip paging wait; go straight to timeout branch (CS or policy) |
| Diameter timeout | `hss_query_timeout_ms` / async no reply | treat as HSS-unavailable |
| Paging timeout | rtimer deadline | CS fallback if VLR else policy |
| Restoration race (REGISTER + timeout together) | htable slot-locked delete | only the winner calls `t_continue`; loser no-ops |
| Subscriber detached mid-process | REGISTER never arrives; HSS later says no MME/VLR | timeout → `unreachable_action` |
| Multiple simultaneous terminating calls | pending table keyed `IMPU#callid` | each transaction tracked independently; alert deduped per IMPU within window |
| Double INVITE retransmit | `t_lookup_request()` in REQINIT (existing) | absorbed before reaching this logic |

---

## 8. Database schema (requirement deliverable)

In-memory `htable` is authoritative for live paging state (lowest latency,
auto-expiring). The SQL tables in
[`term_reachability.sql`](../scscf/db/term_reachability.sql) are **optional** and
used for: (a) multi-instance S-CSCF persistence/audit of pending sessions, and
(b) operator CS-routing policy lookup. See that file for column docs.

- `term_reach_pending` — audit/persistence of suspended terminating sessions.
- `term_reach_policy` — per-IMPU/per-realm CS fallback + unreachable policy.
- `term_reach_hss_cache` — short-TTL cache of last HSS reachability answer.

---

## 9. Mapping of implementation steps to 3GPP specifications

| # | Implementation step | Function / location | Spec section |
|---|---|---|---|
| 1 | Receive terminating INVITE from MGCF / peer IMS | `route(TERM_REACH_ENTRY)` | TS 23.228 §5.6.1; TS 24.229 §5.4.3 |
| 2 | Terminating subscriber lookup, unreg services (Cx SAR) | existing `term_impu_registered` + `assign_server_unreg` | TS 29.228 §6.1.2; TS 29.229 §6 (SAR/SAA) |
| 3 | IMS registration check (contact present?) | `lookup("location")` / `term_impu_has_contact` | TS 23.228 §5.12; TS 24.229 §5.4.1 |
| 4 | Route to registered contact (VoLTE) | `route(TERM_REACH_ROUTE_IMS)` | TS 23.228 §5.6.1; TS 24.229 §5.4.3.2 |
| 5 | HSS subscriber-state query (state, MME, VLR, reachability) | `route(TERM_REACH_HANDLE)` (HSS GET) | TS 29.328/29.329 (Sh UDR); TS 29.272 §5 (S6a) |
| 6 | T-ADS PS-vs-CS decision | `route(TERM_REACH_HANDLE)` (decision branches) | TS 23.237 §6.3 (T-ADS) |
| 7 | UE-reachability trigger: Kamailio sends **Sh SNR**; HSS arms MME via **S6a IDR(URRP-MME)**, MME pages, replies NOR | `route(TERM_REACH_ALERT)` (Sh SNR 308) | TS 29.328 §6.1.2 (SNR); TS 29.272 §5.2.2 (IDR URRP-MME), §5.2.3 (NOR); TS 23.401 §5.3.4 (paging) |
| 9b | Restoration via HSS push (**Sh PNR**) | `event_route[diameter:request]` → `route(TERM_REACH_DIAMETER_REQUEST)` | TS 29.328 §6.1.3 (PNR/PNA) |
| 8 | Suspend transaction + paging timer | `t_suspend()` + `htable`/`rtimer` | TS 23.228 §5.6 (terminating procedures) |
| 9 | IMS restoration monitor (REGISTER) | REGISTER save hook → `t_continue` | TS 23.228 §5.x restoration; TS 24.229 §5.4.1; TS 29.228 restoration procedures |
| 10 | Resume + deliver via IMS | `route[TERM_REACH_RESUME]` | TS 24.229 §5.4.3.2 |
| 11 | Timeout handling | `route[TERM_REACH_TIMEOUT]` | TS 23.237 §6.3 |
| 12 | CS fallback to MSC/VLR | `route(TERM_REACH_ROUTE_CS)` | TS 23.272 §6/§7 (CSFB); TS 23.237 §8 (ICS) |
| 13 | MSC-registered, no MME → direct CS | decision branch (c) | TS 23.272; TS 23.237 §6.3 |
| 14 | Unreachable-subscriber policy | `route(TERM_REACH_UNREACHABLE)` | TS 23.228 §5.6 (operator services); TS 24.229 |
| 15 | EPS attach / paging context (external) | MME (Open5GS) | TS 23.401 §5.3.3 (attach), §5.3.4 (paging) |

---

## 10. Enabling it

In `scscf/kamailio.cfg`:

```
#!define WITH_TERM_REACHABILITY
# Sh peer realm + this S-CSCF's Server-Name (used in the Sh AVPs):
#!define HSS_REALM "ims.mnc001.mcc001.3gppnetwork.org"
#!define SCSCF_SERVER_NAME "sip:scscf.ims.mnc001.mcc001.3gppnetwork.org:6060"
```

The HSS must be reachable as a Diameter **Sh** peer in the S-CSCF `cdp.xml`
(Auth application `16777217`, vendor `10415`).

The include is added (guarded) near the other includes, the terminating entry
hook is added (guarded) inside `route[term]` before `route(FINAL_TERM)`, and the
restoration hook is added (guarded) inside `route[REGISTER]` after a successful
`save()`. All three are inert unless `WITH_TERM_REACHABILITY` is defined. See the
top of `term_reachability.cfg` for the exact three-line integration.
