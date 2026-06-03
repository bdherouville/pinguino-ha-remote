/*
 * Ganymede emulator — nRF52840 (Adafruit Bluefruit nRF52 BSP)
 * =============================================================
 * BLE HID-keyboard PERIPHERAL that impersonates the De'Longhi "Ganymede" remote so the
 * air-conditioner (AC, the central) connects to it and receives the same 8-byte button
 * reports as the manual remote. Port of the proven NimBLE logic in
 * reference/esp-idf-nimble/main/emulator.c — every value is sourced in
 * ../../docs/ganymede_protocol.md (Status/Source).
 *
 * Confirmed protocol (no sniffer needed; all from captures/raw/delonghi_re/):
 *  - ADV_IND  : Flags 06 + Name "Ganymede" + UUID16 180A/180F/181A + Appearance 0x03C1
 *  - SCAN_RSP : Cypress manufacturer data  ff 31 01 3b 04  (company 0x0131, payload 3b 04)
 *  - SMP      : Just Works (IO=NoInputNoOutput, SC=0, MITM=0, bonding, dist LTK+IRK+CSRK).
 *               The AC requests MITM+SC but accepts the Just-Works downgrade (ac-btsnoop.log:
 *               ~8 completed bonds, 0 failures). No passkey path needed.
 *  - HID      : Report Map (61 B) = boot keyboard, ONE 8-byte input report, NO Report ID.
 *               Report char 0x2A4D notifies {0,0,b2,b3,0,0,0,0}, 2x per press, no key-release.
 *
 * Build (Arduino "Adafruit nRF52" BSP; board = your nRF52840 Pro Micro / "Nordic nRF52840"):
 *   arduino-cli compile -b adafruit:nrf52:pca10056 firmware/nrf52_emulator
 *   arduino-cli upload  -b adafruit:nrf52:pca10056 -p /dev/ttyACM0 firmware/nrf52_emulator
 *   (or build in Arduino IDE and drag-drop the UF2).
 *
 * Serial CLI (USB): press <btn> | status | adv | clear-bonds | env <tC> <h%> <hPa> | help
 *
 * VERIFY-ON-HARDWARE NOTES (Bluefruit API varies slightly by BSP version):
 *   [1] SMP IO-capabilities API (Bluefruit.Security.*) — see configureSecurity().
 *   [2] addManufacturerData() payload = company-id(LE) + data, i.e. {0x31,0x01,0x3b,0x04}.
 *   [3] addUuid() may mark the UUID list "complete" vs the remote's "incomplete" (0x02);
 *       harmless for connection. Run the nRF Sniffer to confirm the on-air match.
 */

#include <bluefruit.h>

// ----------------------------------------------------------------------------- constants
#define DEVICE_NAME       "Ganymede"
#define APPEARANCE_KBD    0x03C1            // BLE_APPEARANCE_HID_KEYBOARD
#define REPORT_REPEAT     2                 // real remote emits ~2 notifications per press

// UART link to the ESP32 bridge. nice!nano pads wired to ESP32-S3 (raw nRF pins via the
// Feather variant's pin map): P0.17=D29, P0.20=D28, P0.22=D30.
//   ESP GPIO4(TX) -> P0.17 (nRF RX, D29) ;  P0.20 (nRF TX, D28) -> ESP GPIO5(RX)
//   P0.22 (heartbeat, D30) -> ESP GPIO6
#define PIN_UART_RX    29
#define PIN_UART_TX    28
#define PIN_HEARTBEAT  30

// Captured HID Report Map (0x2A4B) — verbatim, 61 bytes.
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0xc0,
};
static const uint8_t HID_INFO[]         = {0x11, 0x01, 0x00, 0x02}; // bcdHID 0x0111, ctry 0, flags 0x02
static const uint8_t REPORT_REF_INPUT[] = {0x00, 0x01};            // report ID 0, type Input
static const uint8_t CYPRESS_MFG[]      = {0x31, 0x01, 0x3b, 0x04};// company 0x0131 (LE) + payload 3b 04

struct Button { const char* name; uint8_t b2; uint8_t b3; };
static const Button BUTTONS[] = {
    {"power", 0x01, 0x00}, {"down",  0x02, 0x00}, {"up",   0x04, 0x00},
    {"mode",  0x08, 0x00}, {"eco",   0x10, 0x00}, {"timer",0x20, 0x00},
    {"fan",   0x40, 0x00}, {"silent",0x80, 0x00}, {"flap", 0x00, 0x01},
};
static const size_t NUM_BUTTONS = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

// ----------------------------------------------------------------------------- services
// 0x1812 Human Interface Device
BLEService        svcHid(0x1812);
BLECharacteristic chrHidInfo(0x2A4A);   // R
BLECharacteristic chrReportMap(0x2A4B); // R
BLECharacteristic chrHidCtrl(0x2A4C);   // WNR
BLECharacteristic chrProtoMode(0x2A4E); // R, WNR
BLECharacteristic chrReport(0x2A4D);    // R, Notify (+ Report Reference 0x2908 + CCCD)
BLECharacteristic chrBootKbIn(0x2A22);  // R, Notify
BLECharacteristic chrBootKbOut(0x2A32); // R, W, WNR

// 0x180F Battery
BLEService        svcBat(0x180F);
BLECharacteristic chrBattery(0x2A19);   // R, Notify

// 0x181A Environmental Sensing
BLEService        svcEnv(0x181A);
BLECharacteristic chrTemp(0x2A6E);      // R, Notify  sint16 LE /100 C
BLECharacteristic chrHumid(0x2A6F);     // R, Notify  uint16 LE /100 %
BLECharacteristic chrPress(0x2A6D);     // R, Notify  uint32 LE /10 Pa

// ----------------------------------------------------------------------------- state
static uint8_t  g_protocol_mode = 0x01;        // 0x2A4E: Report mode
static uint8_t  g_battery       = 100;
static int16_t  g_temp_x100     = 2734;         // 27.34 C
static uint16_t g_humid_x100    = 4039;         // 40.39 %
static uint32_t g_press_x10     = 10132500;     // 1013.25 hPa in 0.1 Pa units
static uint16_t g_conn = BLE_CONN_HANDLE_INVALID;

// Emit a link-state token to the ESP32 bridge over UART (and mirror to USB for debug).
// Tokens match firmware/esp32_bridge uart_link.c: boot|advertising|connected|bonded|ready|error.
static void emitStatus(const char* tok) {
    Serial1.printf("status %s\n", tok);
    Serial.printf("{\"ev\":\"status\",\"state\":\"%s\"}\n", tok);
}

// ----------------------------------------------------------------------------- helpers
static void configureSecurity() {
    // Just Works, no MITM, bonding. Encryption (no MITM) is required to read the HID
    // characteristics (set per-characteristic below), which triggers Just-Works pairing.
    // [1] If your BSP exposes IO-cap control, force NoInputNoOutput Just Works explicitly:
    //   Bluefruit.Security.setIOCaps(false, false, false); // display, yes/no, keyboard
    //   Bluefruit.Security.setMITM(false);
    // Bonding/keys are persisted in flash by the BSP automatically.
}

// AC subscribing to a report CCCD is what makes notifications deliverable. We instrument
// BOTH report chars (0x2A4D Report and 0x2A22 Boot Keyboard) to learn which one the AC uses,
// plus the Protocol Mode write — to diagnose the "ok:0" (no-subscription) press case.
static void onReportCccd(uint16_t conn_handle, BLECharacteristic* chr, uint16_t value) {
    (void)conn_handle; (void)chr;
    bool en = value & 0x0001;
    Serial.printf("{\"ev\":\"cccd\",\"report_notify\":%d}\n", en);
    if (en) emitStatus("ready");
}
static void onBootKbCccd(uint16_t conn_handle, BLECharacteristic* chr, uint16_t value) {
    (void)conn_handle; (void)chr;
    bool en = value & 0x0001;
    Serial.printf("{\"ev\":\"cccd\",\"bootkb_notify\":%d}\n", en);
    if (en) emitStatus("ready");
}
static void onProtoWrite(uint16_t conn_handle, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)conn_handle; (void)chr;
    if (len >= 1) { g_protocol_mode = data[0];
        Serial.printf("{\"ev\":\"proto_mode\",\"mode\":%d}\n", data[0]); }   // 0=Boot, 1=Report
}

static void setupHid() {
    svcHid.begin(); // services must begin() before their characteristics

    chrHidInfo.setProperties(CHR_PROPS_READ);
    chrHidInfo.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrHidInfo.setFixedLen(sizeof(HID_INFO));
    chrHidInfo.begin();
    chrHidInfo.write(HID_INFO, sizeof(HID_INFO));

    chrReportMap.setProperties(CHR_PROPS_READ);
    chrReportMap.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrReportMap.setMaxLen(sizeof(HID_REPORT_MAP));
    chrReportMap.begin();
    chrReportMap.write(HID_REPORT_MAP, sizeof(HID_REPORT_MAP));

    chrHidCtrl.setProperties(CHR_PROPS_WRITE_WO_RESP);
    chrHidCtrl.setPermission(SECMODE_NO_ACCESS, SECMODE_ENC_NO_MITM);
    chrHidCtrl.setFixedLen(1);
    chrHidCtrl.begin();

    chrProtoMode.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE_WO_RESP);
    chrProtoMode.setPermission(SECMODE_ENC_NO_MITM, SECMODE_ENC_NO_MITM);
    chrProtoMode.setFixedLen(1);
    chrProtoMode.setWriteCallback(onProtoWrite);    // log Boot vs Report mode switches
    chrProtoMode.begin();
    chrProtoMode.write(&g_protocol_mode, 1);

    // Input report: R + Notify, with a Report Reference (0x2908) descriptor = {00,01}.
    chrReport.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    chrReport.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrReport.setFixedLen(8);
    chrReport.setCccdWriteCallback(onReportCccd);   // detect AC subscribing to notifications
    chrReport.begin();
    uint8_t zero8[8] = {0};
    chrReport.write(zero8, sizeof(zero8));
    // [2908] Report Reference descriptor
    chrReport.addDescriptor(BLEUuid((uint16_t)0x2908), REPORT_REF_INPUT, sizeof(REPORT_REF_INPUT),
                            SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);

    chrBootKbIn.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    chrBootKbIn.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrBootKbIn.setFixedLen(8);
    chrBootKbIn.setCccdWriteCallback(onBootKbCccd);   // detect AC subscribing in Boot mode
    chrBootKbIn.begin();
    chrBootKbIn.write(zero8, sizeof(zero8));

    chrBootKbOut.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
    chrBootKbOut.setPermission(SECMODE_ENC_NO_MITM, SECMODE_ENC_NO_MITM);
    chrBootKbOut.setFixedLen(1);
    chrBootKbOut.begin();
}

static void setupBattery() {
    svcBat.begin();
    chrBattery.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    chrBattery.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrBattery.setFixedLen(1);
    chrBattery.begin();
    chrBattery.write(&g_battery, 1);
}

static void setupEnv() {
    svcEnv.begin();
    chrTemp.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    chrTemp.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrTemp.setFixedLen(2); chrTemp.begin(); chrTemp.write((uint8_t*)&g_temp_x100, 2);

    chrHumid.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    chrHumid.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrHumid.setFixedLen(2); chrHumid.begin(); chrHumid.write((uint8_t*)&g_humid_x100, 2);

    chrPress.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    chrPress.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    chrPress.setFixedLen(4); chrPress.begin(); chrPress.write((uint8_t*)&g_press_x10, 4);
}

static void startAdv() {
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();

    // --- ADV_IND : byte-exact match to the real remote (docs/ganymede_protocol.md) ---
    //   02 01 06                       Flags
    //   09 09 "Ganymede"               Complete Local Name
    //   07 02 0a 18 0f 18 1a 18        INCOMPLETE 16-bit UUIDs 180A/180F/181A (type 0x02!)
    //   03 19 c1 03                    Appearance 0x03C1
    // Order and the Incomplete(0x02) UUID type both matter — the AC templates on this in
    // pairing mode (Bluefruit's addUuid would emit Complete 0x03 + reorder, so we hand-build).
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addName();
    static const uint8_t UUID16_INCOMPLETE[] = {0x0a,0x18, 0x0f,0x18, 0x1a,0x18};
    Bluefruit.Advertising.addData(0x02, UUID16_INCOMPLETE, sizeof(UUID16_INCOMPLETE));
    Bluefruit.Advertising.addAppearance(APPEARANCE_KBD);

    // --- SCAN_RSP : Cypress manufacturer data (the AC active-scans + likely filters on it) ---
    Bluefruit.ScanResponse.addManufacturerData(CYPRESS_MFG, sizeof(CYPRESS_MFG)); // [2]

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 64);   // 20–40 ms (units of 0.625 ms) — fast
    Bluefruit.Advertising.setFastTimeout(0);
    Bluefruit.Advertising.start(0);              // 0 = advertise forever
    emitStatus("advertising");
}

// ----------------------------------------------------------------------------- callbacks
static void onConnect(uint16_t conn_handle) {
    g_conn = conn_handle;
    Serial.printf("{\"ev\":\"connected\",\"conn\":%u}\n", conn_handle);
    emitStatus("connected");
    // (AC drives pairing itself when it's in pairing mode — peripheral-initiated Security
    // Request made this AC reject+drop, so we don't request here.)
}
static void onDisconnect(uint16_t conn_handle, uint8_t reason) {
    g_conn = BLE_CONN_HANDLE_INVALID;
    Serial.printf("{\"ev\":\"disconnected\",\"reason\":%u}\n", reason);
    emitStatus("advertising");   // restartOnDisconnect(true) re-advertises automatically
}
static void onSecured(uint16_t conn_handle) {
    BLEConnection* c = Bluefruit.Connection(conn_handle);
    Serial.printf("{\"ev\":\"secured\",\"secured\":%d,\"bonded\":%d}\n",
                  c->secured(), c->bonded());
    emitStatus("bonded");        // link encrypted; "ready" follows once the AC enables CCCD
}

// ----------------------------------------------------------------------------- press
static void sendReport(uint8_t b2, uint8_t b3) {
    if (g_conn == BLE_CONN_HANDLE_INVALID) { Serial.println("{\"ev\":\"press\",\"err\":\"not_connected\"}"); return; }
    uint8_t report[8] = {0, 0, b2, b3, 0, 0, 0, 0};
    bool ok = false;
    for (int i = 0; i < REPORT_REPEAT; i++) {
        // notify whichever report the AC subscribed to (Report 0x2A4D or Boot Keyboard 0x2A22)
        bool r1 = chrReport.notify(report, sizeof(report));
        bool r2 = chrBootKbIn.notify(report, sizeof(report));
        ok = ok || r1 || r2;
        delay(2);
    }
    Serial.printf("{\"ev\":\"press\",\"report\":\"0000%02x%02x000000\",\"ok\":%d}\n", b2, b3, ok);
}

// ----------------------------------------------------------------------------- CLI
static void handleLine(char* line) {
    char* cmd = strtok(line, " \t\r\n");
    if (!cmd) return;
    if (!strcmp(cmd, "press")) {
        char* a = strtok(NULL, " \t\r\n");
        if (!a) { Serial.println("{\"err\":\"usage: press <btn>\"}"); return; }
        for (size_t i = 0; i < NUM_BUTTONS; i++)
            if (!strcmp(a, BUTTONS[i].name)) { sendReport(BUTTONS[i].b2, BUTTONS[i].b3); return; }
        Serial.println("{\"err\":\"unknown btn\",\"valid\":\"power down up mode eco timer fan silent flap\"}");
    } else if (!strcmp(cmd, "status")) {
        Serial.printf("{\"ev\":\"status\",\"conn\":%d,\"proto\":%d,\"batt\":%d,"
                      "\"tempC\":%.2f,\"hum\":%.2f,\"hPa\":%.2f}\n",
                      (int16_t)g_conn, g_protocol_mode, g_battery,
                      g_temp_x100/100.0, g_humid_x100/100.0, g_press_x10/10000.0);
    } else if (!strcmp(cmd, "adv")) {
        startAdv();
    } else if (!strcmp(cmd, "clear-bonds")) {
        Bluefruit.Periph.clearBonds();
        Serial.println("{\"ev\":\"bonds_cleared\"}");
    } else if (!strcmp(cmd, "addr")) {
        ble_gap_addr_t a = Bluefruit.getAddr();
        Serial.printf("{\"ev\":\"addr\",\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"type\":%d}\n",
                      a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1], a.addr[0], a.addr_type);
    } else if (!strcmp(cmd, "reboot")) {
        Serial.println("{\"ev\":\"reboot\"}"); delay(50); NVIC_SystemReset();
    } else if (!strcmp(cmd, "env")) {
        char *t = strtok(NULL, " "), *h = strtok(NULL, " "), *p = strtok(NULL, " ");
        if (!t || !h || !p) { Serial.println("{\"err\":\"usage: env <tC> <h%> <hPa>\"}"); return; }
        g_temp_x100  = (int16_t)(atof(t) * 100);
        g_humid_x100 = (uint16_t)(atof(h) * 100);
        g_press_x10  = (uint32_t)(atof(p) * 10000);
        chrTemp.write((uint8_t*)&g_temp_x100, 2);
        chrHumid.write((uint8_t*)&g_humid_x100, 2);
        chrPress.write((uint8_t*)&g_press_x10, 4);
        handleLine((char*)"status");
    } else if (!strcmp(cmd, "help")) {
        Serial.println("{\"cmds\":\"press <btn> | status | adv | clear-bonds | env <tC> <h%> <hPa>\"}");
    } else {
        Serial.println("{\"err\":\"unknown cmd; try help\"}");
    }
}

static void pollStream(Stream& st, char* buf, size_t& n, size_t cap) {
    while (st.available()) {
        char c = st.read();
        if (c == '\n' || c == '\r') { if (n) { buf[n] = 0; handleLine(buf); n = 0; } }
        else if (n < cap - 1) buf[n++] = c;
    }
}
static void pollCli() {
    static char ub[96]; static size_t un = 0;   // USB CDC (debug)
    static char sb[96]; static size_t sn = 0;   // UART1 from the ESP32 bridge
    pollStream(Serial,  ub, un, sizeof(ub));
    pollStream(Serial1, sb, sn, sizeof(sb));
}

// ----------------------------------------------------------------------------- setup/loop
void setup() {
    Serial.begin(115200);

    // UART link to the ESP32 bridge + hardware heartbeat line.
    Serial1.setPins(PIN_UART_RX, PIN_UART_TX);
    Serial1.begin(115200);
    pinMode(PIN_HEARTBEAT, OUTPUT);
    emitStatus("boot");

    Bluefruit.begin();                 // 1 peripheral connection, 0 central

    // CHEAP-FIX TEST (2026-06-03): use a Cypress-OUI PUBLIC address (00:A0:50:xx) — the AC may
    // gate pairing on the Cypress OUI rather than the LL version company id. NOT the real
    // remote's address (avoids its stale AC bond). If setAddr(PUBLIC) is rejected by the
    // SoftDevice, the `addr` CLI cmd will show the fallback — then the LL-version gate stands.
    ble_gap_addr_t addr = {0};
    addr.addr_type = BLE_GAP_ADDR_TYPE_PUBLIC;
    uint8_t a[6] = {0xEF, 0xCD, 0xAB, 0x50, 0xA0, 0x00};   // LE -> 00:A0:50:AB:CD:EF (Cypress OUI)
    memcpy(addr.addr, a, 6);
    Bluefruit.setAddr(&addr);

    Bluefruit.setTxPower(4);
    Bluefruit.setName(DEVICE_NAME);
    Bluefruit.setAppearance(APPEARANCE_KBD);
    configureSecurity();

    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);
    Bluefruit.Security.setSecuredCallback(onSecured);

    setupHid();
    setupBattery();
    setupEnv();

    // Boot SILENT (no auto-advertise) so advertising can be started precisely when the AC is
    // in pairing mode — send "adv" to begin. (Auto-advertise will return once pairing is solved.)
    Serial.println("{\"ev\":\"boot\",\"role\":\"ganymede-emulator\",\"adv\":\"send 'adv' to start\","
                   "\"cmds\":\"adv|press|status|addr|reboot|clear-bonds|env|help\"}");
}

void loop() {
    pollCli();
    // ~1 Hz heartbeat on PIN_HEARTBEAT so the ESP32 bridge knows the nRF is alive.
    static uint32_t hb = 0; static bool lvl = false;
    if (millis() - hb >= 500) { hb = millis(); lvl = !lvl; digitalWrite(PIN_HEARTBEAT, lvl); }
    delay(5);
}
