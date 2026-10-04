#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG="WAKE_SCAN";
static uint8_t own_addr_type;

static void print_hex(const uint8_t *p, int n){
    for(int i=0;i<n;i++){ printf("%02X",p[i]); if(i+1<n) printf(" "); }
    printf("\n");
}

static int gap_event(struct ble_gap_event *event, void *arg){
    if(event->type!=BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields f;
    memset(&f,0,sizeof(f));
    ble_hs_adv_parse_fields(&f,event->disc.data,event->disc.length_data);

    bool name_match = f.name && f.name_len==9 && memcmp(f.name,"HDRC-BV1",9)==0;
    if(!name_match) return 0;

    char addr[BLE_ADDR_STR_LEN];
    ble_addr_to_str(&event->disc.addr,addr);

    printf("\nADV addr=%s type=%u rssi=%d len=%u\n",
           addr,event->disc.addr.type,event->disc.rssi,event->disc.length_data);
    printf("RAW=");
    print_hex(event->disc.data,event->disc.length_data);

    if(f.mfg_data && f.mfg_data_len){
        printf("MFG=");
        print_hex(f.mfg_data,f.mfg_data_len);
    }
    if(f.name && f.name_len){
        printf("NAME=%.*s\n",f.name_len,f.name);
    }
    return 0;
}

static void start_scan(void){
    struct ble_gap_disc_params p={0};
    p.passive=0;
    p.filter_duplicates=0;
    int rc=ble_gap_disc(own_addr_type,BLE_HS_FOREVER,&p,gap_event,NULL);
    if(rc) ESP_LOGE(TAG,"scan start rc=%d",rc);
    else ESP_LOGI(TAG,"Scanning HDRC-BV1 continuously. Turn TV off, then press original remote POWER.");
}

static void on_sync(void){
    int rc=ble_hs_id_infer_auto(0,&own_addr_type);
    if(rc){ ESP_LOGE(TAG,"addr type rc=%d",rc); return; }
    start_scan();
}
static void on_reset(int reason){ ESP_LOGE(TAG,"reset reason=%d",reason); }
static void host_task(void *p){ nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void){
    esp_err_t ret=nvs_flash_init();
    if(ret==ESP_ERR_NVS_NO_FREE_PAGES || ret==ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret=nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb=on_reset;
    ble_hs_cfg.sync_cb=on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("ESP32C3-WakeScan");
    nimble_port_freertos_init(host_task);
}
