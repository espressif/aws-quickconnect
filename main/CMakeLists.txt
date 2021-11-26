idf_component_register(SRCS "main.c" "networking.c"
    INCLUDE_DIRS ".")
target_add_binary_data(${COMPONENT_TARGET} 
    "server_cert/root_ca.crt" TEXT)