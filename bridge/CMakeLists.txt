add_library(okrapm_opsis_bridge STATIC
    src/opsis_bridge.cpp
)

target_include_directories(okrapm_opsis_bridge
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(okrapm_opsis_bridge
    PUBLIC
        okrapmlib
        opsislib
)

target_compile_features(okrapm_opsis_bridge PUBLIC cxx_std_17)
