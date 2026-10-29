# Portable Discord core: gateway, voice + DAVE, audio mixing.
add_library(nxcord_core STATIC
  src/audio.cpp
  src/client.cpp
  src/dave.cpp
  src/gateway.cpp
  src/log.cpp
  src/tls.cpp
  src/util.cpp
  src/voice.cpp
  src/websocket.cpp
)
target_include_directories(nxcord_core PUBLIC include ${NXCORD_DEPS}/include)
target_compile_features(nxcord_core PUBLIC cxx_std_17)
target_compile_options(nxcord_core PRIVATE -Wall -Wextra)

# DAVE stack from scripts/build-deps.sh. Order matters for static linking.
target_link_directories(nxcord_core PUBLIC ${NXCORD_DEPS}/lib)
target_link_libraries(nxcord_core PUBLIC
  dave mlspp mls_ds hpke tls_syntax bytes crypto
  mbedtls mbedx509 mbedcrypto sodium opus)

if(NINTENDO_SWITCH)
  # mbedTLS, libsodium and Opus come from devkitPro's portlibs.
  target_include_directories(nxcord_core PUBLIC ${DEVKITPRO}/portlibs/switch/include)
  target_link_directories(nxcord_core PUBLIC ${DEVKITPRO}/portlibs/switch/lib)
endif()
