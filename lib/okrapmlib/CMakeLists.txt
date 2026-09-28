cmake_minimum_required(VERSION 3.16)
project(okrapmlib CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

add_library(okrapmlib STATIC
	src/version_scheme.cpp
	src/dependency.cpp
	src/dependency_serialization.cpp
	src/rpm_repository.cpp
	src/rpm_artifact.cpp
	src/rpm_resolver.cpp
	src/rpm_script.cpp
	src/object.cpp
)

target_include_directories(okrapmlib PUBLIC include)

enable_testing()

add_executable(vs_test tests/version_scheme_test.cpp)
target_link_libraries(vs_test PRIVATE okrapmlib)
add_test(NAME version_scheme COMMAND vs_test)

add_executable(dep_test tests/dependency_test.cpp)
target_link_libraries(dep_test PRIVATE okrapmlib)
add_test(NAME dependency COMMAND dep_test)

add_executable(ds_test tests/dependency_serialization_test.cpp)
target_link_libraries(ds_test PRIVATE okrapmlib)
add_test(NAME dependency_serialization COMMAND ds_test)

find_program(RPM_PROGRAM rpm)
if(RPM_PROGRAM)
	add_executable(repo_test tests/rpm_repository_test.cpp)
	target_link_libraries(repo_test PRIVATE okrapmlib)

	add_executable(art_test tests/rpm_artifact_test.cpp)
	target_link_libraries(art_test PRIVATE okrapmlib)

	add_executable(res_test tests/rpm_resolver_test.cpp)
	target_link_libraries(res_test PRIVATE okrapmlib)
endif()

add_executable(fuzz_main tests/fuzz_main.cpp)
target_link_libraries(fuzz_main PRIVATE okrapmlib)