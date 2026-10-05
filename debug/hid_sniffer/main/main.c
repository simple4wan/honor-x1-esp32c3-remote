#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_store.h"

void ble_store_config_init(void);

static const char *TAG = "HONOR_X1";
static const char *TARGET_NAME = "HDRC-BV1";

static uint8_t own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t hid_start = 0;
static uint16_t hid_end = 0;
static uint16_t report_map_handle = 0;

static uint16_t dis_start = 0;
static uint16_t dis_end = 0;
struct identity_chr {
    uint16_t uuid;
    uint16_t handle;
    const char *name;
};
static struct identity_chr identity_chrs[] = {
    {0x2A29, 0, "Manufacturer Name"},
    {0x2A24, 0, "Model Number"},
    {0x2A25, 0, "Serial Number"},
    {0x2A26, 0, "Firmware Revision"},
    {0x2A27, 0, "Hardware Revision"},
    {0x2A28, 0, "Software Revision"},
    {0x2A50, 0, "PnP ID"},
};
static int identity_read_index = 0;

#define MAX_REPORTS 8
struct report_info {
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t end_handle;
    uint16_t cccd_handle;
    uint16_t ref_handle;
    uint8_t report_id;
    uint8_t report_type;
};
static struct report_info reports[MAX_REPORTS];
static int report_count = 0;
static int report_index = 0;

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);
static void discover_hid(void);
static void discover_identity(void);
static void read_next_identity(void);
static void discover_hid_chrs(void);
static void log_bond_state(const char *where);
static void discover_next_report_dscs(void);
static void configure_next_report(void);
static int write_cccd_cb(uint16_t ch, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg);

static void log_bond_state(const char *where)
{
    int our_sec = 0;
    int peer_sec = 0;
    int rc1 = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &our_sec);
    int rc2 = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &peer_sec);
    ESP_LOGI(TAG, "BOND STATE [%s]: our_sec=%d(rc=%d) peer_sec=%d(rc=%d)",
             where, our_sec, rc1, peer_sec, rc2);
}

static void print_mbuf(const struct os_mbuf *om)
{
    while (om) {
        for (int i = 0; i < om->om_len; i++) {
            printf("%02X", om->om_data[i]);
            if (i + 1 < om->om_len || SLIST_NEXT(om, om_next)) printf(" ");
        }
        om = SLIST_NEXT(om, om_next);
    }
    printf("\n");
}

static int read_identity_cb(uint16_t ch, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    struct identity_chr *c = (struct identity_chr *)arg;
    if (error->status == 0 && attr && attr->om) {
        printf("\nIDENTITY %s (0x%04X) = ", c->name, c->uuid);
        if (c->uuid == 0x2A50) {
            print_mbuf(attr->om);
        } else {
            int len = OS_MBUF_PKTLEN(attr->om);
            uint8_t buf[128] = {0};
            int copy = len < (int)sizeof(buf)-1 ? len : (int)sizeof(buf)-1;
            if (ble_hs_mbuf_to_flat(attr->om, buf, copy, NULL) == 0) {
                printf("%.*s\n", copy, (char *)buf);
            } else {
                print_mbuf(attr->om);
            }
        }
    } else {
        ESP_LOGW(TAG, "Identity read failed %s status=%d", c->name, error->status);
    }
    read_next_identity();
    return 0;
}

static void read_next_identity(void)
{
    while (identity_read_index < (int)(sizeof(identity_chrs)/sizeof(identity_chrs[0]))) {
        struct identity_chr *c = &identity_chrs[identity_read_index++];
        if (!c->handle) continue;
        int rc = ble_gattc_read(conn_handle, c->handle, read_identity_cb, c);
        if (rc == 0) return;
        ESP_LOGW(TAG, "Could not start identity read %s rc=%d", c->name, rc);
    }
    ESP_LOGI(TAG, "=== END DEVICE IDENTITY ===");
    discover_hid();
}

static int dis_chr_cb(uint16_t ch, const struct ble_gatt_error *error,
                      const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr) {
        uint16_t uuid16 = ble_uuid_u16(&chr->uuid.u);
        for (int i = 0; i < (int)(sizeof(identity_chrs)/sizeof(identity_chrs[0])); i++) {
            if (identity_chrs[i].uuid == uuid16) {
                identity_chrs[i].handle = chr->val_handle;
                ESP_LOGI(TAG, "Identity characteristic %s uuid=0x%04X handle=0x%04X",
                         identity_chrs[i].name, uuid16, chr->val_handle);
            }
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        identity_read_index = 0;
        ESP_LOGI(TAG, "=== DEVICE IDENTITY ===");
        read_next_identity();
        return 0;
    }

    ESP_LOGW(TAG, "Device Information characteristic discovery status=%d", error->status);
    discover_hid();
    return 0;
}

static int dis_svc_cb(uint16_t ch, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc) {
        dis_start = svc->start_handle;
        dis_end = svc->end_handle;
        ESP_LOGI(TAG, "Device Information service found start=0x%04X end=0x%04X",
                 dis_start, dis_end);
        for (int i = 0; i < (int)(sizeof(identity_chrs)/sizeof(identity_chrs[0])); i++) {
            identity_chrs[i].handle = 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn_handle, dis_start, dis_end, dis_chr_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Device Information characteristic discovery start rc=%d", rc);
            discover_hid();
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (!dis_start) {
            ESP_LOGW(TAG, "Device Information service 0x180A not found");
            discover_hid();
        }
        return 0;
    }

    ESP_LOGW(TAG, "Device Information service discovery status=%d", error->status);
    discover_hid();
    return 0;
}

static void discover_identity(void)
{
    dis_start = dis_end = 0;
    ble_uuid16_t uuid = BLE_UUID16_INIT(0x180A);
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &uuid.u, dis_svc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Device Information discovery start rc=%d", rc);
        discover_hid();
    }
}

static int read_report_map_cb(uint16_t ch, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "Report Map read failed: status=%d", error->status);
        return 0;
    }

    ESP_LOGI(TAG, "=== HID REPORT MAP ===");
    if (attr && attr->om) {
        print_mbuf(attr->om);
    }
    ESP_LOGI(TAG, "=== END REPORT MAP ===");
    return 0;
}

static int read_report_ref_cb(uint16_t ch, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    struct report_info *r = (struct report_info *)arg;

    if (error->status == 0 && attr && attr->om) {
        uint8_t buf[2] = {0};
        int len = OS_MBUF_PKTLEN(attr->om);
        if (len >= 2 && ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), NULL) == 0) {
            r->report_id = buf[0];
            r->report_type = buf[1];
            ESP_LOGI(TAG, "Report val=0x%04X reference: id=%u type=%u",
                     r->val_handle, r->report_id, r->report_type);
        }
    } else {
        ESP_LOGW(TAG, "Report Reference read failed for val=0x%04X status=%d",
                 r->val_handle, error->status);
    }

    if (r->cccd_handle) {
        uint16_t notify = htole16(0x0001);
        int rc = ble_gattc_write_flat(conn_handle, r->cccd_handle,
                                      &notify, sizeof(notify),
                                      write_cccd_cb, r);
        if (rc == 0) {
            return 0;
        }
        ESP_LOGW(TAG, "Could not start CCCD write rc=%d", rc);
    }

    configure_next_report();
    return 0;
}

static int write_cccd_cb(uint16_t ch, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    struct report_info *r = (struct report_info *)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed Report val=0x%04X id=%u",
                 r->val_handle, r->report_id);
    } else {
        ESP_LOGW(TAG, "CCCD write failed val=0x%04X status=%d",
                 r->val_handle, error->status);
    }

    configure_next_report();
    return 0;
}

static void configure_next_report(void)
{
    if (report_index >= report_count) {
        ESP_LOGI(TAG, "All reports configured. HID ready. Press MENU on HDRC-BV1 now; raw Report ID/data will be printed.");
        return;
    }

    struct report_info *r = &reports[report_index++];

    if (r->ref_handle) {
        int rc = ble_gattc_read(conn_handle, r->ref_handle, read_report_ref_cb, r);
        if (rc == 0) {
            return;
        }
        ESP_LOGW(TAG, "Could not start Report Reference read rc=%d", rc);
    }

    if (r->cccd_handle) {
        uint16_t notify = htole16(0x0001);
        int rc = ble_gattc_write_flat(conn_handle, r->cccd_handle,
                                      &notify, sizeof(notify),
                                      write_cccd_cb, r);
        if (rc == 0) {
            return;
        }
        ESP_LOGW(TAG, "Could not start CCCD write rc=%d", rc);
    }

    configure_next_report();
}

static int dsc_disc_cb(uint16_t ch, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc, void *arg)
{
    struct report_info *r = &reports[report_index];

    if (error->status == 0 && dsc) {
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2908) {
            r->ref_handle = dsc->handle;
        } else if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
            r->cccd_handle = dsc->handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Report[%d] val=0x%04X ref=0x%04X cccd=0x%04X",
                 report_index, r->val_handle, r->ref_handle, r->cccd_handle);
        report_index++;
        discover_next_report_dscs();
        return 0;
    }

    ESP_LOGE(TAG, "Descriptor discovery failed status=%d", error->status);
    return 0;
}

static void discover_next_report_dscs(void)
{
    if (report_index >= report_count) {
        report_index = 0;
        configure_next_report();
        return;
    }

    struct report_info *r = &reports[report_index];
    uint16_t end = r->end_handle;
    if (end <= r->val_handle) {
        report_index++;
        discover_next_report_dscs();
        return;
    }

    int rc = ble_gattc_disc_all_dscs(conn_handle, r->val_handle, end,
                                     dsc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Descriptor discovery start failed rc=%d", rc);
        report_index++;
        discover_next_report_dscs();
    }
}

static int chr_disc_cb(uint16_t ch, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr) {
        uint16_t uuid16 = ble_uuid_u16(&chr->uuid.u);

        if (uuid16 == 0x2A4B) {
            report_map_handle = chr->val_handle;
            ESP_LOGI(TAG, "Report Map handle=0x%04X", report_map_handle);
        } else if (uuid16 == 0x2A4D && report_count < MAX_REPORTS) {
            reports[report_count].def_handle = chr->def_handle;
            reports[report_count].val_handle = chr->val_handle;
            reports[report_count].end_handle = hid_end;
            if (report_count > 0) {
                reports[report_count - 1].end_handle = chr->def_handle - 1;
            }
            ESP_LOGI(TAG, "Report characteristic[%d] def=0x%04X val=0x%04X props=0x%02X",
                     report_count, chr->def_handle, chr->val_handle, chr->properties);
            report_count++;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (report_map_handle) {
            int rc = ble_gattc_read(conn_handle, report_map_handle,
                                    read_report_map_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "Could not start Report Map read rc=%d", rc);
            }
        }

        report_index = 0;
        discover_next_report_dscs();
        return 0;
    }

    ESP_LOGE(TAG, "Characteristic discovery failed status=%d", error->status);
    return 0;
}

static void discover_hid_chrs(void)
{
    report_map_handle = 0;
    report_count = 0;
    memset(reports, 0, sizeof(reports));

    int rc = ble_gattc_disc_all_chrs(conn_handle, hid_start, hid_end,
                                     chr_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Characteristic discovery start failed rc=%d", rc);
    }
}

static int svc_disc_cb(uint16_t ch, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc) {
        hid_start = svc->start_handle;
        hid_end = svc->end_handle;
        ESP_LOGI(TAG, "HID service found start=0x%04X end=0x%04X", hid_start, hid_end);
        discover_hid_chrs();
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (!hid_start) {
            ESP_LOGE(TAG, "HID service 0x1812 not found");
        }
        return 0;
    }

    ESP_LOGE(TAG, "Service discovery failed status=%d", error->status);
    return 0;
}

static void discover_hid(void)
{
    ble_uuid16_t uuid = BLE_UUID16_INIT(0x1812);
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &uuid.u, svc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "HID service discovery start failed rc=%d", rc);
    }
}

static int mtu_cb(uint16_t ch, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg)
{
    ESP_LOGI(TAG, "MTU exchange status=%d mtu=%u", error->status, mtu);
    discover_identity();
    return 0;
}

static bool adv_name_matches(const struct ble_hs_adv_fields *fields)
{
    return fields->name && fields->name_len == strlen(TARGET_NAME) &&
           memcmp(fields->name, TARGET_NAME, fields->name_len) == 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }

        if (!adv_name_matches(&fields)) {
            return 0;
        }

        ESP_LOGI(TAG, "Found %s RSSI=%d", TARGET_NAME, event->disc.rssi);

        const ble_addr_t *connect_addr = &event->disc.addr;
#if MYNEWT_VAL(BLE_HOST_BASED_PRIVACY)
        // After a bonded peer is resolved by the host, disc.addr may be the
        // identity address while the controller still needs the current OTA
        // address (RPA) for the actual connection attempt.
        connect_addr = &event->disc.ota_addr;
#endif

        ESP_LOGI(TAG,
                 "Connect target type=%u addr=%02X:%02X:%02X:%02X:%02X:%02X",
                 connect_addr->type,
                 connect_addr->val[5], connect_addr->val[4],
                 connect_addr->val[3], connect_addr->val[2],
                 connect_addr->val[1], connect_addr->val[0]);

        ble_gap_disc_cancel();

        int rc = ble_gap_connect(own_addr_type, connect_addr, 30000,
                                 NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Connect failed to start rc=%d", rc);
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed status=%d", event->connect.status);
            start_scan();
            return 0;
        }

        conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "Connected conn_handle=%u", conn_handle);
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(conn_handle, &desc) == 0) {
                ESP_LOGI(TAG,
                         "LINK BEFORE SECURITY: encrypted=%d authenticated=%d bonded=%d peer_id=%02X:%02X:%02X:%02X:%02X:%02X",
                         desc.sec_state.encrypted,
                         desc.sec_state.authenticated,
                         desc.sec_state.bonded,
                         desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                         desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                         desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            }
        }
        log_bond_state("connect");

        {
            int rc = ble_gap_security_initiate(conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "Security initiate rc=%d", rc);
            }
        }

        {
            int rc = ble_gattc_exchange_mtu(conn_handle, mtu_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "MTU exchange start rc=%d; continuing", rc);
                discover_identity();
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        ESP_LOGI(TAG, "Encryption change status=%d", event->enc_change.status);
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG,
                     "LINK AFTER SECURITY: encrypted=%d authenticated=%d bonded=%d key_size=%d",
                     desc.sec_state.encrypted,
                     desc.sec_state.authenticated,
                     desc.sec_state.bonded,
                     desc.sec_state.key_size);
        }
        log_bond_state("enc_change");
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "=== BOND TEST SUCCESS PATH: reboot ESP32, do NOT press Home+Menu, then press any remote key ===");
        }
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "Pairing action=%d", event->passkey.params.action);
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            struct ble_sm_io pkey = {0};
            pkey.action = BLE_SM_IOACT_NUMCMP;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t report_id = 0;
        for (int i = 0; i < report_count; i++) {
            if (reports[i].val_handle == event->notify_rx.attr_handle) {
                report_id = reports[i].report_id;
                break;
            }
        }

        printf("\nREPORT handle=0x%04X id=%u indication=%u data=",
               event->notify_rx.attr_handle, report_id,
               event->notify_rx.indication);
        print_mbuf(event->notify_rx.om);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected reason=%d", event->disconnect.reason);
        log_bond_state("disconnect");
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        hid_start = hid_end = 0;
        start_scan();
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ESP_LOGW(TAG, "Repeat pairing requested; deleting old bond");
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {0};
    p.passive = 0;
    p.itvl = 0;
    p.window = 0;
    p.filter_duplicates = 0;

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan start failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Scanning for %s ...", TARGET_NAME);
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return;
    }
    log_bond_state("boot/sync");
    ESP_LOGI(TAG, "If first pairing: hold HOME + MENU on HDRC-BV1 until it enters pairing mode.");
    ESP_LOGI(TAG, "After encrypted/bonded success: reboot ESP32 and test reconnect without HOME + MENU.");
    start_scan();
}

static void host_task(void *param)
{
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

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Persist NimBLE bonding keys in NVS so the HDRC-BV1 remains bonded after reboot.
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("ESP32C3-HonorSniffer");

    nimble_port_freertos_init(host_task);
}
