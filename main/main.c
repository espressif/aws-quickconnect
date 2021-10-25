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

/* Temperature Sensor Driver */
#include "driver/temp_sensor.h"

/* Self-Claiming  */
#include "esp_rmaker_claim.h"

/* Self-Claiming Definitions */
#define MAX_SELF_CLAIM_ATTEMPTS 10
#define THING_NAME_SIZE 20

/* Task Configs */
#define FMC_TASK_DEFAULT_STACK_SIZE 3072

/* Serial bookends for the utility */
#define SERIAL_CERT_BOOKEND "DEVICE_CERT"
#define SERIAL_THING_NAME_BOOKEND "DEVICE_THING_NAME"

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
static char* pcThingName =  NULL;
static char* pcWifiSsid = NULL;
static char* pcWifiPass = NULL;
static char* pcEndpoint = NULL;
static char* pcDevCert = NULL;
static char* pcDevKey = NULL;

/* Networking Globals */
static EventGroupHandle_t xNetworkEventGroup;
static MQTTContext_t xMQTTContext = { 0 };
static NetworkContext_t xNetworkContext = { 0 };

static char *pcGenerateThingName(void)
{
    uint8_t pucEthMac[6];

    char *pcRet = NULL;

    if (esp_wifi_get_mac(WIFI_IF_STA, pucEthMac) != ESP_OK) {
        ESP_LOGE(TAG, "Could not fetch MAC address. "
            "Initialise Wi-Fi first");
    }
    else
    {
        pcRet = calloc(1, THING_NAME_SIZE);

        if(pcRet == NULL)
        {
            ESP_LOGE(TAG, 
            "Failed to allocate memory for thing name.");
        }
        else
        {
            snprintf(pcRet, THING_NAME_SIZE, "%02X%02X%02X%02X%02X%02X",
                pucEthMac[0], pucEthMac[1], pucEthMac[2], pucEthMac[3], 
                pucEthMac[4], pucEthMac[5]);
        }
    }

    return pcRet;
}

static char *pcNvsGetStr(const char *pcPartitionName, const char *pcNamespace, 
    const char *pcKey)
{
    size_t uxLengthRequired;
    nvs_handle_t xNvsHandle;
    char *pcRet = NULL;
    
    if(pcPartitionName == NULL)
    {
        ESP_LOGE(TAG, "Partition name is null.");

    }
    else if(pcNamespace == NULL)
    {
        ESP_LOGE(TAG, "Namespace is null.");
    }
    else if(pcKey == NULL)
    {
        ESP_LOGE(TAG, "Key is null.");
    }
    else if(nvs_flash_init_partition(pcPartitionName) != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not initialize partition: %s.", pcPartitionName);
    }
    else if(nvs_open_from_partition(pcPartitionName, pcNamespace, NVS_READONLY, 
        &xNvsHandle) != ESP_OK)
    {
        ESP_LOGE(TAG, 
            "Could not open namespace: %s on partition: %s for reading.", 
            pcNamespace, pcPartitionName);
    }
    else
    {
        if(nvs_get_str(xNvsHandle, pcKey, NULL, &uxLengthRequired) != ESP_OK)
        {
            ESP_LOGE(TAG, 
                "Could not open key: %s from namespace: %s on partition: %s "
                "for reading.", pcKey, pcNamespace, pcPartitionName);
        }
        else
        {
            pcRet = malloc(uxLengthRequired);

            if(pcRet == NULL)
            {
                ESP_LOGE(TAG, 
                "Failed to allocate memory for NVS to output string.");
            }
            else if(nvs_get_str(xNvsHandle, pcKey, pcRet, &uxLengthRequired) 
                != ESP_OK)
            {
                ESP_LOGE(TAG, "Could not output key: %s from namespace: %s on "
                "partition: %s.", pcKey, pcNamespace, pcPartitionName);
            }
        }
    }

    return pcRet;
}

BaseType_t xNvsSetStr(const char *pcPartitionName, const char *pcNamespace,
    const char *pcKey, const char *pcValue)
{
    nvs_handle_t xNvsHandle;

    BaseType_t xRet = pdFALSE;

    if(pcPartitionName == NULL)
    {
        ESP_LOGE(TAG, "Partition name is null.");

    }
    else if(pcNamespace == NULL)
    {
        ESP_LOGE(TAG, "Namespace is null.");
    }
    else if(pcKey == NULL)
    {
        ESP_LOGE(TAG, "Key is null.");
    }
    else if(pcValue == NULL)
    {
        ESP_LOGE(TAG, "Value is null.");
    }
    else if(nvs_flash_init_partition(pcPartitionName) != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not initialize partition: %s.", pcPartitionName);
    }
    else if(nvs_open_from_partition(pcPartitionName, pcNamespace, NVS_READWRITE,
        &xNvsHandle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not open namespace: %s on partition: %s for "
            "writing.", pcNamespace, pcPartitionName);
    }
    else if(nvs_set_str(xNvsHandle, pcKey, pcValue) != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not set key: %s in namespace: %s on "
            "partition: %s.", pcKey, pcNamespace, pcPartitionName);
    }
    else
    {
        xRet = pdTRUE;
    }

    return xRet;
}

static void vBookendedSerialSend(const char *pcBookend, const char *pcData)
{
    printf("\n%s_START\n%s\n%s_END\n", pcBookend, pcData, pcBookend);
    return;
}

static void vWifiEventHandler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "WIFI CONNECTED!");
        xEventGroupSetBits(xNetworkEventGroup, WIFI_CONNECTED_BIT);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "WIFI DISCONNECTED! Attempting to reconnect...");
        xEventGroupClearBits(xNetworkEventGroup, 
            WIFI_CONNECTED_BIT | IP_GOT_BIT);
        xEventGroupSetBits(xNetworkEventGroup, WIFI_DISCONNECTED_BIT);
        break;
    default:
        break;
    }

    return;
}

static void vIpEventHandler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    switch (event_id)
    {
    case IP_EVENT_STA_GOT_IP:
        xEventGroupSetBits(xNetworkEventGroup, IP_GOT_BIT);
        break;
    default:
        break;
    }

    return;
}

void vSelfClaimTask(void* pvParameters)
{
    (void)pvParameters;
    
    BaseType_t xSelfClaimSuccessful = pdFALSE;

    /* See if device already has device credentials from self-claiming */
    pcDevCert = pcNvsGetStr("tls_keys", "FMC", "certificate");
    pcDevKey = pcNvsGetStr("tls_keys", "FMC", "key");

    /* Wait for WiFi to be connected */
    xEventGroupWaitBits(xNetworkEventGroup, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        portMAX_DELAY);

    /* Perform self-claiming if the device does not have device credentials */
    if(pcDevCert == NULL || pcDevKey == NULL)
    {
        esp_rmaker_claim_data_t *pxSelfClaimData = 
            esp_rmaker_self_claim_init(pcThingName);

        if(pxSelfClaimData != NULL)
        {
            uint16_t usSelfClaimAttempts = 0;
            while(xSelfClaimSuccessful == pdFALSE &&
                usSelfClaimAttempts < MAX_SELF_CLAIM_ATTEMPTS)
            {
                vTaskDelay(10);
                /* Wait for the device to have an IP */
                xEventGroupWaitBits(xNetworkEventGroup,
                    IP_GOT_BIT,
                    pdFALSE,
                    pdTRUE,
                    portMAX_DELAY);
                    
                if(esp_rmaker_self_claim_perform(pxSelfClaimData) != ESP_OK)
                {
                    ++usSelfClaimAttempts;
                }
                else
                {
                    xSelfClaimSuccessful = pdTRUE;
                    pcDevCert = get_self_claim_certificate();
                    pcDevKey = get_self_claim_private_key();

                    if(xNvsSetStr("tls_keys", "FMC", "certificate", pcDevCert)
                        == pdFALSE)
                    {
                        ESP_LOGE(TAG, 
                            "Self-claiming certificate failed to store.");
                    }
                    
                    if(xNvsSetStr("tls_keys", "FMC", "key", pcDevKey)
                        == pdFALSE)
                    {
                        ESP_LOGE(TAG, 
                            "Self-claiming private key failed to store.");
                    }
                }
            }

        }

    }
    else
    {
        xSelfClaimSuccessful = pdTRUE;
    }

    if(xSelfClaimSuccessful == pdTRUE)
    {
        ESP_LOGI(TAG, "Self-claiming successful.");
        xEventGroupSetBits(xNetworkEventGroup, SELF_CLAIM_PERFORMED_BIT);

        /* Send device credentials out so they can be registered */
        vBookendedSerialSend(SERIAL_CERT_BOOKEND, pcDevCert);
        vBookendedSerialSend(SERIAL_THING_NAME_BOOKEND, pcThingName);

    }
    else
    {
        ESP_LOGI(TAG, "Self-claiming failed.");
        xEventGroupSetBits(xNetworkEventGroup, SELF_CLAIM_NOT_PERFORMED_BIT);
    }

    vTaskDelete(NULL);
}

void vTlsConnectionTask(void* pvParameters)
{
    BaseType_t xRet;
    (void)pvParameters;

    /* Wait for the device to perform Self-Claiming and have an IP */
    xEventGroupWaitBits(xNetworkEventGroup,
        SELF_CLAIM_PERFORMED_BIT | IP_GOT_BIT, pdFALSE, pdTRUE,portMAX_DELAY);

    /* If a connection was previously established, close it to free memory */
    if (xNetworkContext.pxTls != NULL)
    {
        ESP_LOGI(TAG, "TLS DISCONNECTED!");
        if(xTlsDisconnect(&xNetworkContext) != pdTRUE)
        {
            ESP_LOGE(TAG, "Something went wrong closing an existing TLS "
                "connection.");
        }
    }

    ESP_LOGI(TAG, "Establishing a TLS connection...");
    xRet = xTlsConnect(&xNetworkContext, pcEndpoint, port, server_cert_pem,
        pcDevCert, pcDevKey);

    if (xRet == pdTRUE)
    {
        ESP_LOGI(TAG, "TLS CONNECTED!");
        /* Flag that a TLS connection has been established */
        xEventGroupSetBits(xNetworkEventGroup, TLS_CONNECTED_BIT);
    }
    else
    {
        /* Flag that a TLS connection was not established */
        xEventGroupSetBits(xNetworkEventGroup, TLS_DISCONNECTED_BIT);
    }

    vTaskDelete(NULL);
}

void vMqttConnectionTask(void* pvParameters)
{
    MQTTStatus_t ret;
    (void)pvParameters;

    /* Wait for device to have a TLS connection */
    xEventGroupWaitBits(xNetworkEventGroup, TLS_CONNECTED_BIT, pdFALSE, pdTRUE,
        portMAX_DELAY);

    ESP_LOGI(TAG, "Establishing an MQTT connection...");

    ret = eMqttConnect(&xMQTTContext, pcThingName);

    if (ret == MQTTSuccess)
    {
        ESP_LOGI(TAG, "MQTT CONNECTED!");
        xEventGroupSetBits(xNetworkEventGroup, MQTT_CONNECTED_BIT);
    }
    else if (ret == MQTTNoMemory)
    {
        ESP_LOGE(TAG, "vMqttTask: xMQTTContext.networkBuffer is too small to "
            "send the connection packet.");
    }
    else if (ret == MQTTSendFailed || ret == MQTTRecvFailed)
    {
        ESP_LOGE(TAG, "vMqttTask: Send or Receive failed.");
        xEventGroupClearBits(xNetworkEventGroup, TLS_CONNECTED_BIT);
        xEventGroupSetBits(xNetworkEventGroup,
            TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT);
    }
    else
    {
        ESP_LOGE(TAG, "MQTT_Status: %s", MQTT_Status_strerror(ret));
        xEventGroupSetBits(xNetworkEventGroup, MQTT_DISCONNECTED_BIT);
    }

    vTaskDelete(NULL);
}

void vNetworkHandlingTask(void* pvParameters)
{
    /* Initialize Networking State */
    EventBits_t uxNetworkEventBits;
    xNetworkEventGroup = xEventGroupCreate();
    xEventGroupSetBits(xNetworkEventGroup,
        WIFI_DISCONNECTED_BIT | SELF_CLAIM_NOT_PERFORMED_BIT | 
        TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT);

    while (1)
    {
        uxNetworkEventBits = xEventGroupWaitBits(xNetworkEventGroup,
            WIFI_DISCONNECTED_BIT | SELF_CLAIM_NOT_PERFORMED_BIT | 
            TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT, pdTRUE, pdFALSE,
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
            xTaskCreate(vSelfClaimTask, "SelfClaimTask", 
                FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);
        }

        if ((uxNetworkEventBits & TLS_DISCONNECTED_BIT) != 0)
        {
            /* Establish a TLS connection */
            xTaskCreate(vTlsConnectionTask, "TlsConnectionTask", 
                FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);
        }

        if ((uxNetworkEventBits & MQTT_DISCONNECTED_BIT) != 0)
        {
            /* Establish an MQTT connection */
            xTaskCreate(vMqttConnectionTask, "MqttConnectionTask", 
                FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);
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

        ret = eMqttPublishFMConnect(&xMQTTContext, pcThingName, pcSendBuffer);

        if (ret != MQTTSuccess)
        {
            /* Flag that the TLS connection and MQTT connection were dropped */
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
    /* Initialize Non-Volatile Storage
     * Necessary for:
     * - WiFi drivers to store configs
     * - For getting user-provided WiFi Credentials (ssid, password)
     * - For getting endpoint that the demo connects to
     * - For getting/setting private key and device certificate */
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Extract WiFi SSID from NVS */
    pcWifiSsid = pcNvsGetStr("nvs", "FMC", "wifiSsid");
    if(pcWifiSsid == NULL)
    {
        ESP_LOGE(TAG, "Failed to retrieve WiFi SSID from NVS."
            "Ensure that the device has had credentials flashed.");
        return;
    }

    /* Extract WiFi password from NVS */
    pcWifiPass = pcNvsGetStr("nvs", "FMC", "wifiPass");
    if(pcWifiPass == NULL)
    {
        ESP_LOGE(TAG, "Failed to retrieve WiFi password from NVS. "
            "Ensure that the device has had credentials flashed.");
        return;
    }

    /* Extract endpoint from NVS */
    pcEndpoint = pcNvsGetStr("nvs", "FMC", "endpoint");
    if(pcEndpoint == NULL)
    {
        ESP_LOGE(TAG, "Failed to retrieve endpoint from NVS. "
            "Ensure that the device has had credentials flashed.");
        return;
    }

    /* Initialize Espressif's default event loop that will handle propagating 
     * events for Espressif's:
     * -WiFi
     * -TCP/IP stack */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Add event handlers to the default event loop */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID, &vWifiEventHandler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        ESP_EVENT_ANY_ID, &vIpEventHandler, NULL, NULL));

    /* Initialize networking. This initializes the TCP/IP stack, WiFi, and 
     * coreMQTT context */
    vNetworkingInit(&xNetworkContext, &xMQTTContext);

    /* Set wifi credentials to connect to the provisioned WiFi access point */
    if(xSetWifiCredentials(pcWifiSsid, pcWifiPass) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to set WiFi credentials.");
    }

    /* Generate thing name. Since this is generated using the MAC address
     * WiFi needs to be initialized first */
    pcThingName = pcGenerateThingName();

    if(pcThingName == NULL)
    {
        ESP_LOGE(TAG, "Failed to generate thing name.");
        return;
    }

    /* Handles setting up and maintaining the network connection */
    xTaskCreate(vNetworkHandlingTask, "NetworkEventHandlingTask", 2048, NULL, 2,
        NULL);

    /* Handles getting and sending sensor data */
    xTaskCreate(vSensorSendingTask, "SensorSendingTask", 
        FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);

    return;
}