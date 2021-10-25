idf_component_register(SRCS "esp_rmaker_claim.c"
                       INCLUDE_DIRS "."
                       REQUIRES json_generator json_parser mbedtls esp-tls esp_http_client)
target_add_binary_data(${COMPONENT_TARGET} "server_certs/rmaker_claim_service_server.crt" TEXT)
