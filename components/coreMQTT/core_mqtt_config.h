#ifndef CORE_MQTT_CONFIG_H_
#define CORE_MQTT_CONFIG_H_
/**
 * @brief The maximum number of MQTT PUBLISH messages that may be pending
 * acknowledgement at any time.
 *
 * QoS 1 and 2 MQTT PUBLISHes require acknowledgement from the server before
 * they can be completed. While they are awaiting the acknowledgement, the
 * client must maintain information about their state. The value of this
 * macro sets the limit on how many simultaneous PUBLISH states an MQTT
 * context maintains.
 */
#define MQTT_STATE_ARRAY_MAX_COUNT    ( 10U )

 /**
  * @brief Number of milliseconds to wait for a ping response to a ping
  * request as part of the keep-alive mechanism.
  *
  * If a ping response is not received before this timeout, then
  * #MQTT_ProcessLoop will return #MQTTKeepAliveTimeout.
  */
#define MQTT_PINGRESP_TIMEOUT_MS      ( 5000U )

#endif /* ifndef CORE_MQTT_CONFIG_H_ */