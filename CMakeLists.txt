cmake_minimum_required(VERSION 3.16)
project(nxcord-nx VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NINTENDO_SWITCH)
  set(NXCORD_PLATFORM switch)
else()
  set(NXCORD_PLATFORM linux)
endif()
set(NXCORD_DEPS ${CMAKE_SOURCE_DIR}/deps/${NXCORD_PLATFORM} CACHE PATH "Prefix from scripts/build-deps.sh")
add_compile_definitions(NXCORD_VERSION="${PROJECT_VERSION}")

add_subdirectory(core)

if(NINTENDO_SWITCH)
  add_subdirectory(platform/switch)
else()
  add_subdirectory(platform/linux)
endif()
