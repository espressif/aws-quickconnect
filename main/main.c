/*
 * FreeRTOS Quick Connect for ESP32-C3 v1.0.0
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file main.c
 * @brief Main file for the Quick Connect demo for the ESP32-C3.
 */

/* Standard includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ESP-IDF includes */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_random.h"

/* FreeRTOS includes */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* coreMQTT library include */
#include "core_mqtt.h"

/* Networking code include */
#include "networking.h"

/* Temperature sensor driver */
#include "driver/temp_sensor.h"

/* Self-claiming  */
#include "esp_rmaker_claim.h"

/* Definitions ****************************************************************/

/* JSON sending task */
#define SENDING_INTERVAL_MS                  ( 1000U )

/* Buffer sizes  */
#define THING_NAME_SIZE                      ( 60U )
#define SEND_BUFFER_SIZE                     ( 1024U )
#define ETH_MAC_BUFFER_SIZE                  ( 6U )

/* Task configs */
#define FMC_TASK_DEFAULT_STACK_SIZE          ( 3072U )

/* Serial outputs for the utility */
#define UTIL_SERIAL_WIFI_CONNECTED           "DEVICE_WIFI_CONNECTED"
#define UTIL_SERIAL_WIFI_DISCONNECTED        "DEVICE_WIFI_DISCONNECTED"
#define UTIL_SERIAL_PRIV_KEY_AND_CSR_GEN     "DEVICE_PRIV_KEY_AND_CSR_GEN"
#define UTIL_SERIAL_PRIV_KEY_AND_CSR_FAIL    "DEVICE_PRIV_KEY_AND_CSR_FAIL"
#define UTIL_SERIAL_PRIV_KEY_AND_CSR_SUCCESS "DEVICE_PRIV_KEY_AND_CSR_SUCCESS"
#define UTIL_SERIAL_SELF_CLAIM_PERF          "DEVICE_SELF_CLAIM_PERF"
#define UTIL_SERIAL_SELF_CLAIM_FAIL          "DEVICE_SELF_CLAIM_FAIL"
#define UTIL_SERIAL_SELF_CLAIM_SUCCESS       "DEVICE_SELF_CLAIM_SUCCESS"
#define UTIL_SERIAL_CERT_BOOKEND             "DEVICE_CERT"
#define UTIL_SERIAL_THING_NAME_BOOKEND       "DEVICE_THING_NAME"

/* Utility ouput event group bit definitions */
#define UTIL_WIFI_CONNECTED_BIT              (1 << 0)
#define UTIL_WIFI_DISCONNECTED_BIT           (1 << 1)
#define UTIL_PRIV_KEY_AND_CSR_GEN_BIT        (1 << 2)
#define UTIL_PRIV_KEY_AND_CSR_FAIL_BIT       (1 << 3)
#define UTIL_PRIV_KEY_AND_CSR_SUCCESS_BIT    (1 << 4)
#define UTIL_SELF_CLAIM_CERT_GET_BIT         (1 << 5)
#define UTIL_SELF_CLAIM_CERT_FAIL_BIT        (1 << 6)
#define UTIL_SELF_CLAIM_CERT_SUCCESS_BIT     (1 << 7)

/* Network event group bit definitions */
#define INIT_BIT                             (1 << 0)
#define WIFI_CONNECTED_BIT                   (1 << 1)
#define WIFI_DISCONNECTED_BIT                (1 << 2)
#define IP_GOT_BIT                           (1 << 3)
#define PRIV_KEY_FAIL_BIT                    (1 << 5)
#define PRIV_KEY_SUCCESS_BIT                 (1 << 6)
#define CERT_FAIL_BIT                        (1 << 8)
#define CERT_SUCCESS_BIT                     (1 << 9)
#define TLS_CONNECTED_BIT                    (1 << 11)
#define TLS_DISCONNECTED_BIT                 (1 << 12)
#define MQTT_CONNECTED_BIT                   (1 << 14)
#define MQTT_DISCONNECTED_BIT                (1 << 15)

/* Non-volatile storage definitions for provisioned data */
#define UTIL_PROV_PARTITION                  "nvs"
#define UTIL_PROV_NAMESPACE                  "quickConnect"
#define UTIL_PROV_WIFI_PASS_KEY              "wifiPass"
#define UTIL_PROV_WIFI_SSID_KEY              "wifiSsid"
#define UTIL_PROV_ENDPOINT_KEY               "endpoint"

/* Non-volatile storage defintions for non-provisioned data */
#define RUNTIME_SAVE_PARTITION               "runtime"
#define RUNTIME_SAVE_NAMESPACE               "quickConnect"
#define RUNTIME_SAVE_CERT_KEY                "certificate"
#define RUNTIME_SAVE_PRIV_KEY_KEY            "key"
#define RUNTIME_SAVE_THINGNAME_KEY           "thingname"
#define RUNTIME_SAVE_NODE_ID_KEY             "nodeid"

/* Globals ********************************************************************/

/* Logging tag */
static const char* TAG = "QuickConnectMain";

/* Device connection configurations  */
static char *pcNodeId = NULL;
static char *pcThingName =  NULL;
static char *pcWifiSsid = NULL;
static char *pcWifiPass = NULL;
static char *pcEndpoint = NULL;
static char *pcDevCert = NULL;
static char *pcDevKey = NULL;
static int xPort = 8883;

/* ESP-IDF imported file main/server_cert/root_ca.crt. Changing this file 
 * changes the root CA used for this demo. This is the AWS Root CA if left 
 * unchanged. */
extern const char pcRootCA[] asm("_binary_root_ca_crt_start");

/* Utility output */
static EventGroupHandle_t xUtilityOutputEventGroup;

/* Networking */
static EventGroupHandle_t xNetworkEventGroup;
static MQTTContext_t xMQTTContext = { 0 };
static NetworkContext_t xNetworkContext = { 0 };
static esp_rmaker_claim_data_t *pxSelfClaimData;

/* Non-volatile storage access functions **************************************/

/**
 * @brief Function to get a string stored in non-volatile storage.
 *
 * @param[in] pcPartitionName Name of the partition to get the string from.
 * @param[in] pcNamespace Name of the namespace on the partition to get the 
 * string from.
 * @param[in] pcKey Key name associated with stored string.
 * 
 * @return NULL on failure; Null-terminated string on success.
 */
static char *prvNvsGetStr(const char *pcPartitionName, const char *pcNamespace, 
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

/**
 * @brief Function to store a string in non-volatile storage.
 *
 * @param[in] pcPartitionName Name of the partition to write string to.
 * @param[in] pcNamespace Name of the namespace on the partition to write string
 * to.
 * @param[in] pcKey Key name to be associated with stored string.
 * 
 * @return pdFALSE on failure; pdTRUE on success.
 */
static BaseType_t prvNvsSetStr(const char *pcPartitionName, 
    const char *pcNamespace, const char *pcKey, const char *pcValue)
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

/* Networking Functions *******************************************************/

/**
 * @brief Function to assign the thingname and node ID for the device.
 * This function acquires the thingname and node ID from non-volatile storage, 
 * if they have been stored. Otherwise, generates a node ID from the device's 
 * WiFi MAC address and thingname by appending a random number, and stores them
 * in non-volatile storage. The function then sets the globals pcNodeId and 
 * pcThingName. If this function needs to generate either, then WiFi must be 
 * initialized first before calling this function.
 * 
 * @return pdFALSE on failure; pdTRUE on success.
 */
static BaseType_t prvAssignThingNameAndNodeId(void)
{
    uint8_t pucEthMac[ETH_MAC_BUFFER_SIZE];

    BaseType_t xRet = pdFALSE;

    /* Check if thingname and nodeID is in NVS storage. */
    pcThingName = prvNvsGetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
        RUNTIME_SAVE_THINGNAME_KEY);
    pcNodeId = prvNvsGetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
        RUNTIME_SAVE_NODE_ID_KEY);

    /* If thingname or nodeID is not in NVS, then generate and store. */
    if(pcThingName == NULL || pcNodeId == NULL)
    {
        /* Generate thing name from the device's MAC address, this requires that
         * WiFi was initialized. */
        if (esp_wifi_get_mac(WIFI_IF_STA, pucEthMac) != ESP_OK) {
            ESP_LOGE(TAG, "Could not fetch MAC address. "
                "Initialise Wi-Fi first");
        }
        else
        {
            pcNodeId = calloc(1, THING_NAME_SIZE);
            pcThingName = calloc(1, THING_NAME_SIZE);

            if(pcNodeId == NULL || pcThingName == NULL)
            {
                ESP_LOGE(TAG, 
                "Failed to allocate memory for thingname or nodeID.");
            }
            else
            {
                /* This creates a nodeID for self-claiming service - DO NOT
                 * CHANGE or self-claiming will not work */
                snprintf(pcNodeId, THING_NAME_SIZE,
                    "%02X%02X%02X%02X%02X%02X", pucEthMac[0], pucEthMac[1], 
                    pucEthMac[2], pucEthMac[3], pucEthMac[4], pucEthMac[5]);

                /* This creates a thing name from the nodeID and a random number
                 * to prevent thingname collision. */
                snprintf(pcThingName, THING_NAME_SIZE, 
                    "%s%ld", pcNodeId, esp_random());
                
                /* Store nodeID into NVS for the next time the device is
                 * rebooted. */
                if(prvNvsSetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
                    RUNTIME_SAVE_NODE_ID_KEY, pcNodeId) == pdFALSE)
                {
                    ESP_LOGE(TAG, 
                        "Failed to store nodeID.");
                }
                /* Store thingname into NVS for the next time the device is
                 * rebooted. */
                else if(prvNvsSetStr(RUNTIME_SAVE_PARTITION, 
                    RUNTIME_SAVE_NAMESPACE, RUNTIME_SAVE_THINGNAME_KEY, 
                    pcThingName) == pdFALSE)
                {
                    ESP_LOGE(TAG, 
                        "Failed to store thingname.");
                }
                else
                {
                    xRet = pdTRUE;
                }
            }
        }
    }
    else
    {
        xRet = pdTRUE;
    }

    return xRet;
}

/**
 * @brief Function to be used with Espressif's event handling library to handle
 * WiFi events. This propagates WiFi events to the network event group
 * and to the utility output event group, so they can be used to coordinate
 * tasks of the demo.
 * 
 * @param[in] pvParameters Arguments passed to the event handler. Not used.
 * @param[in] xEventBase Event base of the event being passed to the function.
 * Not used as every event passed to this will have the event base of 
 * WIFI_EVENT.
 * @param[in] lEventId ID of the event associated with the event base.
 * @param[in] pvEventData Data associated with the event. Not used.
 */
static void prvWifiEventHandler(void* pvParameters, esp_event_base_t xEventBase,
    int32_t lEventId, void* pvEventData)
{
    (void)pvParameters;
    (void)xEventBase;
    (void)pvEventData;

    switch (lEventId)
    {
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "WIFI CONNECTED!");
        /* If WiFi is connected, notify networking tasks and utility output
         * task. */
        xEventGroupSetBits(xNetworkEventGroup, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(xUtilityOutputEventGroup, UTIL_WIFI_CONNECTED_BIT);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "WIFI DISCONNECTED! Attempting to reconnect...");
        /* If WiFi is disconnected, notify networking tasks and utility output
         * task. */
        xEventGroupClearBits(xNetworkEventGroup, 
            WIFI_CONNECTED_BIT | IP_GOT_BIT);
        xEventGroupSetBits(xNetworkEventGroup, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(xUtilityOutputEventGroup, 
            UTIL_WIFI_DISCONNECTED_BIT);
        break;
    default:
        break;
    }

    return;
}

/**
 * @brief Function to be used with Espressif's event handling library to handle
 * IP events. This propagates IP events to the network event group, so they can
 * be used to coordinate tasks of the demo.
 * 
 * @param[in] pvParameters Arguments passed to the event handler. Not used.
 * @param[in] xEventBase Event base of the event being passed to the function.
 * Not used as every event passed to this will have the event base of IP_EVENT.
 * @param[in] lEventId ID of the event associated with the event base.
 * @param[in] pvEventData Data associated with the event. Not used.
 */
static void prvIpEventHandler(void* pvParameters, esp_event_base_t xEventBase,
    int32_t lEventId, void* pvEventData)
{
    (void)pvParameters;
    (void)xEventBase;
    (void)pvEventData;

    switch (lEventId)
    {
    case IP_EVENT_STA_GOT_IP:
        /* If an IP is received, notify networking tasks. */
        xEventGroupSetBits(xNetworkEventGroup, IP_GOT_BIT);
        break;
    default:
        break;
    }

    return;
}

/**
 * @brief FreeRTOS task function used to acquire and assign the private key for
 * the demo. This function acquires the private key from non-volatile storage, 
 * if a private key has been stored. Otherwise, generates a private key and 
 * stores it. Assigns the private key to the global variable pcDevKey to be used
 * by other networking functions.
 * 
 * @param[in] pvParameters Parameters passed when the task is created. Not used.
 */
static void prvGetPrivKeyTask(void *pvParamaters)
{
    (void)pvParamaters;

    BaseType_t xPrivKeyAcquired = pdFALSE;

    /* Notify utility that the device is generating private key and CSR. */
    xEventGroupSetBits(xUtilityOutputEventGroup, UTIL_PRIV_KEY_AND_CSR_GEN_BIT);

    /* Check if key is in NVS already. */
    pcDevKey = prvNvsGetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
        RUNTIME_SAVE_PRIV_KEY_KEY);

    /* Check if device certificate is in storage. */
    pcDevCert = prvNvsGetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
        RUNTIME_SAVE_CERT_KEY);

    /* If key or certificate isn't in NVS already, then generate key and CSR and
     * store. */
    if(pcDevKey == NULL || pcDevCert == NULL)
    {
        /* Generate private key and code-siging request. Code-signing request is
         * stored inside of pxSelfClaimData and the private key is acquired with
         * a call to get_self_claim_private_key after this function is called. 
         */
        pxSelfClaimData = esp_rmaker_self_claim_init(pcNodeId);
        if(pxSelfClaimData != NULL)
        {
            pcDevKey = get_self_claim_private_key();

            /* Store private key into NVS for the next time the device is
             * rebooted. */
            if(prvNvsSetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
                RUNTIME_SAVE_PRIV_KEY_KEY, pcDevKey) == pdFALSE)
            {
                ESP_LOGE(TAG, 
                    "Self-claiming private key failed to store.");
            }
            else
            {
                xPrivKeyAcquired = pdTRUE;
            }
        }
    }
    else
    {
        xPrivKeyAcquired = pdTRUE;
    }

    if(xPrivKeyAcquired == pdTRUE)
    {
        ESP_LOGI(TAG, "Private key acquired.");
        /* Notify networking tasks and utility output task that the private
         * key and CSR were generated and successfully acquired. */
        xEventGroupSetBits(xNetworkEventGroup, PRIV_KEY_SUCCESS_BIT);
        xEventGroupSetBits(xUtilityOutputEventGroup, 
            UTIL_PRIV_KEY_AND_CSR_SUCCESS_BIT);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to acquire private key.");
        /* Notify networking tasks and utility output task that the private key
         * could not be acquired. */
        xEventGroupSetBits(xNetworkEventGroup, PRIV_KEY_FAIL_BIT);
        xEventGroupSetBits(xUtilityOutputEventGroup, 
            UTIL_PRIV_KEY_AND_CSR_FAIL_BIT);
    }

    vTaskDelete(NULL);
}

/**
 * @brief FreeRTOS task function used to acquire and assign the device
 * certificate for the demo. This function acquires the certificate from
 * non-volatile storage, if a certificate has been stored. Otherwise, using the
 * code-signing request generated by prvGetKeyTask in pxSelfClaimData, this 
 * function makes an HTTP request to Espressif's self-claiming API for RainMaker
 * in order to acquire a certificate signed by Espressif's RainMaker CA, and
 * then stores it for device reboots. Assigns the certificate to the global 
 * variable pcDevCert to be used by other networking functions.
 * 
 * @param[in] pvParameters Parameters passed when the task is created. Not used.
 */
static void prvGetCertTask(void *pvParameters)
{
    (void)pvParameters;

    BaseType_t xCertAcquired = pdFALSE;

    xEventGroupSetBits(xUtilityOutputEventGroup, UTIL_SELF_CLAIM_CERT_GET_BIT);

    /* Check if certificate is already in storage. */
    pcDevCert = prvNvsGetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
        RUNTIME_SAVE_CERT_KEY);

    /* If certificate isn't in storage then perform self-claiming and store. */
    if(pcDevCert == NULL)
    {
        /* Wait for the device to have a private key and an IP. */
        xEventGroupWaitBits(xNetworkEventGroup, 
            PRIV_KEY_SUCCESS_BIT | IP_GOT_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
            
        if(esp_rmaker_self_claim_perform(pxSelfClaimData) == ESP_OK)
        {
            pcDevCert = get_self_claim_certificate();
            /* Store certificate into NVS for the next time the device is
             * rebooted. */
            if(prvNvsSetStr(RUNTIME_SAVE_PARTITION, RUNTIME_SAVE_NAMESPACE, 
                RUNTIME_SAVE_CERT_KEY, pcDevCert) == pdFALSE)
            {
                ESP_LOGE(TAG, 
                    "Self-claiming certificate failed to store.");
            }
            else
            {
                xCertAcquired = pdTRUE;
            }
        }
    }
    else
    {
        xCertAcquired = pdTRUE;
    }

    if(xCertAcquired == pdTRUE)
    {
        ESP_LOGI(TAG, "Self-Claiming certificate acquired.");
        /* Notify networking tasks and utility output task that the device
         * certificate was successfully acquired. */
        xEventGroupSetBits(xNetworkEventGroup, CERT_SUCCESS_BIT);
        xEventGroupSetBits(xUtilityOutputEventGroup, 
            UTIL_SELF_CLAIM_CERT_SUCCESS_BIT);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to acquire self-claiming certificate.");
        /* Notify networking tasks and utility output task that the device 
         * failed to acquire a certificate. */
        xEventGroupSetBits(xNetworkEventGroup, CERT_FAIL_BIT);
        xEventGroupSetBits(xUtilityOutputEventGroup, 
            UTIL_SELF_CLAIM_CERT_FAIL_BIT);
    }

    vTaskDelete(NULL);
}

/**
 * @brief FreeRTOS task function used to set up the TLS connection for the demo.
 * 
 * @param[in] pvParameters Parameters passed when the task is created. Not used.
 */
static void prvTlsConnectionTask(void* pvParameters)
{
    (void)pvParameters;

    BaseType_t xRet;

    /* Wait for the device to have device credentials and an IP. */
    xEventGroupWaitBits(xNetworkEventGroup,
        PRIV_KEY_SUCCESS_BIT | CERT_SUCCESS_BIT | IP_GOT_BIT, pdFALSE, pdTRUE, 
        portMAX_DELAY);

    /* If a connection was previously established, close it to free memory. */
    if (xNetworkContext.pxTls != NULL)
    {
        ESP_LOGI(TAG, "TLS DISCONNECTED!");
        if(xTlsDisconnect(&xNetworkContext) != pdTRUE)
        {
            ESP_LOGE(TAG, "Something went wrong closing an existing TLS "
                "connection.");
        }
    }

    xRet = xTlsConnect(&xNetworkContext, pcEndpoint, xPort, pcRootCA, 
        pcDevCert, pcDevKey);

    if (xRet == pdTRUE)
    {
        ESP_LOGI(TAG, "TLS CONNECTED!");
        /* Flag that a TLS connection has been established. */
        xEventGroupSetBits(xNetworkEventGroup, TLS_CONNECTED_BIT);
    }
    else
    {
        /* Flag that a TLS connection was not established. */
        xEventGroupSetBits(xNetworkEventGroup, TLS_DISCONNECTED_BIT);
    }

    vTaskDelete(NULL);
}

/**
 * @brief FreeRTOS task function used to set up an MQTT connection over the TLS
 * connection.
 * 
 * @param[in] pvParameters Parameters passed when the task is created. Not used.
 */
static void prvMqttConnectionTask(void* pvParameters)
{
    (void)pvParameters;

    MQTTStatus_t eRet;

    /* Wait for device to have a TLS connection */
    xEventGroupWaitBits(xNetworkEventGroup, TLS_CONNECTED_BIT, pdFALSE, pdTRUE,
        portMAX_DELAY);

    ESP_LOGI(TAG, "Establishing an MQTT connection...");

    eRet = eMqttConnect(&xMQTTContext, pcThingName);

    if (eRet == MQTTSuccess)
    {
        ESP_LOGI(TAG, "MQTT CONNECTED!");
        xEventGroupSetBits(xNetworkEventGroup, MQTT_CONNECTED_BIT);
    }
    else if (eRet == MQTTNoMemory)
    {
        ESP_LOGE(TAG, "xMQTTContext.networkBuffer is too small to send the "
        "connection packet.");
    }
    else if (eRet == MQTTSendFailed || eRet == MQTTRecvFailed)
    {
        ESP_LOGE(TAG, "MQTT send or receive failed.");
        xEventGroupClearBits(xNetworkEventGroup, TLS_CONNECTED_BIT);
        xEventGroupSetBits(xNetworkEventGroup,
            TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT);
    }
    else
    {
        ESP_LOGE(TAG, "MQTT_Status: %s", MQTT_Status_strerror(eRet));
        xEventGroupSetBits(xNetworkEventGroup, MQTT_DISCONNECTED_BIT);
    }

    vTaskDelete(NULL);
}

/**
 * @brief FreeRTOS task function used to start tasks that set-up the device's
 * networking to connect to an endpoint and publish MQTT messages.
 * 
 * @param[in] pvParameters Parameters passed when the task is created. Not used.
 */
static void prvNetworkHandlingTask(void* pvParameters)
{
    (void)pvParameters;

    EventBits_t uxNetworkEventBits;
    
    /* Initialize networking state. */
    xEventGroupSetBits(xNetworkEventGroup, INIT_BIT);

    while (1)
    {
        /* Wait for initialization state or for any network task to fail.
         * If a network task fails, this restarts it. */
        uxNetworkEventBits = xEventGroupWaitBits(xNetworkEventGroup, INIT_BIT |
            WIFI_DISCONNECTED_BIT | PRIV_KEY_FAIL_BIT | CERT_FAIL_BIT 
            | TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT, 
            pdTRUE, pdFALSE, portMAX_DELAY);

        if ((uxNetworkEventBits & (INIT_BIT | WIFI_DISCONNECTED_BIT)) != 0)
        {
            /* Establish a WiFi connection. */
            ESP_LOGI(TAG, "Connecting to WiFi...");
            ESP_ERROR_CHECK(esp_wifi_connect());
        }

        if ((uxNetworkEventBits & (INIT_BIT | PRIV_KEY_FAIL_BIT)) != 0)
        {
            /* Get and set private key. */
            xTaskCreate(prvGetPrivKeyTask, "GetPrivKeyTask", 
                FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);
        }

        if ((uxNetworkEventBits & (INIT_BIT | CERT_FAIL_BIT)) != 0)
        {
            /* Get and set device certificate. */
            xTaskCreate(prvGetCertTask, "GetCertTask", 
                FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);
        }

        if ((uxNetworkEventBits & (INIT_BIT | TLS_DISCONNECTED_BIT)) != 0)
        {
            /* Establish a TLS connection. */
            xTaskCreate(prvTlsConnectionTask, "TlsConnectionTask", 
                FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);
        }

        if ((uxNetworkEventBits & (INIT_BIT | MQTT_DISCONNECTED_BIT)) != 0)
        {
            /* Establish an MQTT connection. */
            xTaskCreate(prvMqttConnectionTask, "MqttConnectionTask", 
                FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);
        }
    }

    vTaskDelete(NULL);
}

/**
 * @brief FreeRTOS task function used to initialize the temperature
 * sensor of the ESP32-C3, poll from it, and send a JSON packet containing
 * sensor data to be parsed and used by the visualizer website.
 * 
 * @param[in] pvParameters Parameters passed when the task is created. Not used.
 */
static void prvQuickConnectSendingTask(void* pvParameters)
{
    (void)pvParameters;
    
    MQTTStatus_t eRet;

    char pcSendBuffer[SEND_BUFFER_SIZE];

    float xTsensOut;

    /* Initialize temperature sensor. */
    temp_sensor_config_t xTsensConfig = TSENS_CONFIG_DEFAULT();
    temp_sensor_get_config(&xTsensConfig);
    xTsensConfig.dac_offset = TSENS_DAC_DEFAULT;
    temp_sensor_set_config(xTsensConfig);
    temp_sensor_start();

    while (1) 
    {
        /* Suspends the task for SENDING_INTERVAL_MS milliseconds. */
        vTaskDelay(SENDING_INTERVAL_MS / portTICK_RATE_MS);

        /* Wait for device to be connected to MQTT. */
        xEventGroupWaitBits(xNetworkEventGroup, MQTT_CONNECTED_BIT, pdFALSE,
            pdTRUE, portMAX_DELAY);

        temp_sensor_read_celsius(&xTsensOut);

/* ADD GRAPHS HERE ************************************************************/
#define CUSTOM_GRAPH_ENABLED 0
#if !CUSTOM_GRAPH_ENABLED
        snprintf(pcSendBuffer, SEND_BUFFER_SIZE, 
            "["
                "{"
                    "\"label\" : \"Temperature\","
                    "\"display_type\" : \"line_graph\","
                    "\"values\" :"
                    "["
                        "{"
                            "\"unit\" : \"Celsius\","
                            "\"value\" : %f,"
                            "\"label\" : \"\""
                        "}"
                    "]"
                "}"
            "]", xTsensOut);
#else
        snprintf(pcSendBuffer, SEND_BUFFER_SIZE, 
            "["
                "{"
                    "\"label\" : \"Temperature\","
                    "\"display_type\" : \"line_graph\","
                    "\"values\" :"
                    "["
                        "{"
                            "\"unit\" : \"Celsius\","
                            "\"value\" : %f,"
                            "\"label\" : \"\""
                        "}"
                    "]"
                "},"
                "{"
                    "\"label\" : \"Random\","
                    "\"display_type\" : \"line_graph\","
                    "\"values\" :"
                    "["
                        "{"
                            "\"unit\" : \"Number\","
                            "\"value\" : %d,"
                            "\"label\" : \"\""
                        "}"
                    "]"
                "}"
            "]", xTsensOut, rand() % 4000);
#endif

/******************************************************************************/

        /* Send JSON over MQTT connection. */
        eRet = eMqttPublishQuickConnect(&xMQTTContext, pcThingName, 
            pcSendBuffer);

        /* If it was not a success, then the connection was dropped. */
        if (eRet != MQTTSuccess)
        {
            /* Flag that the TLS connection and MQTT connection were dropped. */
            xEventGroupClearBits(xNetworkEventGroup,
                TLS_CONNECTED_BIT | MQTT_CONNECTED_BIT);
            xEventGroupSetBits(xNetworkEventGroup,
                TLS_DISCONNECTED_BIT | MQTT_DISCONNECTED_BIT);
        }
    }
    vTaskDelete(NULL);
}

/* Utility communication functions ********************************************/

/**
 * @brief Function that sends bookended data out to UART.
 * 
 * @param[in] pcBookend String that will bookend pcData being sent out of UART.
 * @param[in] pcData String that contains data being sent out of UART.
 */
static void prvUtilSerialSendData(const char *pcBookend, const char *pcData)
{
    /* ESP32 devices output over UART with printf. */
    printf("\n%s_START\n%s\n%s_END\n", pcBookend, pcData, pcBookend);
    return;
}

/**
 * @brief Function that sends a notification string out to UART.
 * 
 * @param[in] pcNotification String representing a notification.
 */
static void prvUtilSerialSendNotify(const char *pcNotification)
{
    /* ESP32 devices output over UART with printf. */
    printf("%s\n", pcNotification);
    return;
}

/**
 * @brief FreeRTOS task function that handles sending device status and data 
 * out to the QuickConnect utility program
 * 
 * @param[in] pvParameters Parameters passed when the task is created. Not used.
 */
static void prvUtilityOutputTask(void* pvParameters)
{
    (void)pvParameters;

    EventBits_t uxUtilityOutputEventBits;

    /* WiFi may not connect immediately if the connection is bad, so this
     * retries until it does connect. The utility handles notifying the user
     * that WiFi hasn't connected after a certain number of attempts. */
    while(1)
    {
        uxUtilityOutputEventBits = xEventGroupWaitBits(xUtilityOutputEventGroup, 
            UTIL_WIFI_CONNECTED_BIT | UTIL_WIFI_DISCONNECTED_BIT, pdTRUE, 
            pdFALSE, portMAX_DELAY);
        
        if((uxUtilityOutputEventBits & UTIL_WIFI_DISCONNECTED_BIT) != 0)
        {
            prvUtilSerialSendNotify(UTIL_SERIAL_WIFI_DISCONNECTED);
        }
        else
        {
            prvUtilSerialSendNotify(UTIL_SERIAL_WIFI_CONNECTED);
            break;
        }
    }

    /* Notify the utility that the device is in the process of generating a
     * Private Key and CSR to perform self-claiming of a device certificate. */
    uxUtilityOutputEventBits = xEventGroupWaitBits(xUtilityOutputEventGroup,
        UTIL_PRIV_KEY_AND_CSR_GEN_BIT, pdTRUE, pdTRUE, portMAX_DELAY);

    prvUtilSerialSendNotify(UTIL_SERIAL_PRIV_KEY_AND_CSR_GEN);

    /* Notify the utility that the device has either succeeded or failed in
     * generating a Private Key and CSR. */
    uxUtilityOutputEventBits = xEventGroupWaitBits(xUtilityOutputEventGroup,
        UTIL_PRIV_KEY_AND_CSR_SUCCESS_BIT | UTIL_PRIV_KEY_AND_CSR_FAIL_BIT, 
        pdTRUE, pdFALSE, portMAX_DELAY);

    if((uxUtilityOutputEventBits & UTIL_PRIV_KEY_AND_CSR_FAIL_BIT) != 0)
    {
        prvUtilSerialSendNotify(UTIL_SERIAL_PRIV_KEY_AND_CSR_FAIL);
    }
    else
    {
        prvUtilSerialSendNotify(UTIL_SERIAL_PRIV_KEY_AND_CSR_SUCCESS);
    }

    /* Notify the utility that the device is in the process of self-claiming
     * to get a certificate. */
    uxUtilityOutputEventBits = xEventGroupWaitBits(xUtilityOutputEventGroup,
        UTIL_SELF_CLAIM_CERT_GET_BIT, pdTRUE, pdTRUE, portMAX_DELAY);

    prvUtilSerialSendNotify(UTIL_SERIAL_SELF_CLAIM_PERF);

    /* Notify the utility that the device has either succeeded or failed in
     * self-claiming a certificate. */
    uxUtilityOutputEventBits = xEventGroupWaitBits(xUtilityOutputEventGroup,
        UTIL_SELF_CLAIM_CERT_SUCCESS_BIT | UTIL_SELF_CLAIM_CERT_FAIL_BIT, 
        pdTRUE, pdFALSE, portMAX_DELAY);

    if((uxUtilityOutputEventBits & UTIL_SELF_CLAIM_CERT_FAIL_BIT) != 0)
    {
        prvUtilSerialSendNotify(UTIL_SERIAL_SELF_CLAIM_FAIL);
    }
    else
    {
        prvUtilSerialSendNotify(UTIL_SERIAL_SELF_CLAIM_SUCCESS);
        /* Send device certificate and thing name out to the utility. */
        prvUtilSerialSendData(UTIL_SERIAL_CERT_BOOKEND, pcDevCert);
        prvUtilSerialSendData(UTIL_SERIAL_THING_NAME_BOOKEND, pcThingName);
    }

    vTaskDelete(NULL);
}

/* Main ***********************************************************************/

/**
 * @brief FreeRTOS task function that serves as the main entry point for the 
 * program. When this returns, other tasks will remain running. This mainly
 * handles intitializing the demo.
 */
void app_main(void)
{
    /* Initialize Non-Volatile Storage
     * Necessary for:
     * - WiFi drivers to store configs
     * - For getting user-provided WiFi Credentials (ssid, password)
     * - For getting endpoint that the demo connects to
     * - For getting/setting private key and device certificate */
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Extract WiFi SSID from NVS. */
    pcWifiSsid = prvNvsGetStr(UTIL_PROV_PARTITION, UTIL_PROV_NAMESPACE, 
        UTIL_PROV_WIFI_SSID_KEY);
    if(pcWifiSsid == NULL)
    {
        ESP_LOGE(TAG, "Failed to retrieve WiFi SSID from NVS. "
            "Ensure that the device has had configurations flashed.");
        return;
    }

    /* Extract WiFi password from NVS. */
    pcWifiPass = prvNvsGetStr(UTIL_PROV_PARTITION, UTIL_PROV_NAMESPACE, 
        UTIL_PROV_WIFI_PASS_KEY);
    if(pcWifiPass == NULL)
    {
        ESP_LOGE(TAG, "Failed to retrieve WiFi password from NVS. "
            "Ensure that the device has had configurations flashed.");
        return;
    }

    /* Extract endpoint from NVS. */
    pcEndpoint = prvNvsGetStr(UTIL_PROV_PARTITION, UTIL_PROV_NAMESPACE, 
        UTIL_PROV_ENDPOINT_KEY);
    if(pcEndpoint == NULL)
    {
        ESP_LOGE(TAG, "Failed to retrieve endpoint from NVS. "
            "Ensure that the device has had configurations flashed.");
        return;
    }

    /* Initialize Espressif's default event loop that will handle propagating 
     * events for Espressif's:
     * -WiFi
     * -TCP/IP stack */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Add event handlers to the default event loop */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID, &prvWifiEventHandler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        ESP_EVENT_ANY_ID, &prvIpEventHandler, NULL, NULL));

    /* Initialize networking. This initializes the TCP/IP stack, WiFi, and 
     * coreMQTT context. */
    vNetworkingInit(&xNetworkContext, &xMQTTContext);
    
    /* Set wifi credentials to connect to the provisioned WiFi access point. */
    if(xSetWifiCredentials(pcWifiSsid, pcWifiPass) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to set WiFi credentials.");
        return;
    }

    /* Set thingname. Since thingname is generated using the MAC address
     * WiFi needs to be initialized first. */
    if(prvAssignThingNameAndNodeId() == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to assign thingname and nodeID.");
        return;
    }

    /* Initialize Event Groups for the demo. */
    xNetworkEventGroup = xEventGroupCreate();
    xUtilityOutputEventGroup = xEventGroupCreate();

    /* Handles outputting device state to the utility. */
    xTaskCreate(prvUtilityOutputTask, "UtilityOutputTask", 
        FMC_TASK_DEFAULT_STACK_SIZE, NULL, 2, NULL);

    /* Handles setting up and maintaining the network connection. */
    xTaskCreate(prvNetworkHandlingTask, "NetworkEventHandlingTask", 
        FMC_TASK_DEFAULT_STACK_SIZE, NULL, 2, NULL);

    /* Handles getting and sending sensor data. */
    xTaskCreate(prvQuickConnectSendingTask, "QuickConnectGraphSendingTask", 
        FMC_TASK_DEFAULT_STACK_SIZE, NULL, 1, NULL);

    return;
}