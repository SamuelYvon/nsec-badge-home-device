#include <esp_system.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_netif.h>

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/lwip_napt.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_wifi_netif.h"

#include "save.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define RAND_CHR (char)((esp_random() % 26) + 0x61)

static esp_event_handler_instance_t event_handler_instance;

static bool wifi_config_saved()
{
    return strlen(Save::save_data.wifi_ssid) && strlen(Save::save_data.wifi_password);
}

void Wifi::init()
{
    if(true || !wifi_config_saved()) {
        snprintf((char *)&Save::save_data.wifi_ssid, sizeof(Save::save_data.wifi_ssid), "wfguest");
        snprintf((char *)&Save::save_data.wifi_password, sizeof(Save::save_data.wifi_password), "mobile-tribunal-nectar");
        Save::write_save(); // overwrite it
    }

    Save::save_data.debug_feature_enabled[debug_tab::wifi] = true;

    _enabled = false;
    _state = State::Disabled;

    if(Save::save_data.debug_feature_enabled[debug_tab::wifi]) {
        enable();
    } else {
        disable();
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else {
        ESP_LOGI(TAG, "%s: other event %ld", __func__, event_id);
    }
}

static void on_wifi_evt_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGV(TAG, "Got IPv4 event: Interface \"%s\" address: " IPSTR, esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
}

static void sta_config(wifi_config_t* config) {
  ESP_LOGV(TAG, "%s: Preparing the STA config", __func__);
  strcpy((char *)&config->sta.ssid, Save::save_data.wifi_ssid);
  strcpy((char *)&config->sta.password, Save::save_data.wifi_password);

  config->sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
}

esp_err_t Wifi::enable()
{
    esp_err_t ret;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if(_enabled) {
        ESP_LOGV(TAG, "Already enabled...");
        return ESP_OK;
    }

    sta_config(&_config);

    ESP_LOGV(TAG, "SSID: |%s|", Save::save_data.wifi_ssid);
    ESP_LOGV(TAG, "PWD : |%s|", Save::save_data.wifi_password);

    _enabled = true;

    // Phase 1: init the network network interface
    ret = esp_netif_init();

    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not initialize netif (%s)", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    // Phase 2: create the event loop the process async events
    ret = esp_event_loop_create_default();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not create event loop (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    // Small deviation from the doc here, no default stack?
    ret = esp_wifi_init(&cfg);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not initialize wifi (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_wifi_set_country_code("CA", false);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not set country code (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &event_handler_instance);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not register event handler (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not set wifi AP mode (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_evt_got_ip, NULL);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "%s: Could not register wifi got ip event (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &_config);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not set wifi config (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    // s1.3 in doc
    _netif_sta = esp_netif_create_default_wifi_sta();
    assert(_netif_sta);

    ret = esp_wifi_start();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not start wifi (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_wifi_connect();
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "%s Failed to connect too wifi network (%s)", __func__, esp_err_to_name(ret));
        goto fail;
    }

    _state = State::Enabled;

    return ESP_OK;

fail:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler_instance);
    esp_wifi_deinit();
    esp_event_loop_delete_default();
    _state = State::Failed;
    return ret;
}

esp_err_t Wifi::disable()
{
    esp_err_t ret;

    if(!_enabled) {
        ESP_LOGV(TAG, "Already disabled...");
        return ESP_OK;
    }

    _enabled = false;

    ret = esp_wifi_stop();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not stop wifi (%s)", __func__, esp_err_to_name(ret));
    }

    ret = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler_instance);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not unregister event handler instance (%s)", __func__, esp_err_to_name(ret));
    }

    ret = esp_wifi_deinit();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not deinit wifi (%s)", __func__, esp_err_to_name(ret));
    }

    ret = esp_event_loop_delete_default();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Could not delete default event loop (%s)", __func__, esp_err_to_name(ret));
    }

    _state = State::Disabled;

    return ESP_OK;
}
