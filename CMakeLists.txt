cmake_minimum_required(VERSION 3.20)

project(OnlineGameBackend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(online_game_backend_project_options INTERFACE)

target_compile_options(
    online_game_backend_project_options
    INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(
    online_game_backend_smoke
    apps/project_smoke_main.cpp
)

target_link_libraries(
    online_game_backend_smoke
    PRIVATE
    online_game_backend_project_options
)

include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(
        online_game_backend_smoke_test
        tests/project_smoke_test.cpp
    )

    target_link_libraries(
        online_game_backend_smoke_test
        PRIVATE
        online_game_backend_project_options
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(online_game_backend_smoke_test)
endif()
