#ifndef APP_EMULATOR  /* probe (BLE central) app; the emulator app lives in emulator.c */
/*
 * Ganymede BLE probe — Milestone 1: connect + bond.
 *
 * Boots, scans for the De'Longhi "Ganymede" A/C remote, connects, initiates
 * Just Works pairing + bonding (Secure Connections, no MITM), reaches an encrypted
 * link, and persists the bond in NVS so reconnects skip re-pairing.
 *
 * The goal is to establish the bonded, encrypted link that Android could create but
 * which is the prerequisite for reading the protected HID command path (0x2A4B /
 * 0x2A4D / 0x2908) in later milestones. See SPECS.md and CLAUDE.md.
 *
 * Structure is intentionally lean (main.c + logger). Later milestones split GATT
 * discovery, decoders, and the CLI into separate modules per SPECS.md §14.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_console.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"   /* ble_gattc_* (service/char/desc discovery, read, write) */
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "logger.h"

static const char *TAG = "ganymede";

/* Bring-up diagnostic: log every discovered advertiser (deduped by the controller),
 * not just the target, to confirm the scanner works and see what is nearby.
 * MUST be 0 for real use: at ~40 adverts/sec the USB-Serial/JTAG console saturates
 * and drops lines under backpressure — including the target's scan_match — which
 * looks like "the ESP32 can't see the remote". With only the target logged there is
 * no flood and the match always gets through. */
#define LOG_ALL_ADVERTS 1

/* ble_store_config.c provides this; declared here because there is no public header. */
void ble_store_config_init(void);

/* Target remote. Stored big-endian (human order) here; BLE addresses on the wire are
 * little-endian (val[0] is the least-significant byte), so we reverse when comparing. */
static const uint8_t TARGET_ADDR_BE[6] = {0x00, 0xA0, 0x50, 0x00, 0x00, 0x00} /* set lower 3 bytes to your remote's address (Cypress OUI 00:A0:50) */;
#define TARGET_NAME "Ganymede"

/* HID GATT UUIDs we care about (see docs/android-capture-findings.md). */
#define HID_SVC_UUID      0x1812
#define REPORT_MAP_UUID   0x2A4B   /* read: the HID report map */
#define REPORT_UUID       0x2A4D   /* subscribe: input-report notifications */
#define CCCD_UUID         0x2902   /* descriptor we write 01 00 to */

/* The remote's connection establishment is a lottery (most attempts fail HCI 0x3e);
 * keep re-issuing the connect until one holds. See docs/esp-idf-nimble-probe-spec.md §4. */
#define PROBE_MAX_ESTABLISH_TRIES 200

static int ganymede_gap_event(struct ble_gap_event *event, void *arg);
static void ganymede_connect_raw(const ble_addr_t *addr);
static void ganymede_discover(uint16_t conn_handle);
static void ganymede_read_report_map(uint16_t conn_handle);
static void ganymede_subscribe(uint16_t conn_handle);

/* Current connection handle, or BLE_HS_CONN_HANDLE_NONE when not connected.
 * Updated from the GAP callback; read by the heartbeat task. */
static volatile uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;

/* Connection-establishment retry lottery (spec §4). */
static ble_addr_t g_target;          /* remembered target for retries */
static bool       g_connect_armed;   /* keep retrying 0x3e while true */
static int        g_connect_tries;

/* HID discovery results, filled in by the discovery state machine (§1b). */
static struct {
    uint16_t hid_start, hid_end;     /* HID service 0x1812 handle range */
    uint16_t report_map_val;         /* 0x2A4B value handle (read) */
    uint16_t report_val;             /* 0x2A4D input-report value handle (~0x003b) */
    uint16_t report_cccd;            /* 0x2902 CCCD handle (write 01 00) */
} g_hid;

/* ---- helpers ---------------------------------------------------------------- */

/* Format a NimBLE address (little-endian val[]) as "00:A0:50:XX:XX:XX". */
static void addr_str(char *out, size_t out_sz, const uint8_t *val_le)
{
    snprintf(out, out_sz, "%02x:%02x:%02x:%02x:%02x:%02x",
             val_le[5], val_le[4], val_le[3], val_le[2], val_le[1], val_le[0]);
}

/* True if an advertised address (little-endian val[]) matches TARGET_ADDR_BE. */
static bool addr_is_target(const uint8_t *val_le)
{
    for (int i = 0; i < 6; i++) {
        if (val_le[i] != TARGET_ADDR_BE[5 - i]) {
            return false;
        }
    }
    return true;
}

/* Build a ble_addr_t for the default target (public address). */
static void target_addr(ble_addr_t *out)
{
    out->type = BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++) {
        out->val[i] = TARGET_ADDR_BE[5 - i];  // human order -> little-endian wire order
    }
}

/* Parse "aa:bb:cc:dd:ee:ff" (human order) into a ble_addr_t. Returns 0 on success. */
static int parse_addr(const char *str, ble_addr_t *out)
{
    unsigned b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return -1;
    }
    out->type = BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++) {
        out->val[i] = (uint8_t)b[5 - i];
    }
    return 0;
}

/* Emit a JSON line describing the current security state of a connection. */
static void log_sec_state(const char *type, const struct ble_gap_conn_desc *desc, int status)
{
    char addr[18];
    addr_str(addr, sizeof(addr), desc->peer_id_addr.val);
    log_json(type,
             "\"addr\":\"%s\",\"status\":%d,\"encrypted\":%d,\"authenticated\":%d,"
             "\"bonded\":%d,\"key_size\":%d",
             addr, status, desc->sec_state.encrypted,
             desc->sec_state.authenticated, desc->sec_state.bonded,
             desc->sec_state.key_size);
}

/* ---- scanning --------------------------------------------------------------- */

/* Common handling for a received advertisement (legacy or extended). Logs the
 * target as scan_match, and any other *named* advertiser as adv (with PHY info),
 * so the remote surfaces whatever address type / PHY it uses. CLI-driven: never
 * auto-connects. */
static void handle_advert(const ble_addr_t *addr, int8_t rssi,
                          const uint8_t *data, uint8_t len,
                          uint8_t event_type, uint8_t prim_phy, uint8_t sec_phy,
                          uint8_t props, uint8_t data_status)
{
    struct ble_hs_adv_fields fields;
    char s[18];
    bool have_name = (ble_hs_adv_parse_fields(&fields, data, len) == 0 && fields.name_len > 0);
    bool name_is_target = have_name &&
                          fields.name_len == strlen(TARGET_NAME) &&
                          memcmp(fields.name, TARGET_NAME, fields.name_len) == 0;
    bool address_is_target = addr_is_target(addr->val);

    if (address_is_target || name_is_target) {
        addr_str(s, sizeof(s), addr->val);
        const char *reason = address_is_target ? (name_is_target ? "addr_name" : "addr") : "name";
        if (have_name) {
            log_json("scan_match", "\"addr\":\"%s\",\"addr_type\":%d,\"rssi\":%d,"
                     "\"event_type\":%d,\"prim_phy\":%d,\"sec_phy\":%d,\"props\":%d,"
                     "\"data_status\":%d,\"reason\":\"%s\",\"name\":\"%.*s\"",
                     s, addr->type, rssi, event_type, prim_phy, sec_phy, props,
                     data_status, reason, fields.name_len, fields.name);
        } else {
            log_json("scan_match", "\"addr\":\"%s\",\"addr_type\":%d,\"rssi\":%d,"
                     "\"event_type\":%d,\"prim_phy\":%d,\"sec_phy\":%d,\"props\":%d,"
                     "\"data_status\":%d,\"reason\":\"%s\"",
                     s, addr->type, rssi, event_type, prim_phy, sec_phy, props,
                     data_status, reason);
        }
        return;
    }
#if LOG_ALL_ADVERTS
    /* Inventory mode: log EVERY advertiser (deduped by the controller), named or not,
     * so we can see exactly which addresses the radio hears. */
    addr_str(s, sizeof(s), addr->val);
    if (have_name) {
        log_json("adv", "\"addr\":\"%s\",\"addr_type\":%d,\"rssi\":%d,\"event_type\":%d,"
                 "\"prim_phy\":%d,\"sec_phy\":%d,\"props\":%d,\"data_status\":%d,"
                 "\"name\":\"%.*s\"",
                 s, addr->type, rssi, event_type, prim_phy, sec_phy, props, data_status,
                 fields.name_len, fields.name);
    } else {
        log_json("adv", "\"addr\":\"%s\",\"addr_type\":%d,\"rssi\":%d,\"event_type\":%d,"
                 "\"prim_phy\":%d,\"sec_phy\":%d,\"props\":%d,\"data_status\":%d",
                 s, addr->type, rssi, event_type, prim_phy, sec_phy, props, data_status);
    }
#endif
}

static void ganymede_scan(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        log_json("error", "\"where\":\"infer_addr\",\"rc\":%d", rc);
        return;
    }

    /* Continuous ACTIVE legacy scan: active fetches scan responses, so a device whose name
     * (or identity) is in the scan response is captured — needed to spot "Ganymede" even if
     * it advertises with a random/RPA address. window == itvl => listen 100% of the time
     * (0.625 ms units; 0x00A0 = 100 ms). filter_duplicates=1 => clean one-line-per-device
     * inventory. duration BLE_HS_FOREVER. */
    struct ble_gap_disc_params disc_params = {
        .filter_duplicates = 0,  // report every advert (no dedup) so cross-checking sees the
                                 // full picture and we never suppress a sparse target.
        .passive = 1,            // PASSIVE: never transmit SCAN_REQ. ACTIVE scanning sends a
                                 // SCAN_REQ to every scannable advertiser; in busy RF that is a
                                 // SCAN_REQ storm on the advertising channels that JAMS a weak
                                 // advertiser like the De'Longhi remote — so the ESP32's own
                                 // active scan was preventing it (and nearby Android/Linux) from
                                 // hearing the remote. The remote's name is in the primary
                                 // ADV_IND, so passive still matches it by name/address.
        .itvl = 0x00A0,          // 100 ms, window == itvl => 100% listen duty
        .window = 0x00A0,
        .filter_policy = 0,
        .limited = 0,
    };

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, ganymede_gap_event, NULL);
    if (rc != 0) {
        log_json("error", "\"where\":\"gap_disc\",\"rc\":%d", rc);
        return;
    }
    log_event("scan_start");
}

/* Issue one connection attempt. Does NOT touch the retry counter, so the
 * establishment-lottery retry path can call it repeatedly. */
static void ganymede_connect_raw(const ble_addr_t *addr)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        log_json("error", "\"where\":\"infer_addr\",\"rc\":%d", rc);
        return;
    }

    /* Stop scanning before connecting — concurrent active scan starves the
     * connection's first LL events and makes every establishment fail (0x3e).
     * See docs/esp-idf-nimble-probe-spec.md §2. */
    ble_gap_disc_cancel();

    char s[18];
    addr_str(s, sizeof(s), addr->val);
    log_json("connecting", "\"addr\":\"%s\",\"addr_type\":%d", s, addr->type);

    /* Duration BLE_HS_FOREVER: the controller keeps initiating and connects the instant
     * the target advertises — essential for this remote, which only advertises briefly
     * (~8 s interval, and only while awake after a pairing-button hold). Continuous
     * initiator scan (scan_window == scan_itvl) so no advert is missed. The remote
     * renegotiates to its slow 4000 ms connection interval after connecting. */
    struct ble_gap_conn_params cp = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min = 24,   // 30 ms
        .itvl_max = 40,   // 50 ms  (Android's successful link negotiated 48.75 ms)
        .latency = 0,
        .supervision_timeout = 0x01F4,  // 5000 ms — matches Android's held link (was 2560 ms)
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    rc = ble_gap_connect(own_addr_type, addr, BLE_HS_FOREVER, &cp, ganymede_gap_event, NULL);
    if (rc != 0) {
        log_json("error", "\"where\":\"gap_connect\",\"rc\":%d", rc);
    }
}

/* Arm a connection: remember the target and reset the retry counter, then make the
 * first attempt. Subsequent 0x3e failures retry via ganymede_connect_raw(). */
static void ganymede_connect(const ble_addr_t *addr)
{
    g_target = *addr;
    g_connect_armed = true;
    g_connect_tries = 0;
    ganymede_connect_raw(addr);
}

/* ---- GATT discovery + HID subscription -------------------------------------- */

/* Descriptor discovery: capture the Report char's CCCD (0x2902), then read the
 * report map and subscribe to notifications. */
static int dsc_disc_cb(uint16_t conn, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0 && dsc != NULL) {
        char uu[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&dsc->uuid.u, uu);
        log_json("desc", "\"uuid\":\"%s\",\"handle\":%u", uu, dsc->handle);
        if (g_hid.report_cccd == 0 && ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID) {
            g_hid.report_cccd = dsc->handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        log_json("discovered", "\"report_map_val\":%u,\"report_val\":%u,\"report_cccd\":%u",
                 g_hid.report_map_val, g_hid.report_val, g_hid.report_cccd);
        ganymede_read_report_map(conn);
        ganymede_subscribe(conn);
    } else {
        log_json("error", "\"where\":\"disc_dsc\",\"status\":%d", error->status);
    }
    return 0;
}

/* Characteristic discovery within the HID service: log each, capture the report-map
 * and the notify-capable input-report value handles, then discover descriptors. */
static int chr_disc_cb(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL) {
        char uu[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&chr->uuid.u, uu);
        log_json("char", "\"uuid\":\"%s\",\"def\":%u,\"val\":%u,\"props\":%u",
                 uu, chr->def_handle, chr->val_handle, chr->properties);
        uint16_t u16 = ble_uuid_u16(&chr->uuid.u);
        if (u16 == REPORT_MAP_UUID) {
            g_hid.report_map_val = chr->val_handle;
        } else if (u16 == REPORT_UUID && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
            /* Two 0x2A4D chars exist; the input report is the notify-capable one. */
            g_hid.report_val = chr->val_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (g_hid.report_val != 0) {
            ble_gattc_disc_all_dscs(conn, g_hid.report_val, g_hid.hid_end, dsc_disc_cb, NULL);
        } else {
            log_json("error", "\"where\":\"disc_chr\",\"msg\":\"no notify report char\"");
        }
    } else {
        log_json("error", "\"where\":\"disc_chr\",\"status\":%d", error->status);
    }
    return 0;
}

/* Service discovery: capture the HID service range, then discover its characteristics. */
static int svc_disc_cb(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service != NULL) {
        g_hid.hid_start = service->start_handle;
        g_hid.hid_end = service->end_handle;
        log_json("service", "\"uuid\":\"0x1812\",\"start\":%u,\"end\":%u",
                 service->start_handle, service->end_handle);
    } else if (error->status == BLE_HS_EDONE) {
        if (g_hid.hid_start != 0) {
            ble_gattc_disc_all_chrs(conn, g_hid.hid_start, g_hid.hid_end, chr_disc_cb, NULL);
        } else {
            log_json("error", "\"where\":\"disc_svc\",\"msg\":\"no hid service 0x1812\"");
        }
    } else {
        log_json("error", "\"where\":\"disc_svc\",\"status\":%d", error->status);
    }
    return 0;
}

/* Kick off HID discovery: narrow straight to service 0x1812. */
static void ganymede_discover(uint16_t conn_handle)
{
    memset(&g_hid, 0, sizeof(g_hid));
    log_json("discovering", "\"conn_handle\":%u", conn_handle);
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, BLE_UUID16_DECLARE(HID_SVC_UUID),
                                        svc_disc_cb, NULL);
    if (rc != 0) {
        log_json("error", "\"where\":\"disc_svc_start\",\"rc\":%d", rc);
    }
}

/* Read result callback (report map or manual `read <handle>`): hex-log the value. */
static int read_cb(uint16_t conn, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0 && attr != NULL) {
        uint8_t buf[128];
        uint16_t n = 0;
        ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), &n);
        char hex[2 * sizeof(buf) + 1];
        log_hex(hex, sizeof(hex), buf, n);
        log_json("read", "\"handle\":%u,\"len\":%u,\"value\":\"%s\"", attr->handle, n, hex);
    } else {
        log_json("error", "\"where\":\"read\",\"status\":%d", error->status);
    }
    return 0;
}

static void ganymede_read_report_map(uint16_t conn_handle)
{
    if (g_hid.report_map_val == 0) {
        log_json("error", "\"where\":\"read_report_map\",\"msg\":\"no report map handle\"");
        return;
    }
    int rc = ble_gattc_read(conn_handle, g_hid.report_map_val, read_cb, NULL);
    if (rc != 0) {
        log_json("error", "\"where\":\"read_report_map\",\"rc\":%d", rc);
    }
}

/* CCCD write result. */
static int subscribe_cb(uint16_t conn, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        log_json("subscribed", "\"cccd\":%u,\"value\":\"0100\"", g_hid.report_cccd);
    } else {
        log_json("error", "\"where\":\"subscribe\",\"status\":%d", error->status);
    }
    return 0;
}

/* Enable notifications on the HID input-report characteristic (write 01 00 to its CCCD). */
static void ganymede_subscribe(uint16_t conn_handle)
{
    if (g_hid.report_cccd == 0) {
        log_json("error", "\"where\":\"subscribe\",\"msg\":\"no report cccd (discover first)\"");
        return;
    }
    uint8_t cccd_val[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, g_hid.report_cccd,
                                  cccd_val, sizeof(cccd_val), subscribe_cb, NULL);
    if (rc != 0) {
        log_json("error", "\"where\":\"subscribe\",\"rc\":%d", rc);
    }
}

/* ---- GAP event machine ------------------------------------------------------ */

static int ganymede_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    char s[18];
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:  // legacy advert
        handle_advert(&event->disc.addr, event->disc.rssi,
                      event->disc.data, event->disc.length_data,
                      event->disc.event_type, 0, 0, 0, 0);
        return 0;

#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC:
        handle_advert(&event->ext_disc.addr, event->ext_disc.rssi,
                      event->ext_disc.data, event->ext_disc.length_data,
                      event->ext_disc.legacy_event_type, event->ext_disc.prim_phy,
                      event->ext_disc.sec_phy, event->ext_disc.props,
                      event->ext_disc.data_status);
        return 0;
#endif

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_connect_armed = false;   /* won the establishment lottery; stop retrying */
            g_conn_handle = event->connect.conn_handle;
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            addr_str(s, sizeof(s), desc.peer_id_addr.val);
            log_json("connected", "\"addr\":\"%s\",\"conn_handle\":%d,\"itvl\":%d,\"latency\":%d,"
                     "\"timeout\":%d", s, event->connect.conn_handle,
                     desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);

            /* Immediately pair + bond + encrypt → fires BLE_GAP_EVENT_ENC_CHANGE. */
            rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                log_json("error", "\"where\":\"security_initiate\",\"rc\":%d", rc);
                return ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            log_json("securing", "\"conn_handle\":%d", event->connect.conn_handle);
        } else {
            /* Establishment lottery loss (typically HCI 0x3e). Re-issue the connect
             * until one holds. See docs/esp-idf-nimble-probe-spec.md §4. */
            log_json("connect_failed", "\"status\":%d,\"try\":%d",
                     event->connect.status, g_connect_tries);
            if (g_connect_armed && ++g_connect_tries < PROBE_MAX_ESTABLISH_TRIES) {
                ganymede_connect_raw(&g_target);
            } else if (g_connect_armed) {
                g_connect_armed = false;
                log_json("connect_gaveup", "\"tries\":%d", g_connect_tries);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Milestone 1 success signal: status 0 + encrypted=1 + bonded=1. */
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        assert(rc == 0);
        log_sec_state("enc_change", &desc, event->enc_change.status);
        /* Goal 2: once the link is encrypted, discover the HID service, read the
         * report map, and subscribe to input-report notifications. */
        if (event->enc_change.status == 0 && desc.sec_state.encrypted) {
            ganymede_discover(event->enc_change.conn_handle);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Already bonded but peer wants a fresh pairing: drop the stale bond and retry. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        log_json("repeat_pairing", "\"conn_handle\":%d", event->repeat_pairing.conn_handle);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* Should not fire under Just Works (NO_IO). If it does, that is a finding:
         * the remote demands authenticated pairing → escalate per SPECS.md §8. */
        log_json("passkey_action", "\"conn_handle\":%d,\"action\":%d",
                 event->passkey.conn_handle, event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        log_json("disconnect", "\"reason\":%d", event->disconnect.reason);
        /* CLI-driven: do not auto-reconnect. A later `connect` should re-encrypt
         * WITHOUT re-pairing if the bond persisted in NVS. */
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        log_json("scan_complete", "\"reason\":%d", event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc == 0) {
            log_json("conn_update", "\"status\":%d,\"itvl\":%d,\"latency\":%d,\"timeout\":%d",
                     event->conn_update.status, desc.conn_itvl, desc.conn_latency,
                     desc.supervision_timeout);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        log_json("mtu", "\"conn_handle\":%d,\"mtu\":%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* Incoming HID input report (button press) on the subscribed Report char.
         * 8-byte boot-keyboard report, e.g. 0000010000000000 = Power. */
        uint8_t buf[32];
        uint16_t n = 0;
        ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &n);
        char hex[2 * sizeof(buf) + 1];
        log_hex(hex, sizeof(hex), buf, n);
        log_json("notification", "\"handle\":%u,\"ind\":%u,\"len\":%u,\"value\":\"%s\"",
                 event->notify_rx.attr_handle, event->notify_rx.indication, n, hex);
        return 0;
    }

    default:
        return 0;
    }
}

/* ---- host init -------------------------------------------------------------- */

static void on_reset(int reason)
{
    log_json("ble_reset", "\"reason\":%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    uint8_t addr_val[6] = {0};
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) == 0 &&
        ble_hs_id_copy_addr(own_addr_type, addr_val, NULL) == 0) {
        char s[18];
        addr_str(s, sizeof(s), addr_val);
        log_json("ready", "\"own_addr\":\"%s\",\"addr_type\":%d", s, own_addr_type);
    } else {
        log_event("ready");
    }

    /* CLI-driven: stay idle until the user issues `scan` / `connect`. */
    log_event("ready");
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- serial CLI ------------------------------------------------------------- */

static int cmd_scan(int argc, char **argv)
{
    ganymede_scan();
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    g_connect_armed = false;  // stop the establishment-retry lottery
    int rc_scan = ble_gap_disc_cancel();
    int rc_conn = ble_gap_conn_cancel();  // cancel a pending "connect when seen"
    log_json("stop", "\"scan_rc\":%d,\"conn_rc\":%d", rc_scan, rc_conn);
    return 0;
}

static int cmd_connect(int argc, char **argv)
{
    ble_addr_t addr;
    if (argc >= 2) {
        if (parse_addr(argv[1], &addr) != 0) {
            log_json("error", "\"where\":\"connect\",\"msg\":\"bad address\"");
            return 0;
        }
    } else {
        target_addr(&addr);  // default: Ganymede
    }
    ganymede_connect(&addr);
    log_json("hint", "\"msg\":\"now hold the remote pairing (home) button ~7s to advertise\"");
    return 0;
}

static int cmd_disconnect(int argc, char **argv)
{
    uint16_t h = g_conn_handle;
    if (h == BLE_HS_CONN_HANDLE_NONE) {
        log_json("disconnect", "\"rc\":\"not_connected\"");
        return 0;
    }
    int rc = ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
    log_json("disconnecting", "\"conn_handle\":%d,\"rc\":%d", h, rc);
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    uint16_t h = g_conn_handle;
    if (h == BLE_HS_CONN_HANDLE_NONE) {
        log_json("error", "\"where\":\"pair\",\"msg\":\"not connected\"");
        return 0;
    }
    int rc = ble_gap_security_initiate(h);
    log_json("securing", "\"conn_handle\":%d,\"rc\":%d", h, rc);
    return 0;
}

static int cmd_clear_bonds(int argc, char **argv)
{
    int rc = ble_store_clear();
    log_json("clear_bonds", "\"rc\":%d", rc);
    return 0;
}

static int cmd_discover(int argc, char **argv)
{
    uint16_t h = g_conn_handle;
    if (h == BLE_HS_CONN_HANDLE_NONE) {
        log_json("error", "\"where\":\"discover\",\"msg\":\"not connected\"");
        return 0;
    }
    ganymede_discover(h);
    return 0;
}

static int cmd_read(int argc, char **argv)
{
    uint16_t h = g_conn_handle;
    if (h == BLE_HS_CONN_HANDLE_NONE) {
        log_json("error", "\"where\":\"read\",\"msg\":\"not connected\"");
        return 0;
    }
    if (argc < 2) {
        log_json("error", "\"where\":\"read\",\"msg\":\"usage: read <handle>\"");
        return 0;
    }
    uint16_t handle = (uint16_t)strtol(argv[1], NULL, 0);  // accepts 0x003b or decimal
    int rc = ble_gattc_read(h, handle, read_cb, NULL);
    if (rc != 0) {
        log_json("error", "\"where\":\"read\",\"rc\":%d", rc);
    }
    return 0;
}

static int cmd_subscribe(int argc, char **argv)
{
    uint16_t h = g_conn_handle;
    if (h == BLE_HS_CONN_HANDLE_NONE) {
        log_json("error", "\"where\":\"subscribe\",\"msg\":\"not connected\"");
        return 0;
    }
    ganymede_subscribe(h);
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    uint16_t h = g_conn_handle;
    struct ble_gap_conn_desc desc;
    if (h != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(h, &desc) == 0) {
        char s[18];
        addr_str(s, sizeof(s), desc.peer_id_addr.val);
        log_json("status", "\"conn\":%d,\"peer\":\"%s\",\"encrypted\":%d,\"authenticated\":%d,"
                 "\"bonded\":%d,\"itvl\":%d", h, s, desc.sec_state.encrypted,
                 desc.sec_state.authenticated, desc.sec_state.bonded, desc.conn_itvl);
    } else {
        log_json("status", "\"conn\":-1");
    }

    /* List bonded peers stored in NVS. */
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int num = 0;
    if (ble_store_util_bonded_peers(peers, &num, CONFIG_BT_NIMBLE_MAX_BONDS) == 0) {
        for (int i = 0; i < num; i++) {
            char s[18];
            addr_str(s, sizeof(s), peers[i].val);
            log_json("bond", "\"idx\":%d,\"addr\":\"%s\",\"type\":%d", i, s, peers[i].type);
        }
        if (num == 0) {
            log_json("bond", "\"count\":0");
        }
    }
    return 0;
}

static void register_cli(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "scan",
            .help = "Start BLE scan (logs target match + public advertisers)",
            .func = cmd_scan,
        },
        {
            .command = "stop",
            .help = "Stop scanning",
            .func = cmd_stop,
        },
        {
            .command = "connect",
            .help = "Connect [addr]; defaults to Ganymede, then auto-bonds",
            .func = cmd_connect,
        },
        {
            .command = "disconnect",
            .help = "Terminate the current connection",
            .func = cmd_disconnect,
        },
        {
            .command = "pair",
            .help = "(Re)initiate pairing/bonding on the current connection",
            .func = cmd_pair,
        },
        {
            .command = "clear-bonds",
            .help = "Erase all stored bonds from NVS",
            .func = cmd_clear_bonds,
        },
        {
            .command = "discover",
            .help = "Discover HID service/chars/CCCD on the current connection",
            .func = cmd_discover,
        },
        {
            .command = "read",
            .help = "Read a GATT value handle: read <handle> (e.g. read 0x003b)",
            .func = cmd_read,
        },
        {
            .command = "subscribe",
            .help = "Enable HID report notifications (write 01 00 to the report CCCD)",
            .func = cmd_subscribe,
        },
        {
            .command = "status",
            .help = "Show connection + security state and bonded peers",
            .func = cmd_status,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void app_main(void)
{
    /* NVS holds PHY calibration and the persisted BLE bonds. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Just Works pairing to the remote: LEGACY (not SC), bonding, no MITM, no IO.
     * Ground truth (logs/ganymede-btsnoop3.log + reproduced on Linux): the remote's
     * Pairing Response is AuthReq=0x01 (Bonding; SC=0, MITM=0), IO=NoInputNoOutput,
     * and it distributes LTK+IRK+CSRK. Force legacy (sm_sc=0) to match it and accept
     * the CSRK it offers. See docs/esp-idf-nimble-probe-spec.md. */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("ganymede-probe");

    ble_store_config_init();

    nimble_port_freertos_init(host_task);

    /* Interactive CLI over the USB-Serial/JTAG console. */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ganymede>";
    repl_config.max_cmdline_length = 128;
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
    esp_console_register_help_command();
    register_cli();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    log_json("boot", "\"target\":\"%s\",\"target_addr\":\"00:A0:50:XX:XX:XX\","
             "\"cmds\":\"scan|stop|connect|disconnect|pair|clear-bonds|discover|read|subscribe|status|help\"",
             TARGET_NAME);
}
#endif /* !APP_EMULATOR */
