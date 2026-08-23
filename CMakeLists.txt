cmake_minimum_required(VERSION 3.16)
project(okrapm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(lib/okrapmlib)
add_subdirectory(src/lunar)

enable_testing()
add_executable(lunar_test tests/test_all.cpp)
target_link_libraries(lunar_test PRIVATE okrapmlib)
add_test(NAME LunarTest COMMAND lunar_test)
