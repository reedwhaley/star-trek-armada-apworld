cmake_minimum_required(VERSION 3.20)
project(star_trek_armada_apworld LANGUAGES NONE)

option(ARMADA_BUILD_OBSERVER "Build the read-only x86 mission-result observer" OFF)
option(ARMADA_BUILD_INJECTOR "Build the x86 local observer DLL injector" OFF)
option(ARMADA_USE_CAMPAIGN_PICKER_ROUTE "Use the validated native campaign picker route" ON)

# Release artifacts have a stable location that the packaged client can use.
set(ARMADA_RELEASE_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${configuration} "${ARMADA_RELEASE_BIN_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${configuration} "${ARMADA_RELEASE_BIN_DIR}")
endforeach()

if(ARMADA_BUILD_OBSERVER)
    enable_language(CXX)
    add_subdirectory(bridge/armada_hook)
endif()

if(ARMADA_BUILD_INJECTOR)
    enable_language(CXX)
    add_subdirectory(bridge/injector)
endif()
