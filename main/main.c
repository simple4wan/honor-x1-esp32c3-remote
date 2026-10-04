#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gattc.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "HONOR_X1";
static const char *TARGET_NAME = "HDRC-BV1";

static uint8_t own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t hid_start = 0;
static uint16_t hid_end = 0;
static uint16_t report_map_handle = 0;

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
static void discover_hid_chrs(void);
static void discover_next_report_dscs(void);
static void configure_next_report(void);

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
        ESP_LOGI(TAG, "All reports configured. Press POWER on HDRC-BV1 now.");
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
    discover_hid();
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
        ble_gap_disc_cancel();

        int rc = ble_gap_connect(own_addr_type, &event->disc.addr, 30000,
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
            int rc = ble_gap_security_initiate(conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "Security initiate rc=%d", rc);
            }
        }

        {
            int rc = ble_gattc_exchange_mtu(conn_handle, mtu_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "MTU exchange start rc=%d; continuing", rc);
                discover_hid();
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_PASSKEY:
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

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("ESP32C3-HonorSniffer");

    nimble_port_freertos_init(host_task);
}
