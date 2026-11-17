# Integrate only through the build owner. This target never installs or launches.
set(UOS_PACKAGE_NATIVE_KV_DIR "" CACHE PATH "Explicit genuine offline native KV artifact directory")
set(UOS_PACKAGE_NATIVE_KV_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/native-kv/inputs.json" CACHE FILEPATH "Pinned native KV inputs")
set(UOS_PACKAGE_NATIVE_KV_PROVENANCE_SHA256 "" CACHE STRING "Independently supplied native KV provenance SHA256")
set(UOS_PACKAGE_PERMISSIONS_DIR "" CACHE PATH "Explicit genuine offline Linux permissions artifact directory")
set(UOS_PACKAGE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}-package" CACHE PATH "New task-worktree package output directory")
if(UOS_GATEWAY_IS_SDK)
  add_custom_target(package
    COMMAND "${CMAKE_COMMAND}" -E echo "SDK comparison packaging is not primary acceptance"
    COMMAND "${CMAKE_COMMAND}" -E false VERBATIM)
else()
  add_custom_target(package
    COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/package.py"
      --build-dir "${CMAKE_CURRENT_BINARY_DIR}"
      --profile-manifest "${CMAKE_CURRENT_BINARY_DIR}/profile-owner.json"
      --native-kv "${UOS_PACKAGE_NATIVE_KV_DIR}"
      --native-kv-inputs "${UOS_PACKAGE_NATIVE_KV_INPUTS}"
      --native-kv-provenance-sha256 "${UOS_PACKAGE_NATIVE_KV_PROVENANCE_SHA256}"
      --permissions "${UOS_PACKAGE_PERMISSIONS_DIR}"
      --output "${UOS_PACKAGE_OUTPUT}"
    DEPENDS uos_gateway uos_kv_convert uos_kv_proof VERBATIM)
endif()
