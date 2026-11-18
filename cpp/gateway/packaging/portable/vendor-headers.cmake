# base64.cpp includes simdutf by its tree path, outside CMake's system include list.
set_property(SOURCE src/rt/base64.cpp APPEND PROPERTY COMPILE_OPTIONS --system-header-prefix=third_party/)
