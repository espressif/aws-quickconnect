#ifndef FMCONNECT_NETWORKING_H
#define FMCONNECT_NETWORKING_H

#include "core_mqtt.h"
#include "esp_tls.h"

struct NetworkContext
{
    esp_tls_t* pxTls;
};

void vNetworkingInit(NetworkContext_t* pxNetworkContext,
    MQTTContext_t* pxMQTTContext);

BaseType_t xSetWifiCredentials(const char* ssid, const char* password);

BaseType_t xTlsConnect(NetworkContext_t* pxNetworkContext,
    const char* pcHostname, int xPort, const char* pcServerCertPem,
    const char* pcClientCertPem, const char* pcClientKeyPem);

BaseType_t xTlsDisconnect(NetworkContext_t* pxNetworkContext);

MQTTStatus_t eMqttConnect(MQTTContext_t* pxMQTTContext, 
    const char* pcThingName);

MQTTStatus_t eMqttPublishQuickConnect(MQTTContext_t* pxMQTTContext, 
    const char* pcThingName, const char* pcSendBuffer);

#endif /* FMCONNECT_NETWORKING_H */