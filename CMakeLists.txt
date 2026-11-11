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

# Rounds the long fuzz runs found faults in, replayed on every test run.
if(TARGET helix_image_fuzz)
    add_test(NAME helix_image_fuzz_round_53736 COMMAND helix_image_fuzz --target=Tga --first=53736 --rounds=1)
    add_test(NAME helix_image_fuzz_round_56494 COMMAND helix_image_fuzz --target=Tga --first=56494 --rounds=1)
    add_test(NAME helix_image_fuzz_round_56522 COMMAND helix_image_fuzz --target=Tga --first=56522 --rounds=1)
    add_test(NAME helix_image_fuzz_round_122966 COMMAND helix_image_fuzz --target=Tga --first=122966 --rounds=1)
    add_test(NAME helix_image_fuzz_round_166436 COMMAND helix_image_fuzz --target=Tga --first=166436 --rounds=1)
    add_test(NAME helix_image_fuzz_round_723789 COMMAND helix_image_fuzz --target=Gif --first=723789 --rounds=1)
    add_test(NAME helix_image_fuzz_round_296497 COMMAND helix_image_fuzz --target=Psd --first=296497 --rounds=1)
endif()
