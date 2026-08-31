########################################################################
# Build Soapy SDR support module for Sidekiq Devices
########################################################################
cmake_minimum_required(VERSION 2.8.7)
project(SoapySidekiq CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_COMPILER g++)

if(NOT DEFINED SIDEKIQ_SDK_DIR OR "${SIDEKIQ_SDK_DIR}" STREQUAL "")
    if(DEFINED ENV{SIDEKIQ_SDK_DIR} AND NOT "$ENV{SIDEKIQ_SDK_DIR}" STREQUAL "")
        set(SIDEKIQ_SDK_DIR "$ENV{SIDEKIQ_SDK_DIR}" CACHE PATH "Path to the Sidekiq SDK")
    else()
        set(SIDEKIQ_SDK_DIR "$ENV{HOME}/sidekiq_sdk_current" CACHE PATH "Path to the Sidekiq SDK")
    endif()
endif()

find_package(SoapySDR "0.4.0" NO_MODULE REQUIRED)
if (NOT SoapySDR_FOUND)
    message(FATAL_ERROR "Soapy SDR development files not found...")
endif ()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Sidekiq)

if (NOT Sidekiq_FOUND)
    message(FATAL_ERROR
        "Sidekiq SDK v4.26 or later not found.  "
        "Location can be specified with:"
        "  -DSIDEKIQ_SDK_DIR=/path/to/sidekiq_sdk_current")
endif ()

message(STATUS "SIDEKIQ_SDK_DIR - ${SIDEKIQ_SDK_DIR}")
message(STATUS "Sidekiq_INCLUDE_DIRS - ${Sidekiq_INCLUDE_DIRS}")
message(STATUS "Sidekiq_LIBRARIES - ${Sidekiq_LIBRARIES}")
message(STATUS "Sidekiq_BUILD_CONFIG - ${Sidekiq_BUILD_CONFIG}")
message(STATUS "Sidekiq_PKG_LIBRARY_DIRS - ${Sidekiq_PKG_LIBRARY_DIRS}")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${Sidekiq_INCLUDE_DIRS})

#enable c++11 features
if(CMAKE_COMPILER_IS_GNUCXX)
    #disable warnings for unused parameters
    add_definitions(-Wno-unused-parameter)
endif(CMAKE_COMPILER_IS_GNUCXX)

if (APPLE)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wc++11-extensions")
endif(APPLE)

SOAPY_SDR_MODULE_UTIL(
    TARGET SidekiqSupport
    SOURCES
        SoapySidekiq.hpp
        Registation.cpp
        Settings.cpp
        Streaming.cpp
        Sensor.cpp
    LIBRARIES
        ${Sidekiq_LIBRARIES}

)
