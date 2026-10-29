# nxcord-cli: test the core from a PC before putting it on the Switch.
find_package(Threads REQUIRED)
add_executable(nxcord-cli main.cpp)
target_link_libraries(nxcord-cli PRIVATE nxcord_core Threads::Threads)
