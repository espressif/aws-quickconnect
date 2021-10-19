idf_component_register(SRCS "main.c" "networking.c"
                       INCLUDE_DIRS ".")

set(ulp_app_name ulp_tsens)
set(ulp_s_sources ulp/ulp_tsens.S)
set(ulp_exp_dep_srcs "main.c")

#ulp_embed_binary(${ulp_app_name} "${ulp_s_sources}" "${ulp_exp_dep_srcs}")