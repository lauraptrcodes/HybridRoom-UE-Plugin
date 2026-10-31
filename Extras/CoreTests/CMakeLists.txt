# CoreTests — baut den Kern von HybridAcousticsCore außerhalb von Unreal (Tests + Python-Anbindung).
# Liegt bewusst NICHT unter Source/: UBT kompiliert jede .cpp im Modulordner mit.
cmake_minimum_required(VERSION 3.20)
project(HybridAcousticsCoreTests CXX)

# Pfad zum Kern-Modul (Standard: Plugins/HybridAcoustics/Source/HybridAcousticsCore)
set(HAC_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/HybridAcousticsCore" CACHE PATH "Ordner des Moduls HybridAcousticsCore")
if(NOT EXISTS "${HAC_CORE_DIR}/Public/FDTDGrid.h")
  message(FATAL_ERROR "FDTDGrid.h nicht gefunden unter ${HAC_CORE_DIR}/Public (-DHAC_CORE_DIR=... setzen)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(HAC_BUILD_TESTS  "GoogleTest-Tests bauen" ON)
option(HAC_BUILD_PYTHON "pybind11-Modul hybridcore bauen" ON)

# ---------------------------------------------------------------- Kernbibliothek
# Alle .cpp aus Private/ einsammeln, außer der UE-Moduldatei (die braucht Unreal-Header)
file(GLOB HAC_CORE_SOURCES CONFIGURE_DEPENDS "${HAC_CORE_DIR}/Private/*.cpp")
list(FILTER HAC_CORE_SOURCES EXCLUDE REGEX "HybridAcousticsCoreModule\\.cpp$")
message(STATUS "Kern-Quellen: ${HAC_CORE_SOURCES}")
add_library(hac_core STATIC ${HAC_CORE_SOURCES})
target_include_directories(hac_core PUBLIC ${HAC_CORE_DIR}/Public)

# ---------------------------------------------------------------- Steam Audio (optional)
# Standard: dasselbe SDK, das auch Unreal benutzt (Source/ThirdParty/SteamAudio mit include/ und lib/)
set(STEAMAUDIO_DIR "${HAC_CORE_DIR}/../ThirdParty/SteamAudio" CACHE PATH "Steam-Audio-SDK (Ordner mit include/ und lib/)")
if(WIN32)
  set(HAC_SA_LIB     "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
  set(HAC_SA_RUNTIME "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll")
else()
  set(HAC_SA_LIB     "${STEAMAUDIO_DIR}/lib/linux-x64/libphonon.so")
  set(HAC_SA_RUNTIME "${HAC_SA_LIB}")
endif()
if(EXISTS "${STEAMAUDIO_DIR}/include/phonon.h" AND EXISTS "${HAC_SA_LIB}")
  set(HAC_HAS_SA ON)
  target_include_directories(hac_core SYSTEM PUBLIC "${STEAMAUDIO_DIR}/include")
  target_link_libraries(hac_core PUBLIC "${HAC_SA_LIB}")
  target_compile_definitions(hac_core PUBLIC HAC_WITH_STEAMAUDIO=1)
  message(STATUS "Steam Audio: ${STEAMAUDIO_DIR}")
else()
  set(HAC_HAS_SA OFF)
  target_compile_definitions(hac_core PUBLIC HAC_WITH_STEAMAUDIO=0)
  message(STATUS "Steam Audio nicht gefunden unter ${STEAMAUDIO_DIR} -> geometrischer Bake deaktiviert")
endif()

# Wie in Unreal: keine Exceptions/RTTI, Shadowing ist ein Fehler (UE: C4456/C4457/C4458)
if(MSVC)
  target_compile_options(hac_core PRIVATE /W4 /we4456 /we4457 /we4458 /GR- /EHs-c- /D_HAS_EXCEPTIONS=0)
else()
  target_compile_options(hac_core PRIVATE -Wall -Wextra -Wshadow -Werror -fno-exceptions -fno-rtti)
endif()

# ---------------------------------------------------------------- Tests
if(HAC_BUILD_TESTS)
  find_package(GTest QUIET)          # z. B. über vcpkg
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  enable_testing()
  add_executable(hac_tests
    tests/test_grid.cpp
    tests/test_modes.cpp
    tests/test_absorption.cpp
    tests/test_bake.cpp
    tests/test_hybrid.cpp
    tests/test_convolver.cpp
    tests/test_binaural.cpp
    tests/test_order4.cpp)
  target_include_directories(hac_tests PRIVATE tools)
  target_link_libraries(hac_tests PRIVATE hac_core GTest::gtest_main)
  if(HAC_HAS_SA AND WIN32)   # phonon.dll neben hac_tests.exe
    add_custom_command(TARGET hac_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HAC_SA_RUNTIME}" $<TARGET_FILE_DIR:hac_tests>)
  endif()
  include(GoogleTest)
  gtest_discover_tests(hac_tests DISCOVERY_TIMEOUT 60)
endif()

# ---------------------------------------------------------------- Python
if(HAC_BUILD_PYTHON)
  find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
  find_package(pybind11 CONFIG REQUIRED)   # pip install pybind11 -> -Dpybind11_DIR=$(python -m pybind11 --cmakedir)
  pybind11_add_module(hybridcore python/bindings.cpp)
  target_include_directories(hybridcore PRIVATE tools)
  target_link_libraries(hybridcore PRIVATE hac_core)
  if(HAC_HAS_SA AND WIN32)   # phonon.dll muss neben der .pyd liegen
    add_custom_command(TARGET hybridcore POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HAC_SA_RUNTIME}" $<TARGET_FILE_DIR:hybridcore>)
  endif()
endif()
