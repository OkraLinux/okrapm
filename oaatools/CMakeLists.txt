cmake_minimum_required(VERSION 3.16)
project(oaatools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# C++ Core Library
add_library(oaa_core STATIC
    src/oaa_core.cpp
)

# C++ CLI Tool
add_executable(oaa-cli
    src/main.cpp
)
target_link_libraries(oaa-cli PRIVATE oaa_core stdc++fs)

install(TARGETS oaa-cli DESTINATION bin)
