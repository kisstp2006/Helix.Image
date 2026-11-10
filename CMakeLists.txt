cmake_minimum_required(VERSION 3.28)
project(Helix.Image LANGUAGES CXX)

if(NOT COMMAND helix_add_library)
    include("${CMAKE_CURRENT_SOURCE_DIR}/External/Helix.CMake/Helix.cmake")
endif()

helix_require(Core)
helix_require(Platform)
helix_require(Profiler)
helix_require(Jobs)
helix_add_library(helix_image ALIAS Image PUBLIC_DEPS Helix::Core PRIVATE_DEPS Helix::Platform Helix::Jobs)
# Copied public domain code, see THIRD-PARTY.txt.
target_include_directories(helix_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)

helix_add_test_suite(helix_image_tests DEPS Helix::Image Helix::Jobs)
helix_add_benchmark_suite(helix_image_bench DEPS Helix::Image Helix::Jobs)
helix_add_fuzz_suite(helix_image_fuzz DEPS Helix::Image ROUNDS 240)
