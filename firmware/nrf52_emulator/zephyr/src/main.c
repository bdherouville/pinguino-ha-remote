/*
 * Ganymede remote EMULATOR — Zephyr port (nice!nano / nRF52840).
 *
 * Impersonates the De'Longhi "Ganymede" BLE HID remote: same name, keyboard
 * appearance, captured 61-byte HID Report Map, Battery + Environmental Sensing,
 * and the Cypress manufacturer data in the scan response. SMP responds
 * NoInputNoOutput / Just Works / bonding (SC=0, MITM=0), like the real remote.
 * The whole reason this is a Zephyr build (not Bluefruit) is the controller-
 * identity gate: CONFIG_BT_CTLR_COMPANY_ID=0x0131 (Cypress) so the AC's
 * LL_VERSION_IND check passes. See docs/ganymede_protocol.md.
 *
 * No serial console is available on this board during bring-up, so the blue LED
 * (P0.15) is a STATE DISPLAY:
 *   5 fast blinks at boot     = app started
 *   slow 1 Hz blink           = advertising (waiting for the AC)
 *   fast 4 Hz blink           = connected, not yet bonded/encrypted
 *   solid ON                  = bonded + encrypted  (<-- pairing gate PASSED)
 *
 * Source of truth: firmware/nrf52_emulator/reference/esp-idf-nimble/main/emulator.c
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>

/* The public address the emulator impersonates (your remote's address — the AC
 * bonds to it). Kept out of version control: copy clone_addr.h.example to
 * clone_addr.h and set your remote's bytes. When src/clone_addr.h is absent the
 * CMakeLists generates a placeholder copy from the template (a fresh clone still
 * builds, it just won't pair until you set your address). */
#include "clone_addr.h"
#include <zephyr/logging/log.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/controller.h>

LOG_MODULE_REGISTER(ganymede_emu, LOG_LEVEL_INF);

#define DEVICE_NAME      CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN  (sizeof(DEVICE_NAME) - 1)

/* ---- captured constants (verbatim from the reference) --------------------- */

static const uint8_t HID_REPORT_MAP[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
	0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
	0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
	0x81, 0x00, 0xc0,
};
static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x02 };
static const uint8_t REPORT_REF_INPUT[] = { 0x00, 0x01 };

struct button { const char *name; uint8_t b2, b3; };
static const struct button BUTTONS[] = {
	{ "power",  0x01, 0x00 }, { "down",   0x04, 0x00 }, { "up",     0x02, 0x00 },
	{ "mode",   0x08, 0x00 }, { "eco",    0x10, 0x00 }, { "timer",  0x20, 0x00 },
	{ "fan",    0x40, 0x00 }, { "silent", 0x80, 0x00 }, { "flap",   0x00, 0x01 },
};
#define NUM_BUTTONS  ARRAY_SIZE(BUTTONS)
#define REPORT_REPEAT 2

/* ---- mutable state -------------------------------------------------------- */

static uint8_t  g_protocol_mode = 0x01;
static uint8_t  g_battery_level = 100;
static int16_t  g_temp_x100  = 2350;
static uint16_t g_humid_x100 = 5000;
static uint32_t g_press_x10  = 1013000;

static struct bt_conn *g_conn;
static volatile bool g_secure;          /* encrypted + bonded */
static volatile bool g_report_subscribed;

/* The SuperMini's user-LED pin varies; drive ALL likely candidates together so
 * the status pattern is visible regardless of which pin the LED is wired to.
 * (Same safe set as the shotgun test that lit the blue LED.) */
static const struct device *g0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *g1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct { const struct device **dev; uint8_t pin; } led_pins[] = {
	{ &g0, 15 }, { &g0, 6 }, { &g0, 26 }, { &g0, 17 }, { &g0, 8 },
	{ &g1, 15 }, { &g1, 10 }, { &g1, 6 }, { &g0, 30 },
};
#define HB_PIN 24   /* P0.24 -> ESP32 GPIO6 heartbeat (toggled ~1 Hz) */

static void led_all_init(void)
{
	/* SuperMini: the user LED is powered through the VCC rail gated by P0.13;
	 * hold it HIGH so the LED can light. */
	gpio_pin_configure(g0, 13, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(g0, HB_PIN, GPIO_OUTPUT_INACTIVE);
	for (size_t i = 0; i < ARRAY_SIZE(led_pins); i++) {
		gpio_pin_configure(*led_pins[i].dev, led_pins[i].pin, GPIO_OUTPUT_INACTIVE);
	}
}

/* UART link to the ESP32 bridge: nRF -> ESP32 state tokens (matches uart_link.c). */
static void bridge_send(const char *s);

/* Current link state as an ESP-parsable token. "ready" = relay-capable, i.e. the
 * HID report CCC is subscribed (this AC drives HID even unencrypted, so don't gate
 * ready on g_secure — the LED would otherwise stay orange while control works). */
static const char *status_line(void)
{
	if (g_conn && g_report_subscribed) return "status ready\n";
	if (g_conn && g_secure)            return "status bonded\n";
	if (g_conn)                        return "status connected\n";
	return "status advertising\n";
}

/* Heartbeat: toggle P0.24 ~1 Hz so the ESP32 sees the emulator is alive.
 * Also RE-PUSH the current status every ~2 s — the per-event pushes can be
 * missed (boot ordering) or dropped on the status wire, leaving the ESP stuck
 * showing "boot"; this makes it converge to the true state within seconds. */
static void hb_thread(void *a, void *b, void *c)
{
	int lvl = 0, n = 0;
	for (;;) {
		lvl = !lvl;
		gpio_pin_set(g0, HB_PIN, lvl);
		if (++n >= 4) { n = 0; bridge_send(status_line()); }   /* ~2 s */
		k_msleep(500);
	}
}
K_THREAD_DEFINE(hb_tid, 512, hb_thread, NULL, NULL, NULL, 7, 0, 0);

/* ---- GATT read/write callbacks -------------------------------------------- */

static ssize_t rd_report_map(struct bt_conn *c, const struct bt_gatt_attr *a,
			     void *buf, uint16_t len, uint16_t off)
{ LOG_INF("AC read Report Map (h%u)", a->handle);
  return bt_gatt_attr_read(c, a, buf, len, off, HID_REPORT_MAP, sizeof(HID_REPORT_MAP)); }
static ssize_t rd_hid_info(struct bt_conn *c, const struct bt_gatt_attr *a,
			   void *buf, uint16_t len, uint16_t off)
{ return bt_gatt_attr_read(c, a, buf, len, off, HID_INFO, sizeof(HID_INFO)); }
static ssize_t rd_report_ref(struct bt_conn *c, const struct bt_gatt_attr *a,
			     void *buf, uint16_t len, uint16_t off)
{ LOG_INF("AC read Report Ref (h%u)", a->handle);
  return bt_gatt_attr_read(c, a, buf, len, off, REPORT_REF_INPUT, sizeof(REPORT_REF_INPUT)); }
static ssize_t rd_proto_mode(struct bt_conn *c, const struct bt_gatt_attr *a,
			     void *buf, uint16_t len, uint16_t off)
{ return bt_gatt_attr_read(c, a, buf, len, off, &g_protocol_mode, sizeof(g_protocol_mode)); }
static ssize_t rd_zero8(struct bt_conn *c, const struct bt_gatt_attr *a,
			void *buf, uint16_t len, uint16_t off)
{ uint8_t z[8] = { 0 }; LOG_INF("AC read input report (h%u)", a->handle);
  return bt_gatt_attr_read(c, a, buf, len, off, z, sizeof(z)); }
static ssize_t rd_batt(struct bt_conn *c, const struct bt_gatt_attr *a,
		       void *buf, uint16_t len, uint16_t off)
{ return bt_gatt_attr_read(c, a, buf, len, off, &g_battery_level, sizeof(g_battery_level)); }
static ssize_t rd_temp(struct bt_conn *c, const struct bt_gatt_attr *a,
		       void *buf, uint16_t len, uint16_t off)
{ return bt_gatt_attr_read(c, a, buf, len, off, &g_temp_x100, sizeof(g_temp_x100)); }
static ssize_t rd_humid(struct bt_conn *c, const struct bt_gatt_attr *a,
			void *buf, uint16_t len, uint16_t off)
{ return bt_gatt_attr_read(c, a, buf, len, off, &g_humid_x100, sizeof(g_humid_x100)); }
static ssize_t rd_press(struct bt_conn *c, const struct bt_gatt_attr *a,
			void *buf, uint16_t len, uint16_t off)
{ return bt_gatt_attr_read(c, a, buf, len, off, &g_press_x10, sizeof(g_press_x10)); }
static ssize_t wr_proto_mode(struct bt_conn *c, const struct bt_gatt_attr *a,
			     const void *buf, uint16_t len, uint16_t off, uint8_t flags)
{
	if (off + len > sizeof(g_protocol_mode)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	memcpy(&g_protocol_mode + off, buf, len);
	return len;
}
static ssize_t wr_sink(struct bt_conn *c, const struct bt_gatt_attr *a,
		       const void *buf, uint16_t len, uint16_t off, uint8_t flags)
{ return len; }

static void ccc_changed(const struct bt_gatt_attr *a, uint16_t value)
{ LOG_INF("CCC %u -> 0x%04x", a->handle, value); }

/* Handle padding: filler descriptors to shift later services to the real
 * remote's handle positions (so the HID report value lands at 0x003b). */
static ssize_t rd_pad(struct bt_conn *c, const struct bt_gatt_attr *a,
		      void *buf, uint16_t len, uint16_t off)
{ uint8_t z = 0; return bt_gatt_attr_read(c, a, buf, len, off, &z, 1); }
#define PAD_DESC(i, _) BT_GATT_DESCRIPTOR(BT_UUID_DECLARE_16(0x2901), \
		BT_GATT_PERM_READ, rd_pad, NULL, NULL)
/* number of filler handles inserted into Env to align HID -> 0x0037 */
#define ENV_PAD 22

static void report_ccc_changed(const struct bt_gatt_attr *a, uint16_t value)
{
	g_report_subscribed = (value & BT_GATT_CCC_NOTIFY) != 0;
	LOG_INF("HID report CCC -> 0x%04x", value);
	if (g_conn && g_report_subscribed) {
		bridge_send("status ready\n");   /* relay-capable -> green now */
	}
}

/* ---- GATT services -------------------------------------------------------- */

BT_GATT_SERVICE_DEFINE(bas_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
	BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, rd_batt, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

BT_GATT_SERVICE_DEFINE(ess_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_ESS),
	BT_GATT_CHARACTERISTIC(BT_UUID_TEMPERATURE,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, rd_temp, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_HUMIDITY,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, rd_humid, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_PRESSURE,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, rd_press, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	LISTIFY(ENV_PAD, PAD_DESC, (,)),   /* filler -> push HID to 0x0037 */
);

BT_GATT_SERVICE_DEFINE(hid_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),                              /* h+0  svc */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,        /* h+1,+2 */
			       BT_GATT_PERM_READ, rd_hid_info, NULL, NULL),
	/* Report value lands at h+4 -> with HID svc at 0x0037, value = 0x003b. */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, rd_zero8, NULL, NULL),     /* h+3,+4 (value=0x3b) */
	BT_GATT_CCC(report_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), /* h+5 */
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,      /* h+6 */
			   rd_report_ref, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,  /* h+7,+8 */
			       BT_GATT_PERM_READ, rd_report_map, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, wr_sink, NULL),    /* h+9,+10 */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,                  /* h+11,+12 */
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       rd_proto_mode, wr_proto_mode, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_IN_REPORT,              /* h+13,+14 */
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, rd_zero8, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),  /* h+15 */
);

static const struct bt_gatt_attr *g_report_attr;
static const struct bt_gatt_attr *g_report_ccc;   /* CCC right after the report value */
static void resolve_report_attr(void)
{
	for (uint16_t i = 0; i < hid_svc.attr_count; i++) {
		if (!bt_uuid_cmp(hid_svc.attrs[i].uuid, BT_UUID_HIDS_REPORT)) {
			g_report_attr = &hid_svc.attrs[i];
			if (i + 1 < hid_svc.attr_count &&
			    !bt_uuid_cmp(hid_svc.attrs[i + 1].uuid, BT_UUID_GATT_CCC)) {
				g_report_ccc = &hid_svc.attrs[i + 1];
			}
			return;
		}
	}
}

/* FORCE-NOTIFY HACK: the AC bonds but never enables our report CCC, so
 * bt_gatt_notify() rejects with -EINVAL. Force the CCC ON for this connection
 * once bonded, so we can push the button report regardless and see if the AC
 * acts on an unsolicited notification. */
static void force_subscribe(struct bt_conn *conn)
{
	if (!g_report_ccc) {
		return;
	}
	uint8_t v[2] = { 0x01, 0x00 };   /* CCC = Notify (LE) */
	bt_gatt_attr_write_ccc(conn, g_report_ccc, v, sizeof(v), 0, 0);
}

/* ---- advertising ---------------------------------------------------------- */

/* Flags byte is mutable: when we have NO bond we advertise LIMITED discoverable
 * (0x01) — that is the "I'm in pairing mode" signal the AC scans for and pairs
 * (confirmed on-air: the real remote pairs while advertising Flags 0x01, and the
 * AC then sends the SMP Pairing Request). When bonded we advertise GENERAL +
 * NO_BREDR (0x06) for normal encrypted reconnect. Set in emu_advertise(). */
#define ADV_FLAGS_PAIRING  BT_LE_AD_LIMITED                      /* 0x01 */
#define ADV_FLAGS_BONDED   (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR) /* 0x06 */
static uint8_t adv_flags = ADV_FLAGS_PAIRING;
static const struct bt_data ad[] = {
	BT_DATA(BT_DATA_FLAGS, &adv_flags, sizeof(adv_flags)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x0a, 0x18, 0x0f, 0x18, 0x1a, 0x18),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xc1, 0x03),
};
/* Cypress manufacturer data (company 0x0131 + key 3b 04) in the scan response,
 * exactly like the real remote (the AC SCAN_REQs to read it). */
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_MANUFACTURER_DATA, 0x31, 0x01, 0x3b, 0x04),
};

static void count_bond_cb(const struct bt_bond_info *info, void *user_data)
{
	(*(int *)user_data)++;
}
static bool emu_has_bond(void)
{
	int n = 0;
	bt_foreach_bond(BT_ID_DEFAULT, count_bond_cb, &n);
	return n > 0;
}
/* Plain connectable advertising. No USE_IDENTITY (it can make adv_start fail
 * when combined with a runtime-set public address); without privacy, a
 * connectable peripheral advertises its identity (the Cypress public addr) anyway. */
static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONNECTABLE,
	BT_GAP_ADV_FAST_INT_MIN_1, BT_GAP_ADV_FAST_INT_MAX_1, NULL);

static volatile bool g_adv_ok;

static void emu_advertise(void)
{
	/* No bond -> LIMITED discoverable (pairing mode); bonded -> GENERAL (reconnect). */
	adv_flags = emu_has_bond() ? ADV_FLAGS_BONDED : ADV_FLAGS_PAIRING;
	int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err && err != -EALREADY) {
		LOG_ERR("adv start failed (%d)", err);
		g_adv_ok = false;
		return;
	}
	g_adv_ok = true;
	bridge_send("status advertising\n");
	LOG_INF("advertising as \"%s\"", DEVICE_NAME);
}

/* ---- send a button report ------------------------------------------------- */

static int emu_send_report(uint8_t b2, uint8_t b3)
{
	if (!g_conn) {
		return -ENOTCONN;
	}
	uint8_t report[8] = { 0, 0, b2, b3, 0, 0, 0, 0 };
	int rc = 0;
	for (int i = 0; i < REPORT_REPEAT; i++) {
		rc = bt_gatt_notify(g_conn, g_report_attr, report, sizeof(report));
		if (rc) {
			break;
		}
		k_msleep(2);
	}
	return rc;
}

/* No autonomous press: the AC only acts on an explicit `press <btn>` command
 * from the ESP32 (web / MQTT / HTTP) — so plugging the boards in never turns
 * the AC on by itself. */

/* ---- connection callbacks ------------------------------------------------- */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err) {
		LOG_ERR("connect failed from %s (0x%02x)", addr, err);
		emu_advertise();
		return;
	}
	g_conn = bt_conn_ref(conn);
	bridge_send("status connected\n");
	LOG_INF("connected: %s", addr);

	/* Prompt the AC to encrypt/pair: as a peripheral this sends an SMP Security
	 * Request. Without it the AC connects but never starts pairing (it waits for
	 * the remote to ask) — so a fresh bond never happens. On an existing bond this
	 * just triggers the normal encrypted reconnect. */
	int sec = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (sec) {
		LOG_WRN("set_security rc=%d", sec);
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("disconnected (reason 0x%02x)", reason);
	g_secure = false;
	g_report_subscribed = false;
	if (g_conn) {
		bt_conn_unref(g_conn);
		g_conn = NULL;
	}
	emu_advertise();
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	g_secure = (err == BT_SECURITY_ERR_SUCCESS && level >= BT_SECURITY_L2);
	if (g_secure) {
		bridge_send("status bonded\n");
		force_subscribe(conn);   /* force-enable report CCC (AC won't) */
		if (g_report_subscribed) {
			/* force-notify makes us relay-capable the instant we bond ->
			 * tell the ESP we're READY so its LED goes green. */
			bridge_send("status ready\n");
		}
	}
	LOG_INF("security: %s level %d (err %d) -> secure=%d", addr, level, err, g_secure);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security_changed,
};

/* ---- LED state display ---------------------------------------------------- */

static void led_set(int on)
{
	for (size_t i = 0; i < ARRAY_SIZE(led_pins); i++) {
		gpio_pin_set(*led_pins[i].dev, led_pins[i].pin, on);
	}
}

static void led_thread(void *a, void *b, void *c)
{
	while (1) {
		if (g_conn && g_secure) {              /* BONDED: solid on */
			led_set(1); k_msleep(200);
		} else if (g_conn) {                   /* connected, not bonded: 4 Hz */
			led_set(1); k_msleep(125); led_set(0); k_msleep(125);
		} else if (g_adv_ok) {                 /* advertising: 1 Hz */
			led_set(1); k_msleep(500); led_set(0); k_msleep(500);
		} else {                               /* adv FAILED: 2 blips + pause */
			led_set(1); k_msleep(60); led_set(0); k_msleep(120);
			led_set(1); k_msleep(60); led_set(0); k_msleep(900);
		}
	}
}
K_THREAD_DEFINE(led_tid, 512, led_thread, NULL, NULL, NULL, 7, 0, 0);

/* ---- shell (works only if USB CDC ACM enumerates) ------------------------- */

static int cmd_press(const struct shell *sh, size_t argc, char **argv)
{
	for (size_t i = 0; i < NUM_BUTTONS; i++) {
		if (strcmp(argv[1], BUTTONS[i].name) == 0) {
			int rc = emu_send_report(BUTTONS[i].b2, BUTTONS[i].b3);
			shell_print(sh, "press %s -> rc=%d", argv[1], rc);
			return 0;
		}
	}
	shell_error(sh, "unknown button");
	return -EINVAL;
}
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "conn=%s secure=%d subscribed=%d",
		    g_conn ? "yes" : "no", g_secure, g_report_subscribed);
	return 0;
}
static int cmd_adv(const struct shell *sh, size_t argc, char **argv)
{ emu_advertise(); shell_print(sh, "advertising"); return 0; }
static int cmd_clear_bonds(const struct shell *sh, size_t argc, char **argv)
{ shell_print(sh, "clear-bonds rc=%d", bt_unpair(BT_ID_DEFAULT, NULL)); return 0; }

static void gatt_dump(void);
static int cmd_gatt(const struct shell *sh, size_t argc, char **argv)
{ gatt_dump(); shell_print(sh, "(see log)"); return 0; }

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "rebooting to UF2 bootloader...");
	nrf_power_gpregret_set(NRF_POWER, 0, 0x57);   /* Adafruit UF2 magic */
	k_msleep(60);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(emu_cmds,
	SHELL_CMD_ARG(press, NULL, "press <power|down|up|mode|eco|timer|fan|silent|flap>", cmd_press, 2, 0),
	SHELL_CMD(status, NULL, "show state", cmd_status),
	SHELL_CMD(gatt, NULL, "dump GATT handle map", cmd_gatt),
	SHELL_CMD(dfu, NULL, "reboot into UF2 bootloader", cmd_dfu),
	SHELL_CMD(adv, NULL, "(re)start advertising", cmd_adv),
	SHELL_CMD(clear_bonds, NULL, "erase bonds", cmd_clear_bonds),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(emu, &emu_cmds, "Ganymede emulator control", NULL);

/* ---- UART bridge to the ESP32 (nRF TX=P0.22, RX=P0.20, 115200) ------------
 * Line protocol: "press <btn>", "status". Replies one line. */
static const struct device *bridge_uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

/* Serialise TX: the heartbeat thread and the BT callbacks both send status
 * lines — without this their characters could interleave on the wire. */
K_MUTEX_DEFINE(bridge_tx_mutex);

static void bridge_send(const char *s)
{
	k_mutex_lock(&bridge_tx_mutex, K_FOREVER);
	while (*s) {
		uart_poll_out(bridge_uart, *s++);
	}
	k_mutex_unlock(&bridge_tx_mutex);
}

static void bridge_exec(char *line)
{
	char *cmd = strtok(line, " \t\r\n");
	if (!cmd) {
		return;
	}
	if (strcmp(cmd, "press") == 0) {
		char *btn = strtok(NULL, " \t\r\n");
		for (size_t i = 0; btn && i < NUM_BUTTONS; i++) {
			if (strcmp(btn, BUTTONS[i].name) == 0) {
				int rc = emu_send_report(BUTTONS[i].b2, BUTTONS[i].b3);
				char buf[48];
				snprintk(buf, sizeof(buf), "ok press %s rc=%d\n", btn, rc);
				bridge_send(buf);
				LOG_INF("bridge press %s -> notify rc=%d", btn, rc);
				return;
			}
		}
		bridge_send("err unknown_button\n");
	} else if (strcmp(cmd, "status") == 0) {
		char buf[64];
		snprintk(buf, sizeof(buf), "status conn=%d secure=%d sub=%d\n",
			 g_conn ? 1 : 0, g_secure, g_report_subscribed);
		bridge_send(buf);
	} else if (strcmp(cmd, "unpair") == 0) {
		/* Drop the bond and the live link, then re-advertise in pairing mode
		 * (LIMITED discoverable) so the AC can pair this or another remote. */
		int rc = bt_unpair(BT_ID_DEFAULT, NULL);
		if (g_conn) {
			bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		} else {
			bt_le_adv_stop();
			emu_advertise();
		}
		char buf[32];
		snprintk(buf, sizeof(buf), "ok unpair rc=%d\n", rc);
		bridge_send(buf);
		LOG_INF("bridge unpair rc=%d", rc);
	} else if (strcmp(cmd, "pair") == 0) {
		/* Ensure we're advertising for pairing (re-kick adv if idle). */
		if (!g_conn) {
			bt_le_adv_stop();
			emu_advertise();
		}
		bridge_send("ok pair\n");
		LOG_INF("bridge pair (advertising pairing mode)");
	} else {
		bridge_send("err\n");
	}
}

/* Interrupt-driven RX: the ISR drains the UART FIFO into a ring buffer so we
 * never lose bytes mid-line. The thread assembles lines from the ring buffer.
 * (Poll mode + k_msleep(5) used to black out for ~57 byte-times at 115200 and
 *  drop the tail of every "press power\n" — only "press p" ever arrived.) */
RING_BUF_DECLARE(bridge_rx_rb, 256);

static void bridge_uart_isr(const struct device *dev, void *ud)
{
	ARG_UNUSED(ud);
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t b[32];
		int n = uart_fifo_read(dev, b, sizeof(b));
		if (n > 0) {
			ring_buf_put(&bridge_rx_rb, b, n);   /* drop silently if full */
		}
	}
}

static void bridge_thread(void *a, void *b, void *c)
{
	if (!device_is_ready(bridge_uart)) {
		LOG_ERR("bridge UART NOT ready");
		return;
	}
	uart_irq_rx_disable(bridge_uart);
	uart_irq_tx_disable(bridge_uart);
	uart_irq_callback_user_data_set(bridge_uart, bridge_uart_isr, NULL);
	uart_irq_rx_enable(bridge_uart);
	LOG_INF("bridge UART ready (RX P0.20, TX P0.22, IRQ-driven) — listening");

	char line[64];
	int len = 0;
	uint8_t ch;
	for (;;) {
		while (ring_buf_get(&bridge_rx_rb, &ch, 1) == 1) {
			if (ch == '\n' || ch == '\r') {
				if (len > 0) {
					line[len] = '\0';
					LOG_INF("UART rx line: '%s'", line);
					bridge_exec(line);
					len = 0;
				}
			} else if (len < (int)sizeof(line) - 1) {
				line[len++] = ch;
			}
		}
		k_msleep(5);   /* ring buffer absorbs the burst while we sleep */
	}
}
K_THREAD_DEFINE(bridge_tid, 1024, bridge_thread, NULL, NULL, NULL, 6, 0, 0);

/* ---- GATT handle-map dump (to match the real remote's layout) ------------- */
static uint8_t dump_cb(const struct bt_gatt_attr *attr, uint16_t handle, void *ud)
{
	char u[BT_UUID_STR_LEN];
	bt_uuid_to_str(attr->uuid, u, sizeof(u));
	LOG_INF("GATT h0x%04x  %s", handle, u);
	k_msleep(15);   /* throttle so the USB console doesn't drop lines */
	return BT_GATT_ITER_CONTINUE;
}
static void gatt_dump(void)
{
	LOG_INF("--- GATT handle map (target: HID report value @ 0x003b) ---");
	bt_gatt_foreach_attr(0x0001, 0xffff, dump_cb, NULL);
}

/* ---- bring-up ------------------------------------------------------------- */

int main(void)
{
	int err;

	led_all_init();
	for (int i = 0; i < 5; i++) {                  /* "I booted" */
		led_set(1); k_msleep(80); led_set(0); k_msleep(120);
	}
	bridge_send("status boot\n");                  /* tell the ESP32 we're alive */

	if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
		(void)usb_enable(NULL);
	}

	resolve_report_attr();

	/* Public Cypress-OUI address (00:A0:50). Little-endian: addr[0]=LSB. Before
	 * bt_enable(). A specific clone_addr.h overrides; otherwise (generic default)
	 * derive a stable, unique per-device suffix from the chip's factory device
	 * address — so each unit gets its own 00:A0:50:xx:xx:xx, like a real remote,
	 * and it stays constant across reboots so the AC bond persists. */
	static const uint8_t compiled_addr[6] = CLONE_ADDR_LE;
	static const uint8_t generic_addr[6] = { 0x01, 0x00, 0x00, 0x50, 0xa0, 0x00 };
	uint8_t cypress_addr[6];
	if (memcmp(compiled_addr, generic_addr, sizeof(generic_addr)) != 0) {
		memcpy(cypress_addr, compiled_addr, sizeof(cypress_addr));  /* pinned address */
	} else {
		uint32_t id = NRF_FICR->DEVICEADDR[0];      /* factory random device addr */
		cypress_addr[0] = id & 0xff;
		cypress_addr[1] = (id >> 8) & 0xff;
		cypress_addr[2] = (id >> 16) & 0xff;
		cypress_addr[3] = 0x50; cypress_addr[4] = 0xa0; cypress_addr[5] = 0x00;
	}
	bt_ctlr_set_public_addr(cypress_addr);
	LOG_INF("public address 00:A0:50:%02x:%02x:%02x",
		cypress_addr[2], cypress_addr[1], cypress_addr[0]);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		while (1) { led_set(1); k_msleep(60); led_set(0); k_msleep(60); }
	}
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load();
	}

	gatt_dump();
	LOG_INF("Ganymede emulator ready (sdc controller, Cypress public addr)");
	emu_advertise();
	return 0;
}
