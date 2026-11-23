# Integrate only through the build owner. This target never installs or launches.
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
      --output "${UOS_PACKAGE_OUTPUT}"
    DEPENDS uos_gateway VERBATIM)
endif()
