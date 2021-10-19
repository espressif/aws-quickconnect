#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "string.h"

#include "credentials.h"
#include "core_mqtt.h"
#include "networking.h"

#include "driver/temp_sensor.h"

/* self claiming */
#include "esp_rmaker_core.h"
#include "esp_rmaker_client_data.h"
#include "esp_rmaker_claim.h"

/* Network Event Group Bit Definitions */
#define WIFI_CONNECTED_BIT           (1 << 0)
#define WIFI_DISCONNECTED_BIT        (1 << 1)
#define IP_GOT_BIT                   (1 << 2)
#define SELF_CLAIM_PERFORMED_BIT     (1 << 3)
#define SELF_CLAIM_NOT_PERFORMED_BIT (1 << 4)
#define TLS_CONNECTED_BIT            (1 << 5)
#define TLS_DISCONNECTED_BIT         (1 << 6)
#define MQTT_CONNECTED_BIT           (1 << 7)
#define MQTT_DISCONNECTED_BIT        (1 << 8)

static const char* TAG = "FMConnectMain";

/* Provisioned Device Connection Credentials */
static char* pcThingName = "7CDFA1B3926C";
static char* pcWifiSsid = NULL;
static char* pcWifiPass = NULL;
static char* pcEndpoint = NULL;
static char* pcClientCert = NULL;
static char* pcClientKey = NULL;

/* Networking Globals */
EventGroupHandle_t xNetworkEventGroup;
static MQTTContext_t xMQTTContext = { 0 };
static NetworkContext_t xNetworkContext = { 0 };

static void wifiEventHandler(void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "WIFI CONNECTED!");
        xEventGroupSetBits(xNetworkEventGroup, WIFI_CONNECTED_BIT);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "WIFI DISCONNECTED! Attempting to reconnect...");
        xEventGroupClearBits(xNetworkEventGroup, WIFI_CONNECTED_BIT | IP_GOT_BIT);
        xEventGroupSetBits(xNetworkEventGroup, WIFI_DISCONNECTED_BIT);
        break;
    default:
        break;
    }
}

static void ipEventHandler(void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data)
{
    switch (event_id)
    {
    case IP_EVENT_STA_GOT_IP:
        xEventGroupSetBits(xNetworkEventGroup, IP_GOT_BIT);
        break;
    default:
        break;
    }
}

void vSelfClaimTask(void* pvParameters)
{
    (void)pvParameters;

    esp_rmaker_config_t rainmaker_cfg = {
                .enable_time_sync = false,
    };
    esp_rmaker_node_t* node = esp_rmaker_node_init(&rainmaker_cfg, "FMConnect", "FMConnect");
    ESP_LOGI(TAG, "rmaker init done");
    if (esp_rmaker_get_client_cert() == NULL)
    {
        do
        {
            vTaskDelay(10);
            xEventGroupWaitBits(xNetworkEventGroup,
                WIFI_CONNECTED_BIT | IP_GOT_BIT,
                pdFALSE,
                pdTRUE,
                portMAX_DELAY);
        }         while (esp_rmaker_self_claim_perform(esp_rmaker_self_claim_init()) != ESP_OK);
    }

    esp_rmaker_node_deinit(node);

    pcClientCert = esp_rmaker_get_client_cert();
    pcClientKey = esp_rmaker_get_client_key();

    ESP_LOGI(TAG, "Self-claiming performed.");

    xEventGroupSetBits(xNetworkEventGroup, SELF_CLAIM_PERFORMED_BIT);

    xEventGroupWaitBits(xNetworkEventGroup,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);

    /* Send device credentials out so they can be registered */
    printf("DEVICE_CERT_START\n%s\nDEVICE_CERT_END\n", pcClientCert);
    printf("DEVICE_THING_NAME_START\n%s\nDEVICE_THING_NAME_END\n", pcThingName);
    vTaskDelete(NULL);
}

void vTlsConnectionTask(void* pvParameters)
{
    BaseType_t ret;
    (void)pvParameters;

    /* Wait for the device to perform Self-Claiming and have an IP */
    xEventGroupWaitBits(xNetworkEventGroup,
        SELF_CLAIM_PERFORMED_BIT | IP_GOT_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);

    /* If a connection was previously established, then close it to free memory */
    if (xNetworkContext.pxTls != NULL)
    {
        ESP_LOGI(TAG, "TLS DISCONNECTED!");
        tlsDisconnect(&xNetworkContext);
    }

    ESP_LOGI(TAG, "Establishing a TLS connection...");
    ret = tlsConnect(&xNetworkContext,
        pcEndpoint,
        port,
        server_cert_pem,
        pcClientCert,
        pcClientKey);

    if (ret == pdTRUE)
    {
        ESP_LOGI(TAG, "TLS CONNECTED!");
        /* Flag that a TLS connection has been established */
        xEventGroupSetBits(xNetworkEventGroup,
            TLS_CONNECTED_BIT);
    }
    else
    {
        /* Flag that a TLS connection was not established */
        xEventGroupSetBits(xNetworkEventGroup,
            TLS_DISCONNECTED_BIT);
    }

    vTaskDelete(NULL);
}

void vMqttConnectionTask(void* pvParameters)
{
    MQTTStatus_t ret;
    (void)pvParameters;

    /* Wait for device to have a TLS connection */
    xEventGroupWaitBits(xNetworkEventGroup,
        TLS_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);

    ESP_LOGI(TAG, "Establishing an MQTT connection...");

    ret = mqttConnect(&xMQTTContext,
        pcThingName);

    if (ret == MQTTSuccess)
    {
        ESP_LOGI(TAG, "MQTT CONNECTED!");
        xEventGroupSetBits(xNetworkEventGroup,
            MQTT_CONNECTED_BIT);
    }
    else if (ret == MQTTNoMemory)
    {
        ESP_LOGE(TAG, "vMqttTask: xMQTTContext.networkBuffer is too small to send the connection packet.");
    }
    else if (ret == MQTTSendFailed || ret == MQTTRecvFailed)
    {
        ESP_LOGE(TAG, "vMqttTask: Send or Receive failed.");
        xEventGroupClearBits(xNetworkEventGroup,
            TLS_CONNECTED_BIT);
        xEventGroupSetBits(xNetworkEventGroup,
            TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT);
    }
    else
    {
        ESP_LOGE(TAG, "MQTT_Status: %s", MQTT_Status_strerror(ret));
        xEventGroupSetBits(xNetworkEventGroup,
            MQTT_DISCONNECTED_BIT);
    }

    vTaskDelete(NULL);
}

void vNetworkHandlingTask(void* pvParameters)
{
    /* Initialize state */
    EventBits_t uxNetworkEventBits;
    xNetworkEventGroup = xEventGroupCreate();
    xEventGroupSetBits(xNetworkEventGroup,
        WIFI_DISCONNECTED_BIT | SELF_CLAIM_NOT_PERFORMED_BIT | TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT);

    while (1)
    {
        uxNetworkEventBits = xEventGroupWaitBits(xNetworkEventGroup,
            WIFI_DISCONNECTED_BIT | SELF_CLAIM_NOT_PERFORMED_BIT | TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);

        if ((uxNetworkEventBits & WIFI_DISCONNECTED_BIT) != 0)
        {
            /* Establish a WiFi connection */
            ESP_LOGI(TAG, "Connecting to WiFi...");
            ESP_ERROR_CHECK(esp_wifi_connect());
        }

        if ((uxNetworkEventBits & SELF_CLAIM_NOT_PERFORMED_BIT) != 0)
        {
            /* Perform Self-Claiming */
            xTaskCreate(vSelfClaimTask, "SelfClaimTask", 3072, NULL, 1, NULL);
        }

        if ((uxNetworkEventBits & TLS_DISCONNECTED_BIT) != 0)
        {
            /* Establish a TLS connection */
            xTaskCreate(vTlsConnectionTask, "TlsConnectionTask", 3072, NULL, 1, NULL);
        }

        if ((uxNetworkEventBits & MQTT_DISCONNECTED_BIT) != 0)
        {
            /* Establish an MQTT connection */
            xTaskCreate(vMqttConnectionTask, "MqttConnectionTask", 3072, NULL, 1, NULL);
        }
    }

    vTaskDelete(NULL);
}

void vSensorSendingTask(void* arg)
{
    MQTTStatus_t ret;
    char pcSendBuffer[256];

    float tsens_out;
    temp_sensor_config_t temp_sensor = TSENS_CONFIG_DEFAULT();
    temp_sensor_get_config(&temp_sensor);
    temp_sensor.dac_offset = TSENS_DAC_DEFAULT;
    temp_sensor_set_config(temp_sensor);
    temp_sensor_start();

    while (1) 
    {
        vTaskDelay(1000 / portTICK_RATE_MS);

        /* Wait for device to be connected to MQTT */
        xEventGroupWaitBits(xNetworkEventGroup,
            MQTT_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY);

        temp_sensor_read_celsius(&tsens_out);

        snprintf(pcSendBuffer,
            256,
            "{ \"Temperature\": { \"unit\": \"C\", \"value\" : %f} }",
            tsens_out);

        ret = mqttPublishAndReceiveFMConnect(&xMQTTContext, pcThingName, pcSendBuffer);

        if (ret != MQTTSuccess)
        {
            /* Flag that the TLS connection and MQTT connection have been dropped */
            xEventGroupClearBits(xNetworkEventGroup,
                TLS_CONNECTED_BIT | MQTT_CONNECTED_BIT);
            xEventGroupSetBits(xNetworkEventGroup,
                TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT);
        }
    }
    vTaskDelete(NULL);
}
void app_main(void)
{
    size_t lengthRequired;
    nvs_handle_t xNvsHandle;

    /* Initialize Non-Volatile Storage
     * Necessary for:
     * - WiFi drivers to store configs
     * - For getting user-provided WiFi Credentials (ssid, password)
     * - For getting endpoint that the demo connects to
     * - For getting/setting private key and device certificate */
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Open 15-min-connect provisioning namespace from NVS */
    ESP_ERROR_CHECK(nvs_open("FMC", NVS_READONLY, &xNvsHandle));

    /* Extract WiFi SSID from NVS */
    ESP_ERROR_CHECK(nvs_get_str(xNvsHandle, "wifiSsid", NULL, &lengthRequired));
    pcWifiSsid = malloc(lengthRequired);
    ESP_ERROR_CHECK(nvs_get_str(xNvsHandle, "wifiSsid", pcWifiSsid, &lengthRequired));

    /* Extract WiFi password from NVS */
    ESP_ERROR_CHECK(nvs_get_str(xNvsHandle, "wifiPass", NULL, &lengthRequired));
    pcWifiPass = malloc(lengthRequired);
    ESP_ERROR_CHECK(nvs_get_str(xNvsHandle, "wifiPass", pcWifiPass, &lengthRequired));

    /* Extract endpoint from NVS */
    ESP_ERROR_CHECK(nvs_get_str(xNvsHandle, "endpoint", NULL, &lengthRequired));
    pcEndpoint = malloc(lengthRequired);
    ESP_ERROR_CHECK(nvs_get_str(xNvsHandle, "endpoint", pcEndpoint, &lengthRequired));

    /* Initialize Espressif's default event loop that will handle propagating events for Espressif's:
     * -WiFi
     * -TCP/IP */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Add event handlers to the default event loop */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifiEventHandler,
        NULL,
        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        ESP_EVENT_ANY_ID,
        &ipEventHandler,
        NULL,
        NULL));

    /* Initialize networking. This initializes the TCP/IP stack, WiFi, and coreMQTT context */
    networkingInit(&xNetworkContext, &xMQTTContext);

    /* Set wifi credentials to connect to the provisioned WiFi access point */
    setWifiCredentials(pcWifiSsid, pcWifiPass);

    /* Start task that handles setting up and maintaining the network connection */
    xTaskCreate(vNetworkHandlingTask, "NetworkEventHandlingTask", 2048, NULL, 2, NULL);

    /* Start task that handles getting and sending sensor data */
    xTaskCreate(vSensorSendingTask, "SensorSendingTask", 3072, NULL, 1, NULL);

    return;
}