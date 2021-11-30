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

/* Standard includes */
#include <string.h>

/* FreeRTOS includes */
#include "freertos/FreeRTOS.h"

/* ESP-IDF includes */
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_event.h"

/* coreMQTT library include */
#include "core_mqtt.h"

#include "networking.h"

/* Definitions ****************************************************************/

/* Timing definitions */
#define MILLISECONDS_PER_SECOND      ( 1000U )
#define MILLISECONDS_PER_TICK        ( MILLISECONDS_PER_SECOND / \
    configTICK_RATE_HZ )

/* Buffer sizes */
#define MQTT_SHARED_BUFFER_SIZE      ( 10000U )
#define WIFI_CONFIG_SSID_BUFFER_SIZE ( 32U )
#define WIFI_CONFIG_PASS_BUFFER_SIZE ( 64U )

/* Globals ********************************************************************/

/* Logging tag */
static const char* TAG = "QuickConnectNetworking";

/* MQTT */
static uint32_t ulGlobalEntryTimeMs;
static uint16_t usPublishPacketIdentifier;
static uint8_t ucSharedBuffer[MQTT_SHARED_BUFFER_SIZE];
static MQTTFixedBuffer_t xBuffer =
{
    ucSharedBuffer,
    MQTT_SHARED_BUFFER_SIZE
};

/* WiFi ***********************************************************************/

BaseType_t xSetWifiCredentials(const char* ssid, const char* password)
{
    BaseType_t xRet = pdFALSE;
    wifi_config_t xWifiConfig = { 0 };
    strncpy((char*)xWifiConfig.sta.ssid, ssid, WIFI_CONFIG_SSID_BUFFER_SIZE);
    strncpy((char*)xWifiConfig.sta.password, password, 
        WIFI_CONFIG_PASS_BUFFER_SIZE);
    if(esp_wifi_set_config(WIFI_IF_STA, &xWifiConfig) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set WiFi credentials.");
    }
    else
    {
        xRet = pdTRUE;
    }

    return xRet;
}

/* TLS ************************************************************************/

BaseType_t xTlsConnect(NetworkContext_t* pxNetworkContext,
    const char* pcHostname, int xPort, const char* pcServerCertPem,
    const char* pcClientCertPem, const char* pcClientKeyPem)
{
    BaseType_t xRet = pdTRUE;

    esp_tls_cfg_t xEspTlsConfig = {
        .cacert_buf = (const unsigned char*)pcServerCertPem,
        .cacert_bytes = strlen(pcServerCertPem) + 1,
        .clientcert_buf = (const unsigned char*)pcClientCertPem,
        .clientcert_bytes = strlen(pcClientCertPem) + 1,
        .clientkey_buf = (const unsigned char*)pcClientKeyPem,
        .clientkey_bytes = strlen(pcClientKeyPem) + 1,
    };

    esp_tls_t* pxTls = esp_tls_init();
    pxNetworkContext->pxTls = pxTls;

    if (esp_tls_conn_new_sync(pcHostname, strlen(pcHostname), xPort, 
        &xEspTlsConfig, pxTls) <= 0)
    {
        xRet = pdFALSE;
    }

    return xRet;
}

BaseType_t xTlsDisconnect(NetworkContext_t* pxNetworkContext)
{
    BaseType_t xRet = pdTRUE;

    if (pxNetworkContext->pxTls != NULL && 
        esp_tls_conn_destroy(pxNetworkContext->pxTls) < 0)
    {
        xRet = pdFALSE;
    }

    return xRet;
}

static int32_t prvEspTlsTransportSend(NetworkContext_t* pxNetworkContext,
    const void* pvData, size_t uxDataLen)
{
    int32_t lBytesSent = 0;

    lBytesSent = esp_tls_conn_write(pxNetworkContext->pxTls, pvData, uxDataLen);

    return lBytesSent;
}

static int32_t prvEspTlsTransportRecv(NetworkContext_t* pxNetworkContext,
    void* pvData, size_t uxDataLen)
{
    int32_t lBytesRead = 0;

    lBytesRead = esp_tls_conn_read(pxNetworkContext->pxTls, pvData, uxDataLen);

    return lBytesRead;
}

/* MQTT ***********************************************************************/

static uint32_t prvMqttGetTimeMs(void)
{
    TickType_t xTickCount = 0;
    uint32_t ulTimeMs = 0UL;

    /* Get the current tick count. */
    xTickCount = xTaskGetTickCount();

    /* Convert the ticks to milliseconds. */
    ulTimeMs = (uint32_t)xTickCount * MILLISECONDS_PER_TICK;

    /* Reduce ulGlobalEntryTimeMs from obtained time so as to always return the
     * elapsed time in the application. */
    ulTimeMs = (uint32_t)(ulTimeMs - ulGlobalEntryTimeMs);

    return ulTimeMs;
}

static void prvMqttEventCallback(MQTTContext_t* pxMQTTContext,
    MQTTPacketInfo_t* pxPacketInfo,
    MQTTDeserializedInfo_t* pxDeserializedInfo)
{
    /* The MQTT context is not used for this demo. */
    (void)pxMQTTContext;

    uint16_t usPacketId = pxDeserializedInfo->packetIdentifier;

    switch (pxPacketInfo->type)
    {
    case MQTT_PACKET_TYPE_PUBACK:
        ESP_LOGI(TAG,"PUBACK received for packet Id %u.", usPacketId);
        /* Make sure ACK packet identifier matches with Request packet 
         * identifier. */
        assert(usPublishPacketIdentifier == usPacketId);
        break;

    case MQTT_PACKET_TYPE_SUBACK:
        ESP_LOGI(TAG, "SUBACK received for packet Id %u.", usPacketId);
        break;

    case MQTT_PACKET_TYPE_UNSUBACK:
        ESP_LOGI(TAG, "UNSUBACK received for packet Id %u.", usPacketId);
        break;

    case MQTT_PACKET_TYPE_PINGRESP:
        ESP_LOGI(TAG,"Ping Response successfully received.");

        break;

        /* Any other packet type is invalid. */
    default:
        ESP_LOGE(TAG, "Unkown response received for packet Id %u.", usPacketId);
        break;
    }
}

static MQTTStatus_t prvMqttInit(NetworkContext_t* pxNetworkContext, 
    MQTTContext_t* pxMQTTContext)
{
    MQTTStatus_t xResult;
    TransportInterface_t xTransport;

    /* Set up transport for coreMQTT */
    xTransport.pNetworkContext = pxNetworkContext;
    xTransport.send = prvEspTlsTransportSend;
    xTransport.recv = prvEspTlsTransportRecv;

    /* Gives an initial value to the timer for MQTT timing */
    ulGlobalEntryTimeMs = prvMqttGetTimeMs();

    xResult = MQTT_Init(pxMQTTContext, 
        &xTransport, 
        prvMqttGetTimeMs,
        prvMqttEventCallback,
        &xBuffer);

    return xResult;
}

MQTTStatus_t eMqttConnect(MQTTContext_t* pxMQTTContext, const char* thingName)
{
    bool xSessionPresent;
    MQTTStatus_t xResult;
    MQTTConnectInfo_t xConnectInfo;

    /* Some fields are not used in this demo so start with everything at 0. */
    (void)memset((void*)&xConnectInfo, 0x00, sizeof(xConnectInfo));

    /* Start with a clean session i.e. direct the MQTT broker to discard any
     * previous session data. Also, establishing a connection with clean session
     * will ensure that the broker does not store any data when this client
     * gets disconnected. */
    xConnectInfo.cleanSession = true;

    /* The client identifier is used to uniquely identify this MQTT client to
     * the MQTT broker. In a production device the identifier can be something
     * unique, such as a device serial number. */
    xConnectInfo.pClientIdentifier = thingName;
    xConnectInfo.clientIdentifierLength = (uint16_t)strlen(thingName);

    /* Set MQTT keep-alive period. If the application does not send packets at 
     * an interval less than the keep-alive period, the MQTT library will send 
     * PINGREQ packets. */
    xConnectInfo.keepAliveSeconds = 5U;

    xResult = MQTT_Connect(pxMQTTContext,
        &xConnectInfo,
        NULL,
        1000U,
        &xSessionPresent);

    return xResult;
}

MQTTStatus_t eMqttPublishQuickConnect(MQTTContext_t* pxMQTTContext, 
    const char* pcThingName,
    const char* pcSendBuffer)
{
    MQTTStatus_t xResult;
    MQTTPublishInfo_t xMQTTPublishInfo = { 0 };

    xMQTTPublishInfo.qos = MQTTQoS0;
    xMQTTPublishInfo.retain = false;
    xMQTTPublishInfo.pTopicName = pcThingName;
    xMQTTPublishInfo.topicNameLength = (uint16_t)strlen(pcThingName);
    xMQTTPublishInfo.pPayload = pcSendBuffer;
    xMQTTPublishInfo.payloadLength = strlen(pcSendBuffer);

    /* Get a unique packet id. */
    usPublishPacketIdentifier = MQTT_GetPacketId(pxMQTTContext);

    /* Send PUBLISH packet. */
    xResult = MQTT_Publish(pxMQTTContext, &xMQTTPublishInfo, 
        usPublishPacketIdentifier);

    if (xResult == MQTTSendFailed)
    {
        ESP_LOGE(TAG, "MQTT publish failed.");
    }
    else
    {
        ESP_LOGI(TAG, "MQTT publish succeeded.\n Sent: %s", pcSendBuffer);
    }

    return xResult;
}

/* Initialization *************************************************************/

void vNetworkingInit(NetworkContext_t* pxNetworkContext,
    MQTTContext_t* pxMQTTContext)
{
    /* Initialize Network Interface
     * Necessary for:
     * - Initializing the underlying TCP/IP Stack (lwIP)
     * - Connecting to the internet using WiFI drivers
     * - Using TLS (ESP-TLS)
     */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize WiFi */
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif != NULL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Initialize MQTT */
    (void)prvMqttInit(pxNetworkContext, pxMQTTContext);
}

