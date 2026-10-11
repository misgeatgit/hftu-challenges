cmake_minimum_required(VERSION 3.20)
project(challenge-02-multi-book LANGUAGES CXX)

add_executable(benchmark benchmark.cpp solution/solution.cpp)

add_executable(unit_tests test_correctness.cpp solution/solution.cpp)
target_compile_features(unit_tests PRIVATE cxx_std_23)
target_compile_options(unit_tests PRIVATE -O2 -g -march=native -Wall -Wextra)
target_include_directories(unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/solution
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../common/challenge.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../common/challenge.cmake)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/common/challenge.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/common/challenge.cmake)
endif()
