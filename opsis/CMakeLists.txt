add_library(opsislib
	src/api.cpp
	src/core.cpp
	src/interpreter.cpp
	src/native.cpp
)

target_include_directories(opsislib
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(opsislib PUBLIC cxx_std_17)

add_executable(opsis
	src/main.cpp
)

target_link_libraries(opsis
	PRIVATE
		opsislib
)

install(TARGETS opsis DESTINATION bin)
