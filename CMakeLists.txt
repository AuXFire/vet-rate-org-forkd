# Builds the engine-independent core (Source/Undertow/Sim) outside Unreal, with its tests and a headless
# simulation. Unreal never sees this file: UnrealBuildTool compiles the same sources as part of the game module.
cmake_minimum_required(VERSION 3.16)
project(UndertowCore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB UT_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Source/Undertow/Sim/*.cpp)
add_library(ut_core STATIC ${UT_CORE_SOURCES})
target_include_directories(ut_core PUBLIC ${CMAKE_SOURCE_DIR}/Source/Undertow)
# Match what Unreal enforces (shadowing is an error there; exceptions and RTTI are off) so code that is clean
# here has the best chance of compiling unchanged in the engine.
target_compile_options(ut_core PUBLIC -Wall -Wextra -Wshadow -Wconversion -Wundef -Werror -fno-exceptions -fno-rtti)

file(GLOB UT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Tests/*.cpp)
add_executable(ut_tests ${UT_TEST_SOURCES})
target_link_libraries(ut_tests PRIVATE ut_core)
target_compile_definitions(ut_tests PRIVATE UT_DATA_DIR="${CMAKE_SOURCE_DIR}/Content/Data")

add_executable(ut_sim ${CMAKE_SOURCE_DIR}/Tools/sim/ut_sim.cpp)
target_link_libraries(ut_sim PRIVATE ut_core)

enable_testing()
# One ctest entry per suite; the suite name is the first argument of UT_TEST(suite, name).
foreach(suite json clock heat roads traffic driver dispatch mission economy crowd sim data)
  add_test(NAME ${suite} COMMAND ut_tests ${suite})
endforeach()
