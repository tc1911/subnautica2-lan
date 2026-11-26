cmake_minimum_required(VERSION 3.20)
project(sn2lan VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# MinHook (vendored, see third_party/minhook)
# ---------------------------------------------------------------------------
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook
    PUBLIC third_party/minhook/include
    PRIVATE third_party/minhook/src
)
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# The mod DLL (loaded by the UE4SS Lua mod through package.loadlib)
# ---------------------------------------------------------------------------
add_library(sn2lan SHARED
    src/mod.cpp
    src/hooks.cpp
    src/tunnel.cpp
    src/control.cpp
    src/config.cpp
    src/log.cpp
    src/crashlog.cpp
    src/protocol.cpp
    src/overlay.cpp
    src/game_overlay.cpp
)
target_include_directories(sn2lan PRIVATE src)
target_link_libraries(sn2lan PRIVATE minhook ws2_32)
target_compile_definitions(sn2lan PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
set_target_properties(sn2lan PROPERTIES PREFIX "" OUTPUT_NAME "sn2lan")

if(MINGW)
    # Keep the DLL free of libstdc++/libgcc/winpthread DLL dependencies: the
    # game directory will not have them.
    target_link_options(sn2lan PRIVATE -static -static-libgcc -static-libstdc++)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(sn2lan PRIVATE -s)
    endif()
    target_compile_options(sn2lan PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
endif()

# ---------------------------------------------------------------------------
# Standalone build: proxy dwmapi.dll, which the game imports at startup. This
# gives us code execution without UE4SS (see src/proxy.cpp).
# ---------------------------------------------------------------------------
add_library(sn2lan_dwmapi SHARED
    src/proxy.cpp
    src/mod.cpp
    src/hooks.cpp
    src/tunnel.cpp
    src/control.cpp
    src/config.cpp
    src/log.cpp
    src/crashlog.cpp
    src/protocol.cpp
    src/overlay.cpp
    src/game_overlay.cpp
)
target_include_directories(sn2lan_dwmapi PRIVATE src)
target_link_libraries(sn2lan_dwmapi PRIVATE minhook ws2_32)
target_compile_definitions(sn2lan_dwmapi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 SN2LAN_STANDALONE=1)
set_target_properties(sn2lan_dwmapi PROPERTIES PREFIX "" OUTPUT_NAME "dwmapi")

if(MINGW)
    target_link_options(sn2lan_dwmapi PRIVATE -static -static-libgcc -static-libstdc++)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(sn2lan_dwmapi PRIVATE -s)
    endif()
    target_compile_options(sn2lan_dwmapi PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
endif()

# ---------------------------------------------------------------------------
# Host-side unit tests for the platform independent wire protocol
# ---------------------------------------------------------------------------
if(MINGW)
    # Loopback integration test for the UDP tunnel (run through Wine on Linux).
    add_executable(test_tunnel tests/test_tunnel.cpp src/tunnel.cpp src/protocol.cpp src/control.cpp src/log.cpp)
    target_include_directories(test_tunnel PRIVATE src)
    target_link_libraries(test_tunnel PRIVATE ws2_32)
    target_compile_definitions(test_tunnel PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    target_compile_options(test_tunnel PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_options(test_tunnel PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(NOT MINGW AND NOT WIN32)
    enable_testing()
    add_executable(test_protocol tests/test_protocol.cpp src/protocol.cpp)
    target_include_directories(test_protocol PRIVATE src)
    add_test(NAME protocol COMMAND test_protocol)
endif()
