add_executable(nxcord-sys main.cpp ipc_server.cpp audio_io.cpp config.cpp ../compat/nxcompat.c)
target_link_libraries(nxcord-sys PRIVATE nxcord_core)
nx_create_exefs(nxcord-sys CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/sysmodule.json OUTPUT ${CMAKE_BINARY_DIR}/exefs.nsp)
