idf_component_register(SRCS "main.c" "networking.c" "qc_graph_json_builder.c"
    INCLUDE_DIRS ".")
target_add_binary_data(${COMPONENT_TARGET} 
    "server_cert/root_ca.crt" TEXT)