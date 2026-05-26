idf_component_register(SRCS "esp_ot_br.c"
                       "led.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_coex
                                esp_eth
                                esp_event
                                esp_netif
                                esp_ot_br_server
                                esp_ot_cli_extension
                                esp_driver_uart
                                mdns
                                nvs_config
                                nvs_flash
                                openthread
                                spiffs
                                thread_border_router
                                vfs
                       EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem
                       )
