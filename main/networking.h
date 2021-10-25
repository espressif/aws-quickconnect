#ifndef FMCONNECT_NETWORKING_H
#define FMCONNECT_NETWORKING_H

#include "core_mqtt.h"
#include "esp_tls.h"

struct NetworkContext
{
    esp_tls_t* pxTls;
};

void networkingInit(NetworkContext_t* pxNetworkContext,
    MQTTContext_t* pxMQTTContext);

void setWifiCredentials(const char* ssid, const char* password);

BaseType_t tlsConnect(NetworkContext_t* pxNetworkContext,
    const char* hostname,
    int port,
    const char* server_cert_pem,
    const char* client_cert_pem,
    const char* client_key_pem);

esp_err_t tlsDisconnect(NetworkContext_t* pxNetworkContext);

MQTTStatus_t eMqttConnect(MQTTContext_t* pxMQTTContext, const char* thingName);

MQTTStatus_t eMqttPublishFMConnect(MQTTContext_t* pxMQTTContext, const char* thingName, const char* sendBuffer);

#endif /* FMCONNECT_NETWORKING_H */