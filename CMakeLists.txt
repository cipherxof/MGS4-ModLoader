cmake_minimum_required(VERSION 3.20)

project(MGS4ModLoader VERSION 0.0.1 LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Build spdlog tests" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Build spdlog benchmarks" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "Generate spdlog install targets" FORCE)

add_subdirectory(external/minhook)
add_subdirectory(external/spdlog)

add_library(MGS4ModLoader SHARED
    src/config.cpp
    src/dllmain.cpp
    src/modloader.cpp
    src/utils.cpp
)

target_compile_features(MGS4ModLoader PRIVATE cxx_std_17)
target_compile_definitions(MGS4ModLoader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE NDEBUG)
target_include_directories(MGS4ModLoader PRIVATE src)
target_link_libraries(MGS4ModLoader PRIVATE minhook spdlog::spdlog)

if(MINGW)
    target_link_options(MGS4ModLoader PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()

set_target_properties(MGS4ModLoader PROPERTIES
    PREFIX ""
    OUTPUT_NAME "MGS4ModLoader"
    SUFFIX ".asi"
)

install(TARGETS MGS4ModLoader RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES MGS4ModLoader.ini DESTINATION .)
