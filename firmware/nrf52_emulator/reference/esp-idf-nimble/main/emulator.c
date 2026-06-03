#ifdef APP_EMULATOR
/*
 * Ganymede remote EMULATOR — Phase 2.
 *
 * The ESP32-S3 acts as a BLE HID peripheral that impersonates the De'Longhi "Ganymede"
 * remote: same device name, keyboard appearance, captured HID Report Map, and
 * Environmental Sensing service. It uses the ESP32's own address by default so
 * the A/C creates a fresh bond; CLONE_MAC can be enabled for stale-cache
 * diagnostics. On serial-CLI command it notifies the exact 8-byte button
 * reports we reverse-engineered (see docs/android-capture-findings.md).
 *
 * Environmental values come from env_source (stub now, BME280 later); the local screen comes
 * from display (no-op now, I2C OLED later). I2C is intentionally not wired yet.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_console.h"

#include "logger.h"
#include "env_source.h"
#include "display.h"

static const char *TAG = "ganymede-emu";

void ble_store_config_init(void);

/* Cloned identity (public MAC, standard byte order). */
static const uint8_t GANYMEDE_MAC[6] = {0x00, 0xA0, 0x50, 0x00, 0x00, 0x00} /* set lower 3 bytes to your remote's address (Cypress OUI 00:A0:50) */;
#define DEVICE_NAME      "Ganymede"
#define APPEARANCE_KBD   0x03C1
#ifdef CLONE_MAC
#define ADDR_POLICY      "clone"
#else
#define ADDR_POLICY      "own"
#endif

static uint8_t  g_own_addr_type;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
/* Set when the A/C's pairing needs us to INPUT a passkey it displays; cleared after inject. */
static uint16_t g_passkey_conn = BLE_HS_CONN_HANDLE_NONE;

/* Filled at GATT registration — value handles we notify on. */
static uint16_t g_hid_report_handle;
static uint16_t g_temp_handle, g_humid_handle, g_press_handle, g_batt_handle;

/* Mutable characteristic state. */
static uint8_t g_protocol_mode = 0x01;  /* 0x2A4E: 1 = Report mode */
static uint8_t g_battery_level = 100;

/* Captured HID Report Map (0x2A4B) — standard 8-byte keyboard report. */
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0xc0,
};

/* HID Information (0x2A4A): bcdHID=0x0111, country=0, flags=0x02 (NormallyConnectable). */
static const uint8_t HID_INFO[] = {0x11, 0x01, 0x00, 0x02};

/* Report Reference (0x2908) for the input report: report ID 0, type 1 (Input). */
static const uint8_t REPORT_REF_INPUT[] = {0x00, 0x01};

/* Button -> report byte2/byte3 bitmask (from the decoded protocol). */
typedef struct { const char *name; uint8_t b2; uint8_t b3; } button_t;
static const button_t BUTTONS[] = {
    {"power",  0x01, 0x00},
    {"down",   0x02, 0x00},
    {"up",     0x04, 0x00},
    {"mode",   0x08, 0x00},
    {"eco",    0x10, 0x00},
    {"timer",  0x20, 0x00},
    {"fan",    0x40, 0x00},
    {"silent", 0x80, 0x00},
    {"flap",   0x00, 0x01},
};
#define NUM_BUTTONS (sizeof(BUTTONS) / sizeof(BUTTONS[0]))

/* Real remote emits ~2 notifications per physical press. */
#define REPORT_REPEAT 2

/* ---- GATT access callbacks -------------------------------------------------- */

static int chr_read_flat(struct ble_gatt_access_ctxt *ctxt, const void *data, uint16_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Device Information would go here if needed; the real remote returned empty strings,
 * so we keep DIS minimal/omitted and rely on the HID + Env + Battery services. */

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid = ble_uuid_u16(ctxt->chr ? ctxt->chr->uuid : ctxt->dsc->uuid);
    env_reading_t e = env_source_get();

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        switch (uuid) {
        case 0x2A4B: return chr_read_flat(ctxt, HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
        case 0x2A4A: return chr_read_flat(ctxt, HID_INFO, sizeof(HID_INFO));
        case 0x2A4E: return chr_read_flat(ctxt, &g_protocol_mode, 1);
        case 0x2A4D: { uint8_t z[8] = {0}; return chr_read_flat(ctxt, z, sizeof(z)); }
        case 0x2A22: { uint8_t z[8] = {0}; return chr_read_flat(ctxt, z, sizeof(z)); }
        case 0x2A19: return chr_read_flat(ctxt, &g_battery_level, 1);
        case 0x2A6E: return chr_read_flat(ctxt, &e.temp_c_x100, 2);   /* sint16 LE */
        case 0x2A6F: return chr_read_flat(ctxt, &e.humidity_x100, 2); /* uint16 LE */
        case 0x2A6D: return chr_read_flat(ctxt, &e.pressure_x10, 4);  /* uint32 LE */
        default: return BLE_ATT_ERR_UNLIKELY;
        }
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        switch (uuid) {
        case 0x2A4E: /* Protocol Mode */
            if (OS_MBUF_PKTLEN(ctxt->om) >= 1) os_mbuf_copydata(ctxt->om, 0, 1, &g_protocol_mode);
            log_json("proto_mode_write", "\"mode\":%d", g_protocol_mode);
            return 0;
        case 0x2A4C: /* HID Control Point */
            log_json("hid_ctrl_write", "\"len\":%d", OS_MBUF_PKTLEN(ctxt->om));
            return 0;
        default: return 0;
        }
    case BLE_GATT_ACCESS_OP_READ_DSC:
        if (uuid == 0x2908) return chr_read_flat(ctxt, REPORT_REF_INPUT, sizeof(REPORT_REF_INPUT));
        return BLE_ATT_ERR_UNLIKELY;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ---- GATT service table ----------------------------------------------------- */

#define U16(x) BLE_UUID16_DECLARE(x)

static const struct ble_gatt_svc_def gatt_svcs[] = {
    { /* Battery */
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = U16(0x2A19), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_batt_handle },
            { 0 },
        },
    },
    { /* Environmental Sensing */
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x181A),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = U16(0x2A6E), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_temp_handle },
            { .uuid = U16(0x2A6F), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_humid_handle },
            { .uuid = U16(0x2A6D), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_press_handle },
            { 0 },
        },
    },
    { /* Human Interface Device */
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = U16(0x2A4A), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A4B), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A4C), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = U16(0x2A4E), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = U16(0x2A4D), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_hid_report_handle,
              .descriptors = (struct ble_gatt_dsc_def[]){
                  { .uuid = U16(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = gatt_access },
                  { 0 },
              },
            },
            { .uuid = U16(0x2A22), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

/* ---- advertising ------------------------------------------------------------ */

static int emu_gap_event(struct ble_gap_event *event, void *arg);

static void emu_advertise(void)
{
    /* Match the pairing-mode advertising burst seen in ac-btsnoop.log:
     * primary ADV_IND carries Flags + incomplete UUID16 list + Appearance;
     * SCAN_RSP carries the complete local name "Ganymede". */
    /* Put the COMPLETE name in the PRIMARY ADV_IND (matches the real remote's live advert
     * and how most HID hosts scan). flags(3)+name(10)+uuids16 x3(8)+appearance(4)=25B < 31B.
     * The A/C connects to an Android emulator but not this one; a name only in the scan
     * response is invisible to a host that doesn't active-scan or filters on the primary PDU. */
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)DEVICE_NAME;
    adv.name_len = strlen(DEVICE_NAME);
    adv.name_is_complete = 1;
    adv.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0x180A), BLE_UUID16_INIT(0x180F),
                                    BLE_UUID16_INIT(0x181A) };
    adv.num_uuids16 = 3;
    adv.uuids16_is_complete = 0;
    adv.appearance = APPEARANCE_KBD;
    adv.appearance_is_present = 1;
    /* Cypress manufacturer data (company 0x0131 LE = 31 01, payload 3b 04). The working
     * nRF-Connect emulator the A/C pairs with advertises exactly `05 ff 31 01 3b 04`, and the
     * real remote carries the same Cypress mfg data — the A/C's pairing scan filters on it, so
     * without it the A/C ignores the emulator. flags(3)+name(10)+uuids(8)+appearance(4)+mfg(6)=31B. */
    static const uint8_t cypress_mfg[4] = {0x31, 0x01, 0x3b, 0x04};
    adv.mfg_data = (uint8_t *)cypress_mfg;
    adv.mfg_data_len = sizeof(cypress_mfg);

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) { log_json("error", "\"where\":\"adv_set_fields\",\"rc\":%d", rc); return; }

    /* Also carry the name in the scan response (redundant, covers active-scanning hosts). */
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { log_json("error", "\"where\":\"adv_rsp_set_fields\",\"rc\":%d", rc); return; }

    if (ble_gap_adv_active()) {
        rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            log_json("error", "\"where\":\"adv_stop\",\"rc\":%d", rc);
        }
    }

    /* Fast, connectable-undirected advertising so the A/C's short pairing-mode scan catches
     * us immediately (20-40 ms; 0.625 ms units). */
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x0020;  /* 20 ms */
    params.itvl_max = 0x0040;  /* 40 ms */
    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &params, emu_gap_event, NULL);
    if (rc != 0) { log_json("error", "\"where\":\"adv_start\",\"rc\":%d", rc); return; }
    log_event("advertising");
}

/* ---- GAP events ------------------------------------------------------------- */

static int emu_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ble_gap_conn_find(event->connect.conn_handle, &desc);
            log_json("connected", "\"conn_handle\":%d,\"encrypted\":%d,\"bonded\":%d",
                     event->connect.conn_handle, desc.sec_state.encrypted, desc.sec_state.bonded);
            display_status("connected");
        } else {
            log_json("connect_failed", "\"status\":%d", event->connect.status);
            emu_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        log_json("disconnect", "\"reason\":%d", event->disconnect.reason);
        display_status("disconnected");
        emu_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        log_json("enc_change", "\"status\":%d,\"encrypted\":%d,\"bonded\":%d",
                 event->enc_change.status, desc.sec_state.encrypted, desc.sec_state.bonded);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        /* Tells us when the central (A/C) subscribes to our HID report / env notifications. */
        log_json("subscribe", "\"attr_handle\":%d,\"notify\":%d,\"indicate\":%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        /* The A/C requires authenticated (MITM) pairing. Handle whichever method SMP picked. */
        int act = event->passkey.params.action;
        log_json("passkey_action", "\"action\":%d", act);
        struct ble_sm_io io = {0};
        if (act == BLE_SM_IOACT_NUMCMP) {
            /* LE Secure Connections numeric comparison: both sides show the same number and
             * the user confirms. We have no display, so auto-accept (== pressing "Pair"). */
            io.action = BLE_SM_IOACT_NUMCMP;
            io.numcmp_accept = 1;
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            log_json("passkey_numcmp", "\"value\":%lu,\"accept\":1,\"rc\":%d",
                     (unsigned long)event->passkey.params.numcmp, rc);
        } else if (act == BLE_SM_IOACT_DISP) {
            /* We must display a passkey for the user to enter on the A/C. */
            io.action = BLE_SM_IOACT_DISP;
            io.passkey = 123456;
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            log_json("passkey_display", "\"enter_on_ac\":123456,\"rc\":%d", rc);
        } else if (act == BLE_SM_IOACT_INPUT) {
            /* The A/C displays a passkey; the user must give it to us: `passkey <n>`. */
            g_passkey_conn = event->passkey.conn_handle;
            log_json("passkey_input_needed",
                     "\"msg\":\"read the A/C display, then run: passkey <number>\"");
        }
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        emu_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        log_json("mtu", "\"mtu\":%d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ---- sending button reports ------------------------------------------------- */

static int emu_send_report(uint8_t b2, uint8_t b3)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        log_json("press", "\"rc\":\"not_connected\"");
        return -1;
    }
    uint8_t report[8] = {0, 0, b2, b3, 0, 0, 0, 0};
    int rc = 0;
    for (int i = 0; i < REPORT_REPEAT; i++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
        if (!om) { rc = BLE_HS_ENOMEM; break; }
        rc = ble_gatts_notify_custom(g_conn_handle, g_hid_report_handle, om);
        if (rc != 0) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    char hex[20];
    log_json("press", "\"report\":\"%s\",\"rc\":%d", log_hex(hex, sizeof(hex), report, 8), rc);
    return rc;
}

/* ---- serial CLI ------------------------------------------------------------- */

static int cmd_press(int argc, char **argv)
{
    if (argc < 2) { log_json("error", "\"msg\":\"usage: press <button>\""); return 0; }
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (strcmp(argv[1], BUTTONS[i].name) == 0) {
            emu_send_report(BUTTONS[i].b2, BUTTONS[i].b3);
            return 0;
        }
    }
    log_json("error", "\"msg\":\"unknown button\",\"valid\":\"power down up mode eco timer fan silent flap\"");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    env_reading_t e = env_source_get();
    log_json("status", "\"conn\":%d,\"proto_mode\":%d,\"battery\":%d,"
             "\"temp_c\":%.2f,\"humidity\":%.2f,\"pressure_hpa\":%.2f",
             (int)(int16_t)g_conn_handle, g_protocol_mode, g_battery_level,
             e.temp_c_x100 / 100.0, e.humidity_x100 / 100.0, e.pressure_x10 / 10000.0);
    return 0;
}

static int cmd_adv(int argc, char **argv) { emu_advertise(); return 0; }

static int cmd_clear_bonds(int argc, char **argv)
{
    int rc = ble_store_clear();
    log_json("clear_bonds", "\"rc\":%d", rc);
    return 0;
}

static int cmd_env(int argc, char **argv)
{
    if (argc < 4) { log_json("error", "\"msg\":\"usage: env <tempC> <humidity> <hPa>\""); return 0; }
    env_source_set_stub((int16_t)(atof(argv[1]) * 100), (uint16_t)(atof(argv[2]) * 100),
                        (uint32_t)(atof(argv[3]) * 10000));
    cmd_status(0, NULL);
    return 0;
}

static int cmd_passkey(int argc, char **argv)
{
    if (g_passkey_conn == BLE_HS_CONN_HANDLE_NONE) {
        log_json("error", "\"where\":\"passkey\",\"msg\":\"no pairing is waiting for a passkey\"");
        return 0;
    }
    if (argc < 2) { log_json("error", "\"msg\":\"usage: passkey <6-digit number from A/C>\""); return 0; }
    struct ble_sm_io io = {0};
    io.action = BLE_SM_IOACT_INPUT;
    io.passkey = (uint32_t)strtoul(argv[1], NULL, 10);
    int rc = ble_sm_inject_io(g_passkey_conn, &io);
    log_json("passkey_injected", "\"passkey\":%lu,\"rc\":%d", (unsigned long)io.passkey, rc);
    g_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
    return 0;
}

static void register_cli(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "press",
            .help = "Send a button report: power|down|up|mode|eco|timer|fan|silent|flap",
            .func = cmd_press,
        },
        {
            .command = "passkey",
            .help = "Enter the passkey the A/C displays during pairing: passkey <number>",
            .func = cmd_passkey,
        },
        {
            .command = "status",
            .help = "Show connection + state + env",
            .func = cmd_status,
        },
        {
            .command = "adv",
            .help = "(Re)start advertising",
            .func = cmd_adv,
        },
        {
            .command = "clear-bonds",
            .help = "Erase stored bonds",
            .func = cmd_clear_bonds,
        },
        {
            .command = "env",
            .help = "Set stub env: env <tempC> <hum%> <hPa>",
            .func = cmd_env,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}

/* ---- host init -------------------------------------------------------------- */

static void on_reset(int reason) { log_json("ble_reset", "\"reason\":%d", reason); }

static void on_sync(void)
{
    /* Use whichever public address policy app_main configured. */
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    assert(rc == 0);

    uint8_t a[6] = {0};
    ble_hs_id_copy_addr(g_own_addr_type, a, NULL);
    log_json("ready", "\"addr\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"addr_type\":%d",
             a[5], a[4], a[3], a[2], a[1], a[0], g_own_addr_type);

    emu_advertise();
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE emulator host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* MAC clone is OFF by default: the A/C accepted the tablet's clone which used the phone's
     * OWN address, and cloning the real remote's MAC may collide with a stale bond on the A/C.
     * Advertise as "Ganymede" with the ESP32's own public address so the A/C pairs it fresh.
     * Build with -DCLONE_MAC to re-enable the MAC clone. */
#ifdef CLONE_MAC
    ret = esp_iface_mac_addr_set(GANYMEDE_MAC, ESP_MAC_BT);
    if (ret != ESP_OK) ESP_LOGW(TAG, "esp_iface_mac_addr_set(BT) rc=%d (continuing)", ret);
#endif
    (void)GANYMEDE_MAC;

    env_source_init();
    display_init();

    ret = nimble_port_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed: %d", ret); return; }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* CLONE THE REAL REMOTE'S PAIRING EXACTLY. Ground truth from the Android HCI snoop
     * (logs bugreport, 196k frames, ZERO SMP failures): when a central pairs to the real
     * remote, the remote (peripheral) responds IO=NoInputNoOutput, AuthReq=0x01
     * (Bonding only; SC=0, MITM=0) -> LEGACY Just Works, and the central accepts the
     * downgrade. So the emulator responds the SAME: NoInputNoOutput, legacy, no MITM, no SC,
     * bonding, distributing LTK+IRK+CSRK (key dist 0x07). The earlier MITM/SC=1 config was
     * based on a misread ("A/C requires MITM") that the snoop disproves. */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(APPEARANCE_KBD);

    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));

    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc_cfg.prompt = "ganymede-emu>";
    rc_cfg.max_cmdline_length = 128;
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc_cfg, &repl));
    esp_console_register_help_command();
    register_cli();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    log_json("boot", "\"role\":\"emulator\",\"name\":\"%s\",\"addr_policy\":\"%s\","
             "\"clone_mac\":\"00:A0:50:XX:XX:XX\","
             "\"cmds\":\"press|status|adv|clear-bonds|env|help\"", DEVICE_NAME, ADDR_POLICY);
}
#endif /* APP_EMULATOR */
