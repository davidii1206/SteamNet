cmake_minimum_required(VERSION 3.21)

project(SteamNet VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

set(STEAMWORKS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "Path to the Steamworks SDK root")

if(NOT EXISTS "${STEAMWORKS_SDK_ROOT}/public/steam/steam_api.h")
    message(FATAL_ERROR
        "Steamworks SDK not found in '${STEAMWORKS_SDK_ROOT}'. "
        "Place the Steamworks SDK under the project root in 'sdk/' or set STEAMWORKS_SDK_ROOT."
    )
endif()

find_library(STEAMWORKS_LIBRARY
    NAMES steam_api steam_api64 libsteam_api
    PATHS
        "${STEAMWORKS_SDK_ROOT}/redistributable_bin"
        "${STEAMWORKS_SDK_ROOT}/redistributable_bin/linux64"
        "${STEAMWORKS_SDK_ROOT}/redistributable_bin/linux32"
        "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64"
        "${STEAMWORKS_SDK_ROOT}/redistributable_bin/osx"
    PATH_SUFFIXES
        linux64 linux32 win64 osx
    NO_DEFAULT_PATH
)

if(NOT STEAMWORKS_LIBRARY)
    message(FATAL_ERROR "Steamworks library was not found under '${STEAMWORKS_SDK_ROOT}/redistributable_bin'.")
endif()

add_library(Steamworks::steam_api UNKNOWN IMPORTED GLOBAL)
set_target_properties(Steamworks::steam_api PROPERTIES
    IMPORTED_LOCATION "${STEAMWORKS_LIBRARY}"
)

find_package(entt CONFIG QUIET)
if(NOT entt_FOUND)
    FetchContent_Declare(
        entt
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG v3.13.2
        SYSTEM
    )
    FetchContent_MakeAvailable(entt)
endif()

# Transport-independent part: serialization, messages and ECS replication. Does not need Steam at runtime.
add_library(steamnet_core STATIC
    src/Messages/MessageRegistry.cpp
    src/Replication/Replication.cpp
)

target_include_directories(steamnet_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(steamnet_core PUBLIC EnTT::EnTT)
target_compile_features(steamnet_core PUBLIC cxx_std_20)

add_library(steamnet STATIC
    src/steamnet/Network.cpp
    src/Session/LobbyManager.cpp
    src/Transport/SteamTransport.cpp
)

target_include_directories(steamnet
    SYSTEM PUBLIC
        "${STEAMWORKS_SDK_ROOT}/public"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(steamnet
    PUBLIC
        steamnet_core
        Steamworks::steam_api
)

add_executable(steamnet_example
    examples/BasicMultiplayer/main.cpp
)

target_link_libraries(steamnet_example PRIVATE steamnet)

if(UNIX AND NOT APPLE)
    set_target_properties(steamnet_example PROPERTIES
        BUILD_RPATH "${STEAMWORKS_SDK_ROOT}/redistributable_bin/linux64"
    )
endif()

option(STEAMNET_BUILD_TESTS "Build the steamnet tests" ON)
if(STEAMNET_BUILD_TESTS)
    enable_testing()
    add_executable(steamnet_tests tests/tests.cpp)
    target_link_libraries(steamnet_tests PRIVATE steamnet_core)
    add_test(NAME steamnet_tests COMMAND steamnet_tests)
endif()

message(STATUS "Steamworks SDK: ${STEAMWORKS_SDK_ROOT}")
message(STATUS "Steamworks library: ${STEAMWORKS_LIBRARY}")
message(STATUS "Configured target: steamnet")

