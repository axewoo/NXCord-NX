# Tesla overlay (works in Ultrahand). Overlays are NROs with an .ovl extension.
set(LIBTESLA_DIR ${CMAKE_SOURCE_DIR}/deps/src/libtesla CACHE PATH "libtesla checkout")
add_executable(nxcord-ovl main.cpp)
target_include_directories(nxcord-ovl PRIVATE ${LIBTESLA_DIR}/include)
target_compile_options(nxcord-ovl PRIVATE -Wno-missing-field-initializers)
set_target_properties(nxcord-ovl PROPERTIES CXX_STANDARD 20)
nx_generate_nacp(OUTPUT nxcord-ovl.nacp NAME "NXCord" AUTHOR "axewoo, Grarak" VERSION "${PROJECT_VERSION}")
nx_create_nro(nxcord-ovl NACP nxcord-ovl.nacp NOICON OUTPUT ${CMAKE_BINARY_DIR}/nxcord.ovl)
