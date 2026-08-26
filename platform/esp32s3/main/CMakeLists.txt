# The portable core is compiled straight into the device app rather than being
# vendored or duplicated, so the simulator and the firmware cannot drift.
#
# Listed explicitly rather than globbed: CMake evaluates a glob once at
# configure time, and ESP-IDF rejects CONFIGURE_DEPENDS during its requirement
# scan, so a newly baked font silently failed to link until a manual
# reconfigure. An explicit list fails loudly at edit time instead.
set(WEDGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(WEDGE_CORE
    ${WEDGE_ROOT}/core/src/ambient.c
    ${WEDGE_ROOT}/core/src/app.c
    ${WEDGE_ROOT}/core/src/canvas.c
    ${WEDGE_ROOT}/core/src/faces/home.c
    ${WEDGE_ROOT}/core/src/faces/message.c
    ${WEDGE_ROOT}/core/src/faces/provision.c
    ${WEDGE_ROOT}/core/src/fonts/font_clock.c
    ${WEDGE_ROOT}/core/src/fonts/font_clock_s.c
    ${WEDGE_ROOT}/core/src/fonts/font_date.c
    ${WEDGE_ROOT}/core/src/fonts/font_kicker.c
    ${WEDGE_ROOT}/core/src/fonts/font_label.c
    ${WEDGE_ROOT}/core/src/fonts/font_msg.c
    ${WEDGE_ROOT}/core/src/fonts/font_msg_s.c
    ${WEDGE_ROOT}/core/src/fonts/font_quote.c
    ${WEDGE_ROOT}/core/src/fonts/font_offer.c
    ${WEDGE_ROOT}/core/src/message.c
    ${WEDGE_ROOT}/core/src/scene.c
    ${WEDGE_ROOT}/core/src/spring.c
    ${WEDGE_ROOT}/core/src/text.c)

idf_component_register(
    SRCS "main.c" "rm67162.c" "cst816.c" "net.c" "store.c" "provision.c" "relay.c" ${WEDGE_CORE}
    INCLUDE_DIRS "." "${WEDGE_ROOT}/core/include" "${WEDGE_ROOT}/core/src"
                 "${WEDGE_ROOT}/core/src/faces"
    REQUIRES driver esp_lcd esp_wifi esp_http_client esp_http_server esp-tls nvs_flash json
             esp_netif esp_event esp_timer lwip mbedtls)
