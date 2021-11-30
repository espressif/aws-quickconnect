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

#ifndef QUICK_CONNECT_NETWORKING_H
#define QUICK_CONNECT_NETWORKING_H

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

#endif /* QUICK_CONNECT_NETWORKING_H */