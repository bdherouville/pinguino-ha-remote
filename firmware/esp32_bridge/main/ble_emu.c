/*
 * Ganymede remote emulator — NimBLE, running on the bridge's own ESP32-S3 radio.
 *
 * Impersonates the De'Longhi "Ganymede" BLE HID remote so the AC (which is the BLE central)
 * connects to us and accepts our button reports. Ported from the field-verified Zephyr
 * emulator that ran on the nRF52840 (firmware/nrf52_emulator/zephyr/src/main.c); everything
 * on-air here is traced to a capture in docs/ganymede_protocol.md.
 *
 * Four things have to be right or the AC will not pair. The archived ESP32 attempt
 * (firmware/nrf52_emulator/reference/esp-idf-nimble/) got the first two wrong, which is why
 * it was believed the ESP32's radio could not do this at all:
 *
 *   1. ADVERTISING FLAGS. With no bond we must advertise Flags 0x01 = LE *Limited*
 *      Discoverable. That is the "I am a remote in pairing mode" signal the AC's pairing scan
 *      looks for. General Discoverable (0x06) gets connected but never paired. Once bonded we
 *      switch to 0x06 for the normal encrypted reconnect.
 *   2. CYPRESS MANUFACTURER DATA IN THE SCAN RESPONSE, not in the primary ADV_IND. The real
 *      remote puts `31 01 3b 04` (company 0x0131 = Cypress) in its SCAN_RSP and the AC
 *      SCAN_REQs to read it.
 *   3. WE MUST ASK FOR SECURITY. On connect the peripheral sends an SMP Security Request. The
 *      AC waits for the remote to ask; without it we connect and sit there unpaired forever.
 *   4. NOTIFY WITHOUT A SUBSCRIPTION. The AC never writes our HID report CCCD, so we push the
 *      report unconditionally with ble_gatts_notify_custom() (which, unlike
 *      ble_gatts_chr_updated(), does not consult the CCC). This is why the Zephyr port needed
 *      a force-subscribe hack and we do not.
 *
 * The press pacing / sync-window / state-reflection logic below is carried over unchanged from
 * the old uart_link.c — only the transport changed (a GATT notify instead of a UART line).
 */
#include "ble_emu.h"
#include "led_status.h"
#include "wifi_mgr.h"
#include "mqtt_ha.h"
#include "ac_state.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "bleemu";

void ble_store_config_init(void);   // provided by the NimBLE store_config module

#define DEVICE_NAME     "Ganymede"
#define APPEARANCE_KBD  0x03C1      // HID Keyboard, as the real remote advertises

// Where the real remote keeps its HID input report value. The AC walks our whole GATT, so this
// is belt-and-braces rather than a proven requirement — but the Zephyr build that pairs does
// land here, so we reproduce it. Verified at boot; see ENV_PAD below.
#define TARGET_REPORT_HANDLE 0x003B

/* ---- captured constants (verbatim from the captures) ----------------------------------- */

// HID Report Map (0x2A4B), 61 bytes: a standard boot-keyboard map, one 8-byte input report
// with no Report ID. docs/ganymede_protocol.md.
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0xc0,
};
static const uint8_t HID_INFO[]         = { 0x11, 0x01, 0x00, 0x02 };  // bcdHID 0x0111, flags 0x02
static const uint8_t REPORT_REF_INPUT[] = { 0x00, 0x01 };              // report ID 0, type Input

// Button -> report byte2/byte3 bitmask. Report is {0,0,b2,b3,0,0,0,0}, sent twice per press
// with no key-release, exactly like the real remote.
//
// NOTE ON UP/DOWN: the protocol doc's table (derived from press-counting an Android HCI snoop)
// lists down=0x02 / up=0x04. The field-verified firmware that actually drives the AC uses the
// opposite, and that is what is reproduced here — the doc's UP/DOWN labels are inverted.
struct button { const char *name; uint8_t b2, b3; };
static const struct button BUTTONS[] = {
    { "power",  0x01, 0x00 },
    { "down",   0x04, 0x00 },
    { "up",     0x02, 0x00 },
    { "mode",   0x08, 0x00 },
    { "eco",    0x10, 0x00 },
    { "timer",  0x20, 0x00 },
    { "fan",    0x40, 0x00 },
    { "silent", 0x80, 0x00 },
    { "flap",   0x00, 0x01 },
};
#define NUM_BUTTONS   (sizeof(BUTTONS) / sizeof(BUTTONS[0]))
#define REPORT_REPEAT 2

/* ---- mutable characteristic state ------------------------------------------------------ */

static uint8_t  s_protocol_mode = 0x01;   // 0x2A4E: 1 = Report mode
static uint8_t  s_battery_level = 100;
static int16_t  s_temp_x100     = 2350;   // 0x2A6E sint16 LE, 1/100 °C
static uint16_t s_humid_x100    = 5000;   // 0x2A6F uint16 LE, 1/100 %
static uint32_t s_press_x10     = 1013000;// 0x2A6D uint32 LE, 1/10 Pa

// Device Information strings. The real remote's values were only read after encryption and
// never decoded, so these are placeholders — their verified job is to occupy the same handles
// (0x000C..0x001A) as the real remote's DIS. Status: inferred.
static const char DIS_MANUF[]  = "Cypress";
static const char DIS_MODEL[]  = "Ganymede";
static const char DIS_HWREV[]  = "1";
static const char DIS_SERIAL[] = "0";
static const char DIS_FWREV[]  = "1.0";
static const uint8_t DIS_SYSTEM_ID[8] = { 0 };
// PnP ID: vendor ID source 0x01 (Bluetooth SIG), VID 0x0131 (Cypress), PID 0, version 1.0.0.
static const uint8_t DIS_PNP_ID[7] = { 0x01, 0x31, 0x01, 0x00, 0x00, 0x00, 0x01 };

/* ---- link state ------------------------------------------------------------------------ */

static volatile emu_state_t s_state = EMU_OFFLINE;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_secure;             // encrypted + bonded
static volatile bool s_report_subscribed;  // AC wrote our report CCCD (it normally never does)

static uint16_t s_hid_report_handle;
static uint16_t s_batt_handle, s_temp_handle, s_humid_handle, s_press_handle;

static const char *STATE_STR[] = {
    [EMU_OFFLINE]="offline", [EMU_BOOT]="boot", [EMU_ADVERTISING]="advertising",
    [EMU_CONNECTED]="connected", [EMU_READY]="ready", [EMU_ERROR]="error",
};

// Relay-capable. ble_gatts_notify_custom() would in fact deliver as soon as we are connected,
// but we keep the old contract (bonded, or the AC actually subscribed) so the LED and the HA
// diagnostic sensor mean the same thing they did with the nRF.
static void recompute_state(void)
{
    // OFFLINE means the BLE host never came up — nothing below can be true yet. EMU_ERROR is
    // deliberately NOT sticky: a live connection is proof the radio is working, and latching
    // the error there once left the emulator showing "error" while it was bonded to the AC.
    if (s_state == EMU_OFFLINE) return;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        if (s_state != EMU_ERROR) s_state = EMU_ADVERTISING;
    } else if (s_secure || s_report_subscribed) {
        s_state = EMU_READY;
    } else {
        s_state = EMU_CONNECTED;
    }
}

/* ---- GATT access ----------------------------------------------------------------------- */

static int flat(struct ble_gatt_access_ctxt *ctxt, const void *data, uint16_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *u = ctxt->chr ? ctxt->chr->uuid : ctxt->dsc->uuid;
    uint16_t uuid = ble_uuid_u16(u);
    uint8_t zero8[8] = { 0 };

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        switch (uuid) {
        // Human Interface Device
        case 0x2A4B: return flat(ctxt, HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
        case 0x2A4A: return flat(ctxt, HID_INFO, sizeof(HID_INFO));
        case 0x2A4E: return flat(ctxt, &s_protocol_mode, 1);
        case 0x2A4D: return flat(ctxt, zero8, sizeof(zero8));   // input report, idle
        case 0x2A22: return flat(ctxt, zero8, sizeof(zero8));   // boot keyboard input
        case 0x2A32: return flat(ctxt, zero8, 1);               // boot keyboard output (LEDs)
        // Battery / Environmental Sensing
        case 0x2A19: return flat(ctxt, &s_battery_level, 1);
        case 0x2A6E: return flat(ctxt, &s_temp_x100, sizeof(s_temp_x100));
        case 0x2A6F: return flat(ctxt, &s_humid_x100, sizeof(s_humid_x100));
        case 0x2A6D: return flat(ctxt, &s_press_x10, sizeof(s_press_x10));
        // Device Information
        case 0x2A29: return flat(ctxt, DIS_MANUF,  strlen(DIS_MANUF));
        case 0x2A24: return flat(ctxt, DIS_MODEL,  strlen(DIS_MODEL));
        case 0x2A27: return flat(ctxt, DIS_HWREV,  strlen(DIS_HWREV));
        case 0x2A25: return flat(ctxt, DIS_SERIAL, strlen(DIS_SERIAL));
        case 0x2A26: return flat(ctxt, DIS_FWREV,  strlen(DIS_FWREV));
        case 0x2A23: return flat(ctxt, DIS_SYSTEM_ID, sizeof(DIS_SYSTEM_ID));
        case 0x2A50: return flat(ctxt, DIS_PNP_ID, sizeof(DIS_PNP_ID));
        default:     return BLE_ATT_ERR_UNLIKELY;
        }

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (uuid == 0x2A4E && OS_MBUF_PKTLEN(ctxt->om) >= 1) {
            os_mbuf_copydata(ctxt->om, 0, 1, &s_protocol_mode);
            ESP_LOGI(TAG, "AC set Protocol Mode = %d", s_protocol_mode);
        } else if (uuid == 0x2A4C) {
            ESP_LOGI(TAG, "AC wrote HID Control Point");
        }
        return 0;   // 0x2A32 (LED output) and anything else: accept and ignore

    case BLE_GATT_ACCESS_OP_READ_DSC:
        if (uuid == 0x2908) return flat(ctxt, REPORT_REF_INPUT, sizeof(REPORT_REF_INPUT));
        if (uuid == 0x2901) return flat(ctxt, zero8, 1);   // handle-alignment padding
        return BLE_ATT_ERR_UNLIKELY;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ---- GATT table ------------------------------------------------------------------------ */

#define U16(x) BLE_UUID16_DECLARE(x)

// ---- handle-alignment padding: ONE number to tune, and it is checked at boot -------------
//
// NimBLE numbers handles in registration order, so where the HID report value lands depends
// on how many handles the services registered before it consume. These no-op Characteristic
// User Description (0x2901) descriptors pad the Environmental Sensing service so the HID
// service starts at 0x0037 and its report value at TARGET_REPORT_HANDLE, like the real remote.
//
// Derivation for ESP-IDF's *default* NimBLE options (each verified in Kconfig.in / the
// NimBLE sources — every one of these can move the answer, which is why it is checked at boot):
//   GAP  0x1800 :  5  = svc + Device Name + Appearance.
//                       No PPCP (all four BT_NIMBLE_SVC_GAP_PPCP_* default to 0, and
//                       PPCP_ENABLED is the OR of them) and no Central Address Resolution
//                       (BT_NIMBLE_SVC_GAP_CAR_CHAR_NOT_SUPP is the default choice -> -1,
//                       and the source only emits the characteristic when the value is >= 0).
//   GATT 0x1801 :  8  = svc + Service Changed (INDICATE, so +1 auto CCCD) + Server Supported
//                       Features + Client Supported Features. Add 2 if you turn on
//                       BT_NIMBLE_GATT_CACHING (default off), which appends a Database Hash.
//   DIS  0x180A : 15  = svc + 7 read-only characteristics.
//   BAS  0x180F :  3  = svc + one READ-ONLY characteristic. Adding NOTIFY here would make it 4
//                       (NimBLE appends the CCCD automatically) and shift everything by one.
//   ESS  0x181A : 10 + ENV_PAD_N = svc + 3 NOTIFY characteristics (each +1 auto CCCD) + padding.
// 5 + 8 + 15 + 3 = 31, so Env starts at handle 32 and the HID service lands at 42 + ENV_PAD_N.
// For that to be 0x0037 (55): ENV_PAD_N = 13.
//
// IS ANY OF THIS NECESSARY? Unproven. The AC walks our GATT and discovers by UUID, and the
// only evidence for the exact layout mattering is that the firmware which does pair happens to
// have it. So if the AC will not pair, try ENV_PAD_N = 0 as well — that is a cheaper
// experiment than chasing the handle number, and it removes 13 junk descriptors that the AC
// otherwise reads during discovery.
#define ENV_PAD_N   13
#define ENV_PAD_MAX 40

// File-scope so the pointer stays valid: a BLE_UUID16_DECLARE compound literal written from
// inside a function would be a dangling pointer once that function returned.
static const ble_uuid16_t uuid_pad_dsc = BLE_UUID16_INIT(0x2901);

// Built by env_pad_build() before the table is registered. A NULL uuid terminates the list, so
// ENV_PAD_N = 0 correctly means "no padding descriptors at all".
static struct ble_gatt_dsc_def env_pad[ENV_PAD_MAX + 1];

static void env_pad_build(void)
{
    int n = ENV_PAD_N > ENV_PAD_MAX ? ENV_PAD_MAX : ENV_PAD_N;
    for (int i = 0; i < n; i++) {
        env_pad[i].uuid = &uuid_pad_dsc.u;
        env_pad[i].att_flags = BLE_ATT_F_READ;
        env_pad[i].access_cb = gatt_access;
    }
    memset(&env_pad[n], 0, sizeof(env_pad[n]));
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    { /* Device Information — 0x000C..0x001A on the real remote (7 characteristics) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = U16(0x2A29), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A24), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A27), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A25), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A26), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A23), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A50), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { /* Battery — 0x001B..0x001D: read-only, so NimBLE adds no CCCD and the span is 3 */
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = U16(0x2A19), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ,
              .val_handle = &s_batt_handle },
            { 0 },
        },
    },
    { /* Environmental Sensing — 0x001E..0x0036, padded to push HID to 0x0037 */
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x181A),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = U16(0x2A6E), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_temp_handle },
            { .uuid = U16(0x2A6F), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_humid_handle },
            { .uuid = U16(0x2A6D), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_press_handle,
              .descriptors = env_pad },
            { 0 },
        },
    },
    { /* Human Interface Device — 0x0037.., report value at 0x003B (= service handle + 4).
       * Characteristic order matters: HID Information first, then the input Report, so the
       * report's value handle lands 4 past the service declaration. */
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = U16(0x2A4A), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A4D), .access_cb = gatt_access,           /* + auto CCCD, + 0x2908 */
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_hid_report_handle,
              .descriptors = (struct ble_gatt_dsc_def[]){
                  { .uuid = U16(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = gatt_access },
                  { 0 },
              },
            },
            { .uuid = U16(0x2A4B), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A4C), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = U16(0x2A4E), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = U16(0x2A22), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = U16(0x2A32), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                       BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 },
        },
    },
    { 0 },
};

// Report where the HID service and its report value actually landed and, if that is not where
// the real remote keeps them, print the exact ENV_PAD_N to use. One rebuild converges it.
static void check_handle_alignment(void)
{
    uint16_t hid_svc = 0;
    ble_gatts_find_svc(U16(0x1812), &hid_svc);

    ESP_LOGI(TAG, "GATT: HID service @ 0x%04x, report value @ 0x%04x (target 0x%04x), "
                  "ENV_PAD_N = %d", hid_svc, s_hid_report_handle, TARGET_REPORT_HANDLE,
             ENV_PAD_N);

    if (s_hid_report_handle == TARGET_REPORT_HANDLE) return;

    int delta = (int)TARGET_REPORT_HANDLE - (int)s_hid_report_handle;
    int want = ENV_PAD_N + delta;
    if (want < 0 || want > ENV_PAD_MAX) {
        ESP_LOGW(TAG, "report handle is off by %+d, which padding alone cannot fix "
                      "(would need ENV_PAD_N = %d). Check the GATT table itself.", delta, want);
    } else {
        ESP_LOGW(TAG, "report handle is off by %+d — set ENV_PAD_N = %d in ble_emu.c and "
                      "rebuild to match the real remote's layout", delta, want);
    }
    ESP_LOGW(TAG, "this may pair anyway (the AC discovers by UUID); if it does not, try the "
                  "corrected ENV_PAD_N, and also try ENV_PAD_N = 0");
}

/* ---- advertising ----------------------------------------------------------------------- */

// No bond -> Flags 0x01 (LE Limited Discoverable) = pairing mode, which is what the AC's
// pairing scan looks for. Bonded -> 0x06 (General + BR/EDR-not-supported) for reconnect.
// Note the pairing byte is 0x01 exactly, with no BR/EDR-not-supported bit, matching the real
// remote on air.
#define ADV_FLAGS_PAIRING 0x01
#define ADV_FLAGS_BONDED  (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP)

// Cypress manufacturer data: company ID 0x0131 little-endian + the remote's key bytes.
// Goes in the SCAN RESPONSE only -> on air `05 ff 31 01 3b 04`.
static const uint8_t CYPRESS_MFG[4] = { 0x31, 0x01, 0x3b, 0x04 };

static uint8_t s_own_addr_type;

static bool emu_has_bond(void)
{
    int count = 0;
    return ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count) == 0 && count > 0;
}

static int emu_gap_event(struct ble_gap_event *event, void *arg);

static void emu_advertise(void)
{
    // With CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1, connectable advertising cannot start while a
    // connection is up: ble_gap_adv_start() checks ble_hs_conn_can_alloc() and returns
    // BLE_HS_ENOMEM. That is correct and expected — the real remote also serves one central at
    // a time — so don't attempt it and don't report it as a failure.
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGD(TAG, "not advertising: already connected (conn=%d)", s_conn);
        return;
    }

    bool bonded = emu_has_bond();

    // Stop first: the advertising data cannot be rewritten while a set is live, and we are
    // called on every state change (including bond -> no bond, which flips the Flags byte).
    if (ble_gap_adv_active()) {
        int srv = ble_gap_adv_stop();
        if (srv != 0 && srv != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv_stop rc=%d", srv);
    }

    // Primary ADV_IND: flags(3) + name(10) + 3x uuid16(8) + appearance(4) = 25 bytes, the same
    // 25-byte payload the real remote sends.
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = bonded ? ADV_FLAGS_BONDED : ADV_FLAGS_PAIRING;
    adv.name = (uint8_t *)DEVICE_NAME;
    adv.name_len = strlen(DEVICE_NAME);
    adv.name_is_complete = 1;
    adv.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0x180A), BLE_UUID16_INIT(0x180F),
                                    BLE_UUID16_INIT(0x181A) };
    adv.num_uuids16 = 3;
    adv.uuids16_is_complete = 0;   // "Incomplete 16-bit Service UUIDs", as captured
    adv.appearance = APPEARANCE_KBD;
    adv.appearance_is_present = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        s_state = EMU_ERROR;
        return;
    }

    // SCAN_RSP: the Cypress manufacturer data and nothing else -> on air `05 ff 31 01 3b 04`.
    // The AC active-scans for this; it must NOT be in the primary PDU.
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.mfg_data = (uint8_t *)CYPRESS_MFG;
    rsp.mfg_data_len = sizeof(CYPRESS_MFG);
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);
        s_state = EMU_ERROR;
        return;
    }

    // Fast connectable-undirected advertising so the AC's short pairing scan catches us.
    // disc_mode stays GENERAL on purpose: it is only NimBLE's internal bookkeeping (it would
    // impose the 180 s limited-discovery timer), while the byte the AC actually gates on is
    // the Flags value we authored above.
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x0030;   // 30 ms
    params.itvl_max = 0x0060;   // 60 ms
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, emu_gap_event, NULL);
    if (rc == BLE_HS_ENOMEM) {
        // A connection slot is already taken — we raced a connect. Benign; the disconnect
        // handler will advertise again. Never latch EMU_ERROR for this.
        ESP_LOGI(TAG, "adv_start: a connection is already up, staying connected");
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        s_state = EMU_ERROR;
        return;
    }

    s_state = EMU_ADVERTISING;
    ESP_LOGI(TAG, "advertising as \"%s\", flags 0x%02x (%s)", DEVICE_NAME, adv.flags,
             bonded ? "bonded: general discoverable, awaiting reconnect"
                    : "no bond: LIMITED discoverable = pairing mode");
}

/* ---- GAP events ------------------------------------------------------------------------ */

// Ask the AC to secure the link: as a peripheral, ble_gap_security_initiate() emits an SMP
// Security Request. The AC waits for the remote to ask — without this we stay connected and
// unpaired forever. On an existing bond it triggers the normal encrypted reconnect instead.
//
// Called from both CONNECT and LINK_ESTAB. NimBLE grew LINK_ESTAB precisely because "even if
// BLE_GAP_EVENT_CONNECT is posted, the link synchronization procedure may fail and the link
// gets disconnected with reason 0x3E" — and 0x3E is this AC's known failure mode (see the
// protocol doc's *Connection behaviour*: establishment is a lottery). Current esp-nimble posts
// the two events back to back, so this is belt-and-braces; on a build where LINK_ESTAB really
// is deferred, asking on CONNECT alone would be too early. Asking twice is free: the second
// call returns BLE_HS_EALREADY, which is documented and harmless.
static uint16_t s_sec_asked_conn = BLE_HS_CONN_HANDLE_NONE;

// Adopt a connection handle if the host really has a connection for it.
//
// Do NOT infer "there is no connection" from a non-zero event status. Observed on this AC: the
// CONNECT and LINK_ESTAB events both arrive with status 26 (BLE_HS_EENCRYPT_KEY_SZ) on an
// encrypted reconnect, yet the connection is live and goes on to report encrypted=1 bonded=1 a
// second later. Believing the status meant we never recorded the handle, so presses could not
// be relayed and the AC eventually dropped the link on supervision timeout (reason 0x208).
// The host is the authority: ask it.
static bool adopt_conn(uint16_t handle, const char *where, int status)
{
    struct ble_gap_conn_desc desc;
    if (handle == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(handle, &desc) != 0) {
        ESP_LOGW(TAG, "%s: status=%d and no connection exists — back to advertising",
                 where, status);
        return false;
    }
    if (status != 0) {
        ESP_LOGW(TAG, "%s reported status=%d but conn=%d is live — continuing",
                 where, status, handle);
    }
    s_conn = handle;
    s_secure = desc.sec_state.encrypted;
    recompute_state();
    return true;
}

static void request_security(uint16_t conn)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || s_secure) return;
    if (s_sec_asked_conn == conn) return;          // already asked on this link

    int rc = ble_gap_security_initiate(conn);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_sec_asked_conn = conn;                    // don't ask again on this link
    } else {
        // Leave unmarked so the next event retries.
        ESP_LOGW(TAG, "security_initiate rc=%d — will retry on the next event", rc);
    }
}

static int emu_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_report_subscribed = false;
        s_sec_asked_conn = BLE_HS_CONN_HANDLE_NONE;
        if (!adopt_conn(event->connect.conn_handle, "connect", event->connect.status)) {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_secure = false;
            emu_advertise();
            return 0;
        }
        ESP_LOGI(TAG, "connected (conn=%d)", s_conn);
        request_security(s_conn);
        return 0;

#ifdef BLE_GAP_EVENT_LINK_ESTAB
    case BLE_GAP_EVENT_LINK_ESTAB:
        // The link really is up now. If CONNECT already got security going this is a no-op.
        if (!adopt_conn(event->link_estab.conn_handle, "link establishment",
                        event->link_estab.status)) {
            return 0;   // genuinely gone; the CONNECT path or a disconnect re-advertises
        }
        ESP_LOGI(TAG, "link established (conn=%d)", s_conn);
        request_security(s_conn);
        return 0;
#endif

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_secure = false;
        s_report_subscribed = false;
        s_sec_asked_conn = BLE_HS_CONN_HANDLE_NONE;
        emu_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            // Also adopt the handle here. If CONNECT arrived with a misleading status and we
            // dropped it, this is the second chance to notice we are in fact connected —
            // otherwise we would sit "bonded but not connected" and refuse every press.
            s_conn = event->enc_change.conn_handle;
            s_secure = desc.sec_state.encrypted;
            recompute_state();
            ESP_LOGI(TAG, "encryption change: status=%d encrypted=%d bonded=%d -> %s",
                     event->enc_change.status, desc.sec_state.encrypted,
                     desc.sec_state.bonded, STATE_STR[s_state]);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_hid_report_handle) {
            s_report_subscribed = event->subscribe.cur_notify;
            recompute_state();
        }
        ESP_LOGI(TAG, "subscribe: handle=%d notify=%d", event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The AC is re-pairing an address we already have keys for: drop the stale bond and
        // let it pair again, which is what a real remote does after its MODE-hold unpair.
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // Should never happen: the remote pairs Just Works (NoInputNoOutput, SC=0, MITM=0) and
        // the AC accepts that. Log loudly if the AC ever asks for more.
        ESP_LOGW(TAG, "unexpected passkey action %d — AC wants authenticated pairing?",
                 event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        emu_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU = %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ---- sending a button report ----------------------------------------------------------- */

// ble_gatts_notify_custom() pushes the notification without consulting the CCCD, which is
// required here: the AC bonds but never subscribes to our HID report.
static int emu_send_report(uint8_t b2, uint8_t b3)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return BLE_HS_ENOTCONN;

    uint8_t report[8] = { 0, 0, b2, b3, 0, 0, 0, 0 };
    int rc = 0;
    for (int i = 0; i < REPORT_REPEAT; i++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
        if (!om) { rc = BLE_HS_ENOMEM; break; }
        rc = ble_gatts_notify_custom(conn, s_hid_report_handle, om);
        if (rc != 0) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return rc;
}

/* ---- press gating: pacing, sync window, model updates ---------------------------------- */
/* Carried over from uart_link.c unchanged apart from the transport. */

bool ble_emu_valid_btn(const char *btn)
{
    for (size_t i = 0; i < NUM_BUTTONS; i++) if (!strcmp(btn, BUTTONS[i].name)) return true;
    return false;
}

// Sync window: while active, presses still update the model but are NOT sent to the AC, so the
// user can re-align the model to the AC's real display by pressing the physical remote.
static int64_t s_mute_until_us;

// Timestamp of the last registered press (relayed or model-only), guarding the AC's ~1.5 s
// capacitive-touch debounce. Read/stamped under a spinlock so the web task and the HA worker
// can't both pass the gap check on a stale value and double-press within the debounce.
static int64_t s_last_press_us;
static portMUX_TYPE s_press_mux = portMUX_INITIALIZER_UNLOCKED;

int64_t ble_emu_since_press_us(void)
{
    portENTER_CRITICAL(&s_press_mux);
    int64_t last = s_last_press_us;
    portEXIT_CRITICAL(&s_press_mux);
    return esp_timer_get_time() - last;
}

void ble_emu_mute(int seconds)
{
    if (seconds < 0) seconds = 0;
    s_mute_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGI(TAG, "sync window: muting sends for %ds (model-only presses)", seconds);
}

int ble_emu_mute_secs(void)
{
    int64_t r = s_mute_until_us - esp_timer_get_time();
    return r > 0 ? (int)(r / 1000000) + 1 : 0;
}

bool ble_emu_ready(void) { return s_state == EMU_READY; }

// A press is "meaningful" for the model when it will reach the AC OR we're in a sync window.
// Outside both, commanding can't take effect, so the model is left alone rather than drifting.
bool ble_emu_will_model(void)
{
    return esp_timer_get_time() < s_mute_until_us || s_state == EMU_READY;
}

bool ble_emu_press(const char *btn)
{
    if (!ble_emu_valid_btn(btn)) return false;

    if (esp_timer_get_time() < s_mute_until_us) {
        // Sync window: model-only re-alignment. Nothing is sent and there's no AC debounce to
        // honour (no press reaches the unit), so every press registers immediately.
        ESP_LOGI(TAG, "(sync, model-only) press %s", btn);
        ac_state_apply(btn);
        return true;
    }
    if (s_state != EMU_READY) {
        ESP_LOGI(TAG, "press %s [no relay — emulator not ready]", btn);
        return false;
    }
    // Atomically claim the press only if a full gap has elapsed since the last one. Both the
    // web handler and the HA worker funnel through here, so this stops a tap and a worker
    // press double-firing inside the AC's debounce. Read the clock inside the lock so a
    // preemption before we enter can't stamp a stale (early) time.
    portENTER_CRITICAL(&s_press_mux);
    int64_t now = esp_timer_get_time();
    bool too_soon = (now - s_last_press_us) < (int64_t)BLE_EMU_PRESS_GAP_MS * 1000;
    if (!too_soon) s_last_press_us = now;
    portEXIT_CRITICAL(&s_press_mux);
    if (too_soon) return false;   // within the AC's debounce -> coalesced, drop

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (strcmp(btn, BUTTONS[i].name)) continue;
        int rc = emu_send_report(BUTTONS[i].b2, BUTTONS[i].b3);
        if (rc != 0) {
            ESP_LOGW(TAG, "press %s: notify rc=%d", btn, rc);
            return false;
        }
        ESP_LOGI(TAG, "press %s -> notified", btn);
        ac_state_apply(btn);
        return true;
    }
    return false;
}

void ble_emu_env(float t, float h, float p)
{
    s_temp_x100  = (int16_t)(t * 100.0f);
    s_humid_x100 = (uint16_t)(h * 100.0f);
    s_press_x10  = (uint32_t)(p * 10000.0f);   // hPa -> 1/10 Pa, as the characteristic wants

    // Push to the AC if it subscribed to any of them (it normally does not, but it costs
    // nothing to be correct here — chr_updated respects the CCC, so it is a no-op if not).
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gatts_chr_updated(s_temp_handle);
        ble_gatts_chr_updated(s_humid_handle);
        ble_gatts_chr_updated(s_press_handle);
    }
}

void ble_emu_pairing(bool unpair)
{
    if (!unpair) {
        // "Pair": make sure we are advertising in pairing mode. If nothing is bonded this is
        // already the case; if something is, the caller wanted unpair.
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) emu_advertise();
        ESP_LOGI(TAG, "pair requested (advertising, %s)",
                 emu_has_bond() ? "bond present — use unpair to re-pair" : "pairing mode");
        return;
    }

    // "Unpair": drop the bond and the live link, then re-advertise Limited Discoverable so the
    // AC (or another remote) can pair fresh. Same effect as holding MODE on a real remote.
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "unpair: bonds cleared rc=%d", rc);
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        // The disconnect callback re-advertises, now with no bond -> pairing mode.
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        emu_advertise();
    }
}

emu_state_t ble_emu_state(void)  { return s_state; }
bool        ble_emu_alive(void)  { return s_state != EMU_OFFLINE; }
const char *ble_emu_status(void) { return STATE_STR[s_state]; }

/* ---- state reflection to the LED and MQTT ---------------------------------------------- */

static void reflect(emu_state_t st)
{
    static emu_state_t last = -1;
    if (st != last) {
        last = st;
        ESP_LOGI(TAG, "emulator: %s", STATE_STR[st]);
        mqtt_ha_publish_link(STATE_STR[st]);
    }
    // The LED shows the emulator only once Wi-Fi is connected; during provisioning the Wi-Fi
    // states own it (handled by wifi_mgr).
    if (strcmp(wifi_mgr_state_str(), "connected") != 0) return;
    switch (st) {
    case EMU_OFFLINE:     led_status_set(LED_EMU_DOWN);    break;
    case EMU_ERROR:       led_status_set(LED_ERROR);       break;
    case EMU_BOOT:
    case EMU_ADVERTISING: led_status_set(LED_EMU_PAIRING); break;
    case EMU_CONNECTED:   led_status_set(LED_EMU_LINK);    break;
    case EMU_READY:       led_status_set(LED_EMU_READY);   break;
    }
}

static void reflect_task(void *arg)
{
    int err_ticks = 0;
    for (;;) {
        reflect(s_state);

        // Self-heal. A transient advertising failure (or a host reset) would otherwise latch
        // EMU_ERROR forever: nothing else calls emu_advertise() unless we are already
        // advertising and get a disconnect or ADV_COMPLETE. This device lives unattended, so
        // retry every ~5 s instead of needing a power cycle.
        if (s_state == EMU_ERROR && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            if (++err_ticks >= 20) {
                err_ticks = 0;
                ESP_LOGW(TAG, "in error state — retrying advertising");
                emu_advertise();
            }
        } else {
            err_ticks = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ---- host bring-up -------------------------------------------------------------------- */

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
    s_state = EMU_ERROR;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);        // prefer the public address
    if (rc != 0) ESP_LOGW(TAG, "ensure_addr rc=%d", rc);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "id_infer_auto rc=%d", rc);
        s_state = EMU_ERROR;
        return;
    }

    uint8_t a[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    ESP_LOGI(TAG, "address %02x:%02x:%02x:%02x:%02x:%02x (type %d)",
             a[5], a[4], a[3], a[2], a[1], a[0], s_own_addr_type);

    check_handle_alignment();

    s_state = EMU_BOOT;
    emu_advertise();
}

static void host_task(void *param)
{
    nimble_port_run();               // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void ble_emu_init(void)
{
    // Give ourselves a Cypress-OUI (00:A0:50) public address like a real remote, with the low
    // three bytes derived from this chip's factory MAC so it is unique per unit and stable
    // across reboots (the AC's bond is keyed to it). The address is NOT the pairing gate — the
    // AC pairs whatever advertises Limited Discoverable — so a failure here is not fatal.
    uint8_t factory[6] = { 0 };
    if (esp_read_mac(factory, ESP_MAC_BT) == ESP_OK) {
        uint8_t cypress[6] = { 0x00, 0xA0, 0x50, factory[3], factory[4], factory[5] };
        esp_err_t err = esp_iface_mac_addr_set(cypress, ESP_MAC_BT);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "could not set Cypress-OUI address (%s) — using the factory address",
                     esp_err_to_name(err));
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        s_state = EMU_ERROR;
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Pair exactly like the real remote does. Ground truth is an Android HCI snoop with zero
    // SMP failures: the remote responds IO=NoInputNoOutput, AuthReq = Bonding only (SC=0,
    // MITM=0) -> LEGACY Just Works, distributing LTK + IRK + CSRK, key size 16. The AC accepts
    // that, so do not "upgrade" to Secure Connections here.
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;
    ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(APPEARANCE_KBD);

    env_pad_build();          // must precede count_cfg: it sizes the descriptor list
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));

    ble_store_config_init();          // bonds persist in NVS across reboots
    nimble_port_freertos_init(host_task);

    xTaskCreate(reflect_task, "emu_led", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Ganymede emulator starting (NimBLE, on-chip radio)");
}
