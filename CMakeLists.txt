cmake_minimum_required(VERSION 3.20)

project(OnlineGameBackend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(online_game_backend_project_options INTERFACE)

target_compile_options(online_game_backend_project_options INTERFACE -Wall -Wextra -Wpedantic -Werror)

find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql REQUIRED)

find_library(MYSQLCLIENT_LIBRARY NAMES mysqlclient REQUIRED)

add_library(online_game_backend_mysql INTERFACE)

target_include_directories(online_game_backend_mysql SYSTEM INTERFACE "${MYSQL_INCLUDE_DIR}")

target_link_libraries(online_game_backend_mysql INTERFACE "${MYSQLCLIENT_LIBRARY}")

add_library(online_game_backend_storage STATIC
    storage/mysql_RAII.cpp
)

target_include_directories(online_game_backend_storage
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(online_game_backend_storage
    PUBLIC
        online_game_backend_mysql
    PRIVATE
        online_game_backend_project_options
)

add_executable(online_game_backend_smoke apps/project_smoke_main.cpp)

target_link_libraries(online_game_backend_smoke PRIVATE online_game_backend_project_options)

add_executable(online_game_backend_match_lookup apps/match_lookup_main.cpp)

target_link_libraries(online_game_backend_match_lookup
    PRIVATE
        online_game_backend_project_options
        online_game_backend_storage
        online_game_backend_mysql
)

add_executable(online_game_backend_match_id_lookup apps/match_id_lookup_main.cpp)

target_link_libraries(online_game_backend_match_id_lookup PRIVATE online_game_backend_project_options online_game_backend_mysql)

include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(online_game_backend_smoke_test tests/project_smoke_test.cpp)
    target_link_libraries(online_game_backend_smoke_test PRIVATE online_game_backend_project_options GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(online_game_backend_smoke_test)
endif()
