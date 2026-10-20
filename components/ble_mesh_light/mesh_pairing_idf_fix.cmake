# Build-local workaround for ESP-IDF 5.5.5's one-shot BLE advertising ownership.
# The SDK cache is never changed. Keep the BLE advertiser's owner reference
# until ble_adv_tx_reset(), while adv_send() releases the queued reference.
include_guard(GLOBAL)

function(ble_mesh_fix_one_shot_advertising)
  if(NOT COMMAND idf_component_get_property)
    return()
  endif()
  idf_component_get_property(bt_dir bt COMPONENT_DIR)
  idf_component_get_property(bt_library bt COMPONENT_LIB)
  set(original "${bt_dir}/esp_ble_mesh/core/ble_adv.c")
  file(SHA256 "${original}" sdk_sha)
  if(NOT sdk_sha STREQUAL "6ead4e6b91303b44e0b91fa98cc03a0202727b71aad06d24db542e6d69d73691")
    message(FATAL_ERROR "ble_mesh_light: review the one-shot BLE advertising workaround for this ESP-IDF source version; tested with 5.5.5")
  endif()
  file(READ "${original}" source)
  string(REPLACE
    "            tx->param.count = 0;\n            net_buf_unref(buf);\n            return 0;"
    "            tx->param.count = 0;\n            return 0;"
    source "${source}")
  string(REPLACE
    "    } else {\n        /* Send the BLE advertising packet only once */\n        net_buf_unref(buf);\n    }"
    "    } else {\n        /* Retain the owner reference until the one-shot end callback. */\n    }"
    source "${source}")
  set(patched "${CMAKE_BINARY_DIR}/mesh_pairing_ble_adv.c")
  set(previous "")
  if(EXISTS "${patched}")
    file(READ "${patched}" previous)
  endif()
  if(NOT source STREQUAL previous)
    file(WRITE "${patched}" "${source}")
  endif()
  # Compile the corrected copy with the original bt target's flags/includes.
  # The original source remains untouched and is skipped only in this project.
  set_source_files_properties("${original}" TARGET_DIRECTORY ${bt_library}
                             PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(${bt_library} PRIVATE "${patched}")
  message(STATUS "ble_mesh_light: applied build-local one-shot BLE advertising ownership fix")
endfunction()

# ESP-IDF creates component targets after the underlying CMake project() call.
cmake_language(DEFER CALL ble_mesh_fix_one_shot_advertising)
