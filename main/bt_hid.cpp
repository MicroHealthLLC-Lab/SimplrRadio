/*
 * Bluetooth HID host implementation (bt_hid.hpp).
 *
 * Connection setup (NimBLE host task, one GATT procedure at a time, each started from the
 * previous one's callback): encrypt (pair or restore the bond) -> find the HID service ->
 * its characteristics -> each input report's descriptors (CCCD, Report Reference) -> the
 * report IDs -> the report map (hid_report.c) -> subscribe to the input reports the map has
 * keys in (a boot-protocol keyboard when there are none). Notifications then become keys
 * (hid_keymap.c), queued for the app task.
 *
 * Bonds: NimBLE's RAM store (CONFIG_BT_NIMBLE_NVS_PERSIST=n) is seeded from the vault at start
 * and every key it writes or deletes is mirrored to the vault (bt_store.cpp), so the long-term
 * keys are only ever in flash encrypted.
 */

#include "bt_hid.hpp"
#include "board.h"
#include "hid_keymap.h"
#include "hid_report.h"
#include "secret_vault.hpp"
#include "text.h"
#include "wifi_mgr.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mbedtls/platform_util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store/config/ble_store_config.h"

#include <algorithm>
#include <cstring>
#include <iterator>

extern "C" void ble_store_config_init(void);

static const char *TAG = "bt";

static const uint16_t UUID_REPORT_MAP = 0x2A4B;
static const uint16_t UUID_REPORT = 0x2A4D;
static const uint16_t UUID_BOOT_KEYBOARD_IN = 0x2A22;
static const uint16_t UUID_PROTOCOL_MODE = 0x2A4E;
static const uint16_t UUID_CCCD = 0x2902;
static const uint16_t UUID_REPORT_REF = 0x2908;
static const ble_uuid16_t UUID_HID_SERVICE = {{BLE_UUID_TYPE_16}, 0x1812};

static const int SCAN_MS = 8000;
static const int SYNC_TIMEOUT_MS = 8000;
static const int PAIR_CONNECT_MS = 30000;
static const size_t MAX_SEEN = 24;       // Devices remembered during one scan (HID or not)
static const size_t MAX_REPORTS = 12;    // Input reports of one device
static const size_t KEY_QUEUE = 32;
static const uint8_t REPORT_INPUT = 1;   // Report Reference: report type

// Bond record in the vault: version | sizeof(value) | flags (1 ours, 2 the peer's) | ours | peer's
static const uint8_t BOND_VERSION = 1;
static const size_t SEC_SIZE = sizeof(struct ble_store_value_sec);
static_assert(3 + 2 * sizeof(struct ble_store_value_sec) <= 256, "a bond must fit one vault secret");

using bt_hid::State;

// ---------------------------------------------------------------------------
// State shared with the app task
// ---------------------------------------------------------------------------

struct Seen {
    bt_hid::Found found;
    bool hid = false;
};

static SemaphoreHandle_t s_lock;       // Guards the strings and lists below
static SemaphoreHandle_t s_synced;
static SemaphoreHandle_t s_scan_done;
static QueueHandle_t s_keys;
static volatile bool s_started;
static volatile State s_state = State::Off;
static volatile uint32_t s_version;
static volatile bool s_waiting;        // A whitelist connect to the paired devices is pending
static volatile bool s_bg_connect;     // The pending connect is that one (not the user's)
static volatile bool s_hold;           // No background connect: a scan or a pairing runs
static volatile bool s_scanning;
static volatile bool s_user_pairing;   // The connection was started by pair()
static volatile bool s_pending_pair;   // pair() waits for the current link to close
static volatile bool s_has_passkey;
static volatile uint32_t s_passkey;
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static std::vector<Seen> s_seen;
static std::string s_device;
static std::string s_message;
static bt_store::Device s_pairing;     // The device pair() connects to

struct BtLock {
    BtLock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~BtLock() { xSemaphoreGive(s_lock); }
};

static void changed()
{
    s_version = s_version + 1;
}

static void set_state(State state)
{
    if (s_state != state) {
        s_state = state;
        changed();
    }
}

static void message(const std::string& text)
{
    {
        BtLock lock;
        s_message += text + "\n";
        if (s_message.size() > 512) {
            s_message.erase(0, s_message.size() - 512);
        }
    }
    changed();
}

// BLE addresses are 6 bytes everywhere; the array references make the compiler check both sizes
static void copy_addr(uint8_t (&dst)[6], const uint8_t (&src)[6])
{
    std::copy(std::begin(src), std::end(src), std::begin(dst));
}

static std::string shown_name(const std::string& name)
{
    return name.empty() ? std::string("the device") : name;
}

// ---------------------------------------------------------------------------
// Bonds: NimBLE's RAM store, mirrored to the vault
// ---------------------------------------------------------------------------

static bool is_sec(int type)
{
    return type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC;
}

static bool read_sec(int type, const ble_addr_t& addr, struct ble_store_value_sec& out)
{
    union ble_store_key key;
    memset(&key, 0, sizeof(key));
    key.sec.peer_addr = addr;
    union ble_store_value value;
    if (ble_store_config_read(type, &key, &value) != 0) {
        return false;
    }
    out = value.sec;
    mbedtls_platform_zeroize(&value, sizeof(value));
    return true;
}

static void persist(const ble_addr_t& addr)
{
    struct ble_store_value_sec ours, theirs;
    memset(&ours, 0, sizeof(ours));
    memset(&theirs, 0, sizeof(theirs));
    bool has_ours = read_sec(BLE_STORE_OBJ_TYPE_OUR_SEC, addr, ours);
    bool has_theirs = read_sec(BLE_STORE_OBJ_TYPE_PEER_SEC, addr, theirs);
    if (!has_ours && !has_theirs) {
        bt_store::remove_keys(addr.type, addr.val);
        return;
    }
    std::string record;
    record.reserve(3 + 2 * SEC_SIZE);
    record += (char)BOND_VERSION;
    record += (char)SEC_SIZE;
    record += (char)((has_ours ? 1 : 0) | (has_theirs ? 2 : 0));
    record.append(reinterpret_cast<const char*>(&ours), SEC_SIZE);
    record.append(reinterpret_cast<const char*>(&theirs), SEC_SIZE);
    if (bt_store::put_keys(addr.type, addr.val, record) != ESP_OK) {
        ESP_LOGW(TAG, "Bond not saved (vault unavailable)");
    }
    vault::wipe(record);
    mbedtls_platform_zeroize(&ours, sizeof(ours));
    mbedtls_platform_zeroize(&theirs, sizeof(theirs));
}

static int store_write(int type, const union ble_store_value *value)
{
    int rc = ble_store_config_write(type, value);
    if (rc == 0 && is_sec(type)) {
        persist(value->sec.peer_addr);
    }
    return rc;
}

static int store_delete(int type, const union ble_store_key *key)
{
    int rc = ble_store_config_delete(type, key);
    if (rc == 0 && is_sec(type)) {
        const uint8_t *v = key->sec.peer_addr.val;
        if (key->sec.peer_addr.type || std::any_of(v, v + 6, [](uint8_t b) { return b != 0; })) {
            persist(key->sec.peer_addr);
        }
    }
    return rc;
}

// Before the host runs: the RAM store gets the bonds saved in the vault
static void load_bonds()
{
    for (const auto& d : bt_store::devices()) {
        std::string record;
        if (!bt_store::get_keys(d.addr_type, d.addr, record)) {
            continue;
        }
        if (record.size() == 3 + 2 * SEC_SIZE && (uint8_t)record[0] == BOND_VERSION &&
            (uint8_t)record[1] == SEC_SIZE) {
            union ble_store_value value;
            if (record[2] & 1) {
                record.copy(reinterpret_cast<char*>(&value.sec), SEC_SIZE, 3);
                ble_store_config_write(BLE_STORE_OBJ_TYPE_OUR_SEC, &value);
            }
            if (record[2] & 2) {
                record.copy(reinterpret_cast<char*>(&value.sec), SEC_SIZE, 3 + SEC_SIZE);
                ble_store_config_write(BLE_STORE_OBJ_TYPE_PEER_SEC, &value);
            }
            mbedtls_platform_zeroize(&value, sizeof(value));
        } else {
            ESP_LOGW(TAG, "Bond record from another firmware dropped; pair the device again");
            bt_store::remove_keys(d.addr_type, d.addr);
        }
        vault::wipe(record);
    }
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

struct Report {
    uint16_t val = 0;
    uint16_t end = 0;      // Last handle of the characteristic
    uint16_t cccd = 0;
    uint16_t ref = 0;
    uint8_t id = 0;
    uint8_t type = 0;
    bool boot = false;     // Boot keyboard input report
    bool used = false;     // Subscribed
};

static Report s_reports[MAX_REPORTS];
static hid_input_t s_held[MAX_REPORTS];
static size_t s_n_reports;
static size_t s_step;
static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_map_handle;
static uint16_t s_protocol_handle;
static uint8_t *s_map_bytes;           // HID_MAX_MAP_BYTES, PSRAM
static size_t s_map_len;
static hid_map_t *s_map;               // PSRAM

static int gap_event(struct ble_gap_event *event, void *arg);

static struct ble_gap_conn_params conn_params(bool background)
{
    struct ble_gap_conn_params p;
    memset(&p, 0, sizeof(p));
    // In the background the radio listens 30 ms a second for a paired device, so WiFi keeps
    // the air almost to itself; for the device the user picked, 30 ms in 60
    p.scan_itvl = background ? 0x0640 : 0x0060;
    p.scan_window = 0x0030;
    p.itvl_min = 0x000C;              // 15-30 ms: keys feel immediate, WiFi still streams
    p.itvl_max = 0x0018;
    p.latency = 0;
    p.supervision_timeout = 0x0190;   // 4 s
    return p;
}

// Connects to whichever paired device shows up first (the controller's white list)
static void background_connect()
{
    if (!s_started || s_hold || s_scanning || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_active()) {
        return;
    }
    std::vector<bt_store::Device> devices = bt_store::devices();
    if (devices.empty()) {
        s_waiting = false;
        changed();
        return;
    }
    ble_addr_t list[bt_store::MAX_DEVICES];
    uint8_t n = 0;
    for (const auto& d : devices) {
        list[n].type = d.addr_type;
        copy_addr(list[n].val, d.addr);
        n++;
    }
    struct ble_gap_conn_params p = conn_params(true);
    bool ok = ble_gap_wl_set(list, n) == 0 &&
              ble_gap_connect(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL) == 0;
    if (!ok) {
        ESP_LOGW(TAG, "Waiting for paired devices failed");
    }
    s_waiting = ok;
    s_bg_connect = ok;
    changed();
}

// A scan or a pairing needs the controller: the background connect stops until it ends
static void hold_background()
{
    s_hold = true;
    if (ble_gap_conn_active() && s_conn == BLE_HS_CONN_HANDLE_NONE && !s_user_pairing) {
        ble_gap_conn_cancel();
    }
    s_waiting = false;
}

static void release_background()
{
    s_hold = false;
    background_connect();
}

// Closes the link; `why` is shown (and replaces the "didn't finish" note of a pairing)
static void end_link(const char* why)
{
    if (why) {
        message(why);
        s_user_pairing = false;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void setup_failed(const char* why)
{
    ESP_LOGW(TAG, "Device setup failed");
    std::string name;
    {
        BtLock lock;
        name = s_device;
    }
    end_link(("Couldn't use " + shown_name(name) + ": " + why).c_str());
}

static uint16_t uuid16(const ble_uuid_any_t& uuid)
{
    return uuid.u.type == BLE_UUID_TYPE_16 ? ble_uuid_u16(&uuid.u) : 0;
}

static void setup_done()
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) != 0) {
        return;
    }
    std::string name;
    {
        BtLock lock;
        name = s_device;
    }
    if (s_user_pairing) {
        bt_store::Device d;
        d.addr_type = desc.peer_id_addr.type;
        copy_addr(d.addr, desc.peer_id_addr.val);
        d.name = name;
        bt_store::remember(d);
        message("Paired with " + shown_name(name) + ". Its keys now work the radio.");
    }
    s_user_pairing = false;
    s_has_passkey = false;
    s_hold = false;
    set_state(State::Connected);
}

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg);

static void next_subscription()
{
    while (s_step < s_n_reports) {
        if (s_reports[s_step].used) {
            static const uint8_t NOTIFY_ON[2] = {1, 0};
            if (ble_gattc_write_flat(s_conn, s_reports[s_step].cccd, NOTIFY_ON, sizeof(NOTIFY_ON), on_subscribed,
                                     NULL) != 0) {
                setup_failed("its keys can't be read.");
            }
            return;
        }
        s_step++;
    }
    setup_done();
}

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        s_reports[s_step].used = false;
        ESP_LOGW(TAG, "Report subscription refused (%d)", error->status);
    }
    s_step++;
    next_subscription();
    return 0;
}

static void map_read()
{
    bool parsed = hid_parse_map(s_map_bytes, s_map_len, s_map);
    bool any = false;
    for (size_t i = 0; i < s_n_reports; i++) {
        Report& r = s_reports[i];
        r.used = parsed && !r.boot && r.type == REPORT_INPUT && r.cccd && hid_map_uses(s_map, r.id);
        any = any || r.used;
    }
    bool boot = false;
    if (!any) {
        // No report the map explains: a keyboard's boot protocol (fixed 8-byte reports)
        for (size_t i = 0; i < s_n_reports; i++) {
            Report& r = s_reports[i];
            r.used = r.boot && r.cccd;
            boot = boot || r.used;
        }
    }
    if (!any && !boot) {
        setup_failed("it sends no keys the radio understands.");
        return;
    }
    if (boot && s_protocol_handle) {
        static const uint8_t BOOT_PROTOCOL = 0;
        ble_gattc_write_no_rsp_flat(s_conn, s_protocol_handle, &BOOT_PROTOCOL, 1);
    }
    memset(s_held, 0, sizeof(s_held));
    s_step = 0;
    next_subscription();
}

static int on_map(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)arg;
    if (error->status == 0 && attr) {
        size_t n = OS_MBUF_PKTLEN(attr->om);
        n = std::min(n, (size_t)HID_MAX_MAP_BYTES - s_map_len);
        if (n && os_mbuf_copydata(attr->om, 0, (int)n, s_map_bytes + s_map_len) == 0) {
            s_map_len += n;
        }
        return 0;
    }
    map_read();   // Done (or the read failed: what arrived is parsed, the boot keyboard is next)
    return 0;
}

static int on_reference(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg);

static void next_reference()
{
    while (s_step < s_n_reports) {
        if (s_reports[s_step].ref) {
            if (ble_gattc_read(s_conn, s_reports[s_step].ref, on_reference, NULL) != 0) {
                setup_failed("its reports can't be read.");
            }
            return;
        }
        s_step++;
    }
    s_map_len = 0;
    if (!s_map_handle || ble_gattc_read_long(s_conn, s_map_handle, 0, on_map, NULL) != 0) {
        map_read();
    }
}

static int on_reference(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)arg;
    if (error->status == 0 && attr && OS_MBUF_PKTLEN(attr->om) >= 2) {
        uint8_t ref[2];
        if (os_mbuf_copydata(attr->om, 0, 2, ref) == 0) {
            s_reports[s_step].id = ref[0];
            s_reports[s_step].type = ref[1];
        }
    }
    s_step++;
    next_reference();
    return 0;
}

static int on_descriptor(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg);

static void next_descriptors()
{
    while (s_step < s_n_reports) {
        const Report& r = s_reports[s_step];
        if (r.end > r.val) {
            if (ble_gattc_disc_all_dscs(s_conn, r.val, r.end, on_descriptor, NULL) != 0) {
                setup_failed("its reports can't be found.");
            }
            return;
        }
        s_step++;
    }
    s_step = 0;
    next_reference();
}

static int on_descriptor(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn;
    (void)chr_val_handle;
    (void)arg;
    if (error->status == 0 && dsc) {
        uint16_t uuid = uuid16(dsc->uuid);
        if (uuid == UUID_CCCD) {
            s_reports[s_step].cccd = dsc->handle;
        } else if (uuid == UUID_REPORT_REF) {
            s_reports[s_step].ref = dsc->handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        s_step++;
        next_descriptors();
    } else {
        setup_failed("its reports can't be found.");
    }
    return 0;
}

static int on_characteristic(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                             void *arg)
{
    (void)arg;
    if (error->status == 0 && chr) {
        // Each characteristic ends where the next one's declaration starts
        if (s_n_reports && !s_reports[s_n_reports - 1].end) {
            s_reports[s_n_reports - 1].end = chr->def_handle - 1;
        }
        uint16_t uuid = uuid16(chr->uuid);
        if (uuid == UUID_REPORT_MAP) {
            s_map_handle = chr->val_handle;
        } else if (uuid == UUID_PROTOCOL_MODE) {
            s_protocol_handle = chr->val_handle;
        } else if ((uuid == UUID_REPORT || uuid == UUID_BOOT_KEYBOARD_IN) &&
                   (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) && s_n_reports < MAX_REPORTS) {
            Report r;
            r.val = chr->val_handle;
            r.boot = uuid == UUID_BOOT_KEYBOARD_IN;
            s_reports[s_n_reports++] = r;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_n_reports && !s_reports[s_n_reports - 1].end) {
            s_reports[s_n_reports - 1].end = s_svc_end;
        }
        if (!s_n_reports) {
            setup_failed("it has no key reports.");
        } else {
            s_step = 0;
            next_descriptors();
        }
        (void)conn;
        return 0;
    }
    setup_failed("its reports can't be found.");
    return 0;
}

static int on_service(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *service,
                      void *arg)
{
    (void)arg;
    if (error->status == 0 && service) {
        if (!s_svc_start) {
            s_svc_start = service->start_handle;
            s_svc_end = service->end_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE && s_svc_start) {
        if (ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_characteristic, NULL) != 0) {
            setup_failed("its reports can't be found.");
        }
    } else {
        setup_failed("it isn't a keyboard or remote.");
    }
    return 0;
}

static void start_setup()
{
    for (auto& r : s_reports) {
        r = Report();
    }
    s_n_reports = 0;
    s_step = 0;
    s_svc_start = s_svc_end = 0;
    s_map_handle = s_protocol_handle = 0;
    s_map_len = 0;
    set_state(State::Setup);
    if (ble_gattc_disc_svc_by_uuid(s_conn, &UUID_HID_SERVICE.u, on_service, NULL) != 0) {
        setup_failed("it doesn't answer.");
    }
}

static void on_notify(uint16_t handle, struct os_mbuf *om)
{
    if (s_state != State::Connected) {
        return;
    }
    for (size_t i = 0; i < s_n_reports; i++) {
        const Report& r = s_reports[i];
        if (!r.used || r.val != handle) {
            continue;
        }
        uint8_t data[HID_MAX_REPORT];
        size_t len = std::min((size_t)OS_MBUF_PKTLEN(om), sizeof(data));
        if (os_mbuf_copydata(om, 0, (int)len, data) != 0) {
            return;
        }
        hid_input_t now;
        if (r.boot) {
            hid_decode_boot_keyboard(data, len, &now);
        } else {
            hid_decode(s_map, r.id, data, len, &now);
        }
        uint32_t keys[8];
        size_t n = hid_new_keys(&s_held[i], &now, keys, 8);
        s_held[i] = now;
        for (size_t k = 0; k < n; k++) {
            xQueueSend(s_keys, &keys[k], 0);   // A full queue drops keys rather than stall the host
        }
        mbedtls_platform_zeroize(data, sizeof(data));   // It may have been a password
        return;
    }
}

static void on_advertisement(const struct ble_gap_disc_desc& d)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d.data, d.length_data) != 0) {
        return;
    }
    bool hid = false;
    for (int i = 0; i < fields.num_uuids16; i++) {
        hid = hid || ble_uuid_u16(&fields.uuids16[i].u) == 0x1812;
    }
    if (fields.appearance_is_present) {
        // HID devices (keyboards, mice, gamepads...) 0x03C0-0x03CF; remote controls 0x0180-0x01BF
        hid = hid || (fields.appearance >= 0x03C0 && fields.appearance <= 0x03CF) ||
              (fields.appearance >= 0x0180 && fields.appearance <= 0x01BF);
    }
    char name[bt_store::MAX_NAME + 1] = "";
    if (fields.name && fields.name_len) {
        // At most 31 bytes (an advertisement carries fewer); the screen keeps MAX_NAME
        std::string raw(reinterpret_cast<const char*>(fields.name), std::min((size_t)fields.name_len, (size_t)31));
        text_to_ascii(name, sizeof(name), raw.c_str());
    }
    BtLock lock;
    for (auto& s : s_seen) {
        if (s.found.addr_type == d.addr.type && memcmp(s.found.addr, d.addr.val, 6) == 0) {
            s.hid = s.hid || hid;
            s.found.rssi = std::max(s.found.rssi, (int)d.rssi);
            if (s.found.name.empty() && name[0]) {
                s.found.name = name;
            }
            return;
        }
    }
    if (s_seen.size() < MAX_SEEN) {
        Seen s;
        s.found.addr_type = d.addr.type;
        copy_addr(s.found.addr, d.addr.val);
        s.found.name = name;
        s.found.rssi = d.rssi;
        s.hid = hid;
        s_seen.push_back(s);
    }
}

static void start_pair_connect()
{
    ble_addr_t addr;
    addr.type = s_pairing.addr_type;
    copy_addr(addr.val, s_pairing.addr);
    struct ble_gap_conn_params p = conn_params(false);
    s_user_pairing = true;
    if (ble_gap_connect(s_own_addr_type, &addr, PAIR_CONNECT_MS, &p, gap_event, NULL) != 0) {
        s_user_pairing = false;
        message("Couldn't connect to " + shown_name(s_pairing.name) + " - try again.");
        set_state(State::Idle);
        release_background();
        return;
    }
    set_state(State::Connecting);
}

static std::string paired_name(const ble_addr_t& addr)
{
    for (const auto& d : bt_store::devices()) {
        if (bt_store::same(d, addr.type, addr.val)) {
            return d.name;
        }
    }
    return "";
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            on_advertisement(event->disc);
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scanning = false;
            xSemaphoreGive(s_scan_done);
            return 0;

        case BLE_GAP_EVENT_CONNECT: {
            bool background = s_bg_connect;
            s_bg_connect = false;
            s_waiting = false;
            if (event->connect.status != 0) {
                // Failed, timed out, or the background connect was cancelled for a scan/pairing
                // (the user's connect may already be on its way then: it reports for itself)
                if (!background && s_user_pairing) {
                    s_user_pairing = false;
                    message("Couldn't connect to " + shown_name(s_pairing.name) +
                            ". Is it on and in pairing mode?");
                    s_hold = false;
                }
                if (!s_user_pairing) {
                    set_state(State::Idle);
                }
                background_connect();
                return 0;
            }
            s_conn = event->connect.conn_handle;
            if (background) {
                s_user_pairing = false;   // A paired device came back
            }
            struct ble_gap_conn_desc desc;
            std::string name = s_user_pairing ? s_pairing.name : "";
            if (!s_user_pairing && ble_gap_conn_find(s_conn, &desc) == 0) {
                name = paired_name(desc.peer_id_addr);
            }
            {
                BtLock lock;
                s_device = name;
            }
            set_state(State::Pairing);
            // HID needs an encrypted link: pairs a new device, restores a paired one's keys
            if (ble_gap_security_initiate(s_conn) != 0) {
                end_link("Pairing couldn't start.");
            }
            return 0;
        }

        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.conn_handle != s_conn) {
                return 0;
            }
            s_has_passkey = false;
            if (event->enc_change.status == 0) {
                start_setup();
            } else {
                ESP_LOGW(TAG, "Encryption failed (%d)", event->enc_change.status);
                std::string name;
                {
                    BtLock lock;
                    name = s_device;
                }
                end_link(("Pairing with " + shown_name(name) +
                          " failed. Wrong code, or forget it here and on the device, then pair again.")
                             .c_str());
            }
            return 0;

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            struct ble_sm_io io;
            memset(&io, 0, sizeof(io));
            io.action = event->passkey.params.action;
            if (io.action == BLE_SM_IOACT_DISP) {
                // The keyboard types this code; it is shown on the screen until pairing ends
                io.passkey = esp_random() % 1000000;
                s_passkey = io.passkey;
                s_has_passkey = true;
                changed();
            } else if (io.action == BLE_SM_IOACT_NUMCMP) {
                io.numcmp_accept = 1;
            } else {
                return 0;   // The radio has no way to enter a code shown by the device
            }
            ble_sm_inject_io(event->passkey.conn_handle, &io);
            mbedtls_platform_zeroize(&io, sizeof(io));
            return 0;
        }

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The device lost our bond (it was reset): drop the old keys and pair again
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.conn_handle == s_conn && !event->notify_rx.indication) {
                on_notify(event->notify_rx.attr_handle, event->notify_rx.om);
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT: {
            if (event->disconnect.conn.conn_handle != s_conn) {
                return 0;
            }
            State was = s_state;
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_has_passkey = false;
            s_n_reports = 0;
            if (s_user_pairing && (was == State::Pairing || was == State::Setup)) {
                message("Pairing with " + shown_name(s_pairing.name) + " didn't finish.");
            }
            s_user_pairing = false;
            {
                BtLock lock;
                s_device.clear();
            }
            set_state(State::Idle);
            if (s_pending_pair) {
                s_pending_pair = false;
                start_pair_connect();
            } else {
                s_hold = false;
                background_connect();
            }
            return 0;
        }

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Start and stop
// ---------------------------------------------------------------------------

static void on_sync()
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    xSemaphoreGive(s_synced);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth host reset (%d)", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();   // Returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

esp_err_t bt_hid::start()
{
    if (s_started) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_synced = xSemaphoreCreateBinary();
        s_scan_done = xSemaphoreCreateBinary();
        s_keys = xQueueCreate(KEY_QUEUE, sizeof(uint32_t));
        s_map = (hid_map_t*)heap_caps_malloc(sizeof(hid_map_t), MALLOC_CAP_SPIRAM);
        s_map_bytes = (uint8_t*)heap_caps_malloc(HID_MAX_MAP_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!s_lock || !s_synced || !s_scan_done || !s_keys || !s_map || !s_map_bytes) {
        set_state(State::Unavailable);
        return ESP_ERR_NO_MEM;
    }
    // WiFi / Bluetooth coexistence needs WiFi modem sleep while Bluetooth runs
    wifi::set_power_save(true);
    esp_err_t err = board_bt_prepare();
    if (err == ESP_OK) {
        err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
            board_bt_release();
        }
    }
    if (err != ESP_OK) {
        wifi::set_power_save(false);
        set_state(State::Unavailable);
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // The radio shows a code for a keyboard to type (passkey entry); remotes pair "just works"
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    ble_hs_cfg.store_read_cb = ble_store_config_read;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    load_bonds();

    xSemaphoreTake(s_synced, 0);
    nimble_port_freertos_init(host_task);
    if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Bluetooth controller didn't answer");
        nimble_port_stop();
        nimble_port_deinit();
        board_bt_release();
        wifi::set_power_save(false);
        set_state(State::Unavailable);
        return ESP_ERR_TIMEOUT;
    }
    s_hold = false;
    s_started = true;
    set_state(State::Idle);
    background_connect();
    return ESP_OK;
}

void bt_hid::stop()
{
    if (s_started) {
        s_started = false;   // No reconnect from the events that follow
        s_hold = true;
        if (s_scanning) {
            ble_gap_disc_cancel();
        }
        if (ble_gap_conn_active()) {
            ble_gap_conn_cancel();
        }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            for (int i = 0; i < 20 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        nimble_port_stop();
        nimble_port_deinit();
        board_bt_release();
        wifi::set_power_save(false);
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_waiting = false;
    s_scanning = false;
    s_has_passkey = false;
    s_user_pairing = false;
    s_pending_pair = false;
    if (s_lock) {
        BtLock lock;
        s_device.clear();
    }
    set_state(State::Off);
}

// ---------------------------------------------------------------------------
// App task / worker API
// ---------------------------------------------------------------------------

State bt_hid::state()
{
    return s_state;
}

bool bt_hid::waiting()
{
    return s_started && s_waiting;
}

std::string bt_hid::device()
{
    if (!s_lock) {
        return "";
    }
    BtLock lock;
    return s_device;
}

bool bt_hid::is_connected(const bt_store::Device& device)
{
    struct ble_gap_conn_desc desc;
    uint16_t conn = s_conn;
    return s_state == State::Connected && conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn, &desc) == 0 &&
           bt_store::same(device, desc.peer_id_addr.type, desc.peer_id_addr.val);
}

bool bt_hid::passkey(uint32_t& code)
{
    code = s_passkey;
    return s_has_passkey;
}

std::string bt_hid::take_message()
{
    if (!s_lock) {
        return "";
    }
    BtLock lock;
    std::string m;
    m.swap(s_message);
    return m;
}

uint32_t bt_hid::version()
{
    return s_version;
}

esp_err_t bt_hid::scan(std::vector<Found>& out)
{
    out.clear();
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    hold_background();
    {
        BtLock lock;
        s_seen.clear();
    }
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.itvl = 0x0060;          // 30 ms in 60: WiFi keeps streaming meanwhile
    p.window = 0x0030;
    p.passive = 0;            // Active: the scan response carries most devices' names
    p.filter_duplicates = 0;  // ... and comes as a second report from the same address
    xSemaphoreTake(s_scan_done, 0);
    State before = s_state;
    s_scanning = true;
    set_state(State::Scanning);
    esp_err_t err = ESP_OK;
    if (ble_gap_disc(s_own_addr_type, SCAN_MS, &p, gap_event, NULL) != 0) {
        s_scanning = false;
        err = ESP_FAIL;
    } else if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(SCAN_MS + 2000)) != pdTRUE) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
    set_state(before == State::Scanning ? State::Idle : before);
    {
        BtLock lock;
        for (const auto& s : s_seen) {
            if (s.hid) {
                out.push_back(s.found);
            }
        }
        s_seen.clear();
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.rssi > b.rssi; });
    if (out.size() > MAX_FOUND) {
        out.resize(MAX_FOUND);
    }
    release_background();
    return err;
}

esp_err_t bt_hid::pair(const Found& device)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pairing.addr_type = device.addr_type;
    copy_addr(s_pairing.addr, device.addr);
    s_pairing.name = device.name;
    {
        BtLock lock;
        s_device = device.name;
    }
    hold_background();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        // One device at a time: the current one goes, then this one connects (DISCONNECT event)
        s_pending_pair = true;
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        set_state(State::Connecting);
        return ESP_OK;
    }
    for (int i = 0; i < 10 && ble_gap_conn_active(); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));   // The background connect is being cancelled
    }
    start_pair_connect();
    return ESP_OK;
}

void bt_hid::forget(const bt_store::Device& device)
{
    if (s_started) {
        ble_addr_t addr;
        addr.type = device.addr_type;
        copy_addr(addr.val, device.addr);
        struct ble_gap_conn_desc desc;
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(s_conn, &desc) == 0 &&
            ble_addr_cmp(&desc.peer_id_addr, &addr) == 0) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        ble_store_util_delete_peer(&addr);   // NimBLE's copy, and the vault's (store_delete)
    }
    bt_store::forget(device.addr_type, device.addr);
    if (s_started && !s_hold && s_conn == BLE_HS_CONN_HANDLE_NONE) {
        hold_background();   // The white list changed
        release_background();
    }
}

uint32_t bt_hid::read_key()
{
    uint32_t key = 0;
    if (s_keys && xQueueReceive(s_keys, &key, 0) == pdTRUE) {
        return key;
    }
    return 0;
}
