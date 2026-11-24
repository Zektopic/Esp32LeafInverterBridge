# Web assets are gzip-compressed at configure time and embedded into the app
# image (served with Content-Encoding: gzip). Editing a file triggers a
# re-configure through CMAKE_CONFIGURE_DEPENDS.
set(web_root "${CMAKE_CURRENT_LIST_DIR}/../../web")
set(web_assets
    "${web_root}/index.html"
    "${web_root}/static/bootstrap.min.css"
    "${web_root}/static/bootstrap.min.js"
    "${web_root}/static/jquery.min.js"
    "${web_root}/static/popper.min.js"
    "${CMAKE_CURRENT_LIST_DIR}/www/update.html")

set(gz_dir "${CMAKE_CURRENT_BINARY_DIR}/www_gz")
file(MAKE_DIRECTORY "${gz_dir}")
set(embedded_files "")
foreach(asset IN LISTS web_assets)
    get_filename_component(name "${asset}" NAME)
    set(gz "${gz_dir}/${name}.gz")
    get_filename_component(asset_dir "${asset}" DIRECTORY)
    # gzip a single file ("raw" format), without the directory in the stream
    file(ARCHIVE_CREATE OUTPUT "${gz}" PATHS "${name}" FORMAT raw COMPRESSION GZip WORKING_DIRECTORY "${asset_dir}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asset}")
    list(APPEND embedded_files "${gz}")
endforeach()

idf_component_register(
    SRCS "app_main.c"
         "bridge.c"
         "can_twai.c"
         "mcp2515.c"
         "settings_store.c"
         "web_server.c"
         "network.c"
    INCLUDE_DIRS "."
    REQUIRES leaf_can bridge_settings
             esp_driver_twai esp_driver_spi esp_driver_gpio
             esp_http_server esp_wifi esp_eth esp_netif esp_event nvs_flash
             app_update esp_app_format esp_timer
    EMBED_FILES ${embedded_files})
