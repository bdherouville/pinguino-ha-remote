# ESP-IDF / NimBLE probe spec — connecting & bonding to the Ganymede remote

Translates the validated Linux/BlueZ findings (`docs/linux-capture-findings.md`, where the
host successfully bonded with the remote) into concrete NimBLE configuration for the
**central/probe** role in `main/main.c`. Ground truth = Android's successful bond
(`logs/ganymede-btsnoop3.log`) + our reproduced Linux bond.

Scope: the probe acting as **central → remote (peripheral)**. The emulator side
(**A/C central → ESP32 peripheral**) has the OPPOSITE security requirements — see the note
at the end and memory `ac-smp-requirements`.

## TL;DR — three changes in `main/main.c`

| # | Setting | Current (`main.c`) | Required | Why |
|---|---|---|---|---|
| 1 | `cp.supervision_timeout` | `0x0100` (2560 ms) | **`0x01F4` (5000 ms)** | Android's held link used 5000 ms; gives the slow post-bond ops headroom |
| 2 | `ble_hs_cfg.sm_sc` | `1` | **`0`** (legacy) | Remote answers SC=0 → legacy Just Works. SC=1 *can* downgrade, but forcing legacy removes a failure mode |
| 3 | key dist masks | `ENC \| ID` | **`ENC \| ID \| SIGN`** | Remote distributes LTK+IRK+**CSRK** (0x07); accept all it offers |

Everything else in `main.c` is already correct (see "Already correct" below). The dominant
real-world fix is **operational**, not config: it's a retry lottery and the remote must be
in **pairing mode** — see §4.

## 1. Connection parameters (`struct ble_gap_conn_params`)

Match Android's successful link exactly. Units: interval = 1.25 ms, timeout = 10 ms,
scan = 0.625 ms.

```c
struct ble_gap_conn_params cp = {
    .scan_itvl   = 0x0010,   // 10 ms  — continuous initiator scan
    .scan_window = 0x0010,   // 10 ms  (== itvl: never miss the ~3 s advert window)
    .itvl_min    = 24,       // 30 ms   (0x0018)   ✅ already correct
    .itvl_max    = 40,       // 50 ms   (0x0028)   ✅ already correct  (Android neg. 48.75 ms)
    .latency     = 0,        //                     ✅ already correct
    .supervision_timeout = 0x01F4,  // 5000 ms  ← CHANGE from 0x0100 (2560 ms)
    .min_ce_len  = 0,
    .max_ce_len  = 0,
};
```

Rationale: the remote only services a **fast** link (30–50 ms). Slow intervals
(300–4000 ms) produced **zero** data on Linux. Do **not** switch to the 4000 ms value from
char `0x2A04` for the initial connect — that is the remote's *post-bond* preference, which
it requests itself via a Connection Parameter Update (Linux saw it move to ~400 ms after
bonding). Accept that update when it arrives (NimBLE does by default).

## 2. NEVER scan while initiating (the #1 Linux failure cause)

On Linux, leaving an active discovery scan running during the connect starved the
connection's first LL events → `0x3e` on **every** attempt (held 0/30 vs Android's ~40 %).

`main.c` **already does this right** — it calls `ble_gap_disc_cancel()` before
`ble_gap_connect()` (line ~209, comment "can't scan and initiate at once"). Keep it.
Hard rule:

- Use **`ble_gap_connect()`** (it runs its own internal initiator scan) — never run
  `ble_gap_disc()` concurrently.
- In the GAP event handler, do **not** restart `ble_gap_disc()` until a connect attempt has
  fully resolved (connected or `BLE_GAP_EVENT_CONNECT` with nonzero status).

## 3. SMP / bonding config (`ble_hs_cfg`) — Just Works, LEGACY

Remote's Pairing Response (ground truth): `IO = NoInputNoOutput`, `AuthReq = 0x01`
(Bonding; **SC=0, MITM=0**), max key size 16, distributes **LTK + IRK + CSRK**.

```c
ble_hs_cfg.sm_bonding      = 1;                          // ✅ keep
ble_hs_cfg.sm_mitm         = 0;                          // ✅ keep (Just Works, unauthenticated)
ble_hs_cfg.sm_sc           = 0;                          // ← CHANGE from 1: remote is legacy-only
ble_hs_cfg.sm_io_cap       = BLE_HS_IO_NO_INPUT_OUTPUT;  // ✅ keep
ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;
ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;
```

Notes:
- `sm_sc = 1` is *tolerable* (Android requested SC and the remote downgraded to legacy), but
  set `0` to match the remote and avoid any SC-negotiation edge case.
- Keep `sm_mitm = 0`. With `NO_INPUT_OUTPUT` the only valid method is Just Works anyway; do
  NOT set `sm_mitm = 1` here (that's the A/C side, and it's contradictory with NIO IO caps).
- Persist bonds (LTK) in NVS so reconnects skip pairing (NimBLE store; ensure
  `CONFIG_BT_NIMBLE_NVS_PERSIST=y`).
- After `BLE_GAP_EVENT_CONNECT` (status 0), call `ble_gap_security_initiate(conn_handle)` —
  `main.c` already does this (line ~274). ✅

## 4. Handle the establishment lottery + pairing mode (operational)

Even configured perfectly, **most attempts fail** with HCI reason `0x3e`
("Connection Failed to be Established") — the remote isn't at the first connection events.
Android also failed ~60 % and just retried. So:

- On connect-failure (`BLE_GAP_EVENT_CONNECT` with nonzero status — typically
  `BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT)` = HCI `0x3e`), **immediately re-issue the
  connect**. Loop until connected. Log the attempt count.
- The remote only reliably *services* an incoming central connection while held in its
  **pairing mode** (the ~7 s home/pairing-button hold). The connection that bonded on Linux
  happened in pairing mode. So: arm the probe (`connect`), then **hold the remote in pairing
  mode** and let it retry.
- `AuthenticationCanceled`-type failures = the link dropped before SMP, i.e. lottery lost —
  not a security-param problem. Don't "fix" SMP in response; just retry.

### Concrete retry change for `main/main.c` (NOT yet applied — review first)

`main.c` is intentionally CLI-driven ("do not auto-reconnect", line ~310), so today each
`0x3e` just logs `connect_failed` and stops. Add a bounded auto-retry of the *establishment*
(distinct from reconnecting a finished session). Sketch:

```c
/* file scope */
static ble_addr_t g_target;          /* set by ganymede_connect() */
static bool       g_connect_armed;   /* set true on CLI connect, false on stop/success */
static int        g_connect_tries;
#define PROBE_MAX_ESTABLISH_TRIES 200

/* in ganymede_connect(addr): remember target + arm */
g_target = *addr; g_connect_armed = true; g_connect_tries = 0;

/* in BLE_GAP_EVENT_CONNECT, the else (failure) branch: */
} else {
    log_json("connect_failed", "\"status\":%d,\"try\":%d", event->connect.status, g_connect_tries);
    if (g_connect_armed && ++g_connect_tries < PROBE_MAX_ESTABLISH_TRIES) {
        ganymede_connect(&g_target);     /* retry the establishment lottery */
    }
}

/* on success (status==0): g_connect_armed = false;   // stop the retry loop
   add a `stop` CLI command that sets g_connect_armed=false + ble_gap_conn_cancel(). */
```

Keep `ganymede_connect()` from resetting `g_connect_tries` when called as a retry (guard it),
or factor the raw `ble_gap_connect()` call into a helper the retry calls directly. This keeps
the read-only probe philosophy (no auto-reconnect after a real session) while still winning
the establishment lottery the device requires.

## Already correct in `main.c` (do not change)

- `itvl_min=24 / itvl_max=40 / latency=0` connection interval.
- `ble_gap_disc_cancel()` before `ble_gap_connect()`.
- `sm_bonding=1`, `sm_mitm=0`, `sm_io_cap=NO_INPUT_OUTPUT`.
- `ble_gap_security_initiate()` on connect.
- `BLE_HS_FOREVER` connect duration.

## ⚠️ Per-role SMP difference (emulator vs probe)

`main/emulator.c` (A/C-facing peripheral) correctly uses the OPPOSITE security:
`sm_mitm=1, sm_sc=1` because the **A/C central demands MITM + LE Secure Connections**
(memory `ac-smp-requirements`). Do not unify these. Summary:

| Role | Peer | sm_sc | sm_mitm | io_cap | method |
|---|---|---|---|---|---|
| Probe (`main.c`) central → remote | Ganymede remote | **0** | **0** | NIO | legacy Just Works |
| Emulator (`emulator.c`) peripheral ← A/C | air-conditioner | 1 | 1 | (needs review*) | LE SC |

\* `emulator.c` sets `sm_mitm=1` with `io_cap=NO_INPUT_OUTPUT`, which can only yield Just
Works (no MITM possible without IO). If the A/C truly requires authenticated MITM, the
emulator needs real IO capabilities or OOB — flag for the A/C-pairing work, out of scope here.
