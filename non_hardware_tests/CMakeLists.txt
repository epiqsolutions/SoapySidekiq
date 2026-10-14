# Non-hardware test build: compiles portable driver logic without the Sidekiq SDK.
cmake_minimum_required(VERSION 3.16)
project(SoapySidekiqNonHardwareTests LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SOAPYSIDEKIQ_TEST_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SOAPYSIDEKIQ_TEST_TSAN "Enable ThreadSanitizer" OFF)

if(SOAPYSIDEKIQ_TEST_ASAN AND SOAPYSIDEKIQ_TEST_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan must use separate build directories")
endif()

add_library(non_hardware_test_options INTERFACE)
target_compile_options(non_hardware_test_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion;-Wshadow>)

if(SOAPYSIDEKIQ_TEST_ASAN)
    target_compile_options(non_hardware_test_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined,float-cast-overflow;-fno-sanitize-recover=all;-fno-omit-frame-pointer>)
    target_link_options(non_hardware_test_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined,float-cast-overflow>)
endif()

if(SOAPYSIDEKIQ_TEST_TSAN)
    target_compile_options(non_hardware_test_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=thread;-fno-omit-frame-pointer>)
    target_link_options(non_hardware_test_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=thread>)
endif()

add_executable(discovery_tests
    support/TestMain.cpp
    component/DeviceDiscoveryTests.cpp
    ../DeviceDiscovery.cpp)
target_include_directories(discovery_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(discovery_tests PRIVATE non_hardware_test_options)

add_test(NAME discovery.all COMMAND discovery_tests)
set_tests_properties(discovery.all PROPERTIES LABELS "component;non-hardware")

# Keep constructor validation independent from the hardware-owning device class.
add_executable(device_option_tests
    support/TestMain.cpp
    unit/DeviceOptionsTests.cpp
    ../DeviceOptions.cpp)
target_include_directories(device_option_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(device_option_tests PRIVATE non_hardware_test_options)

add_test(NAME device-options.all COMMAND device_option_tests)
set_tests_properties(device-options.all PROPERTIES LABELS "unit;non-hardware")

# Test sensor lifecycle and formatting without constructing a hardware device.
add_executable(sensor_tests
    support/TestMain.cpp
    unit/SensorReaderTests.cpp
    ../SensorReader.cpp)
target_include_directories(sensor_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(sensor_tests PRIVATE non_hardware_test_options)

add_test(NAME sensors.all COMMAND sensor_tests)
set_tests_properties(sensors.all PROPERTIES LABELS "unit;non-hardware")

# Validate stream requests without invoking SDK setup or allocating buffer pools.
add_executable(stream_configuration_tests
    support/TestMain.cpp
    unit/StreamConfigurationTests.cpp
    ../StreamConfiguration.cpp)
target_include_directories(stream_configuration_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(stream_configuration_tests PRIVATE non_hardware_test_options)

add_test(NAME stream-configuration.all COMMAND stream_configuration_tests)
set_tests_properties(stream-configuration.all PROPERTIES LABELS "unit;non-hardware")

# Test numerical stream conversion policy independently from SDK buffers.
add_executable(stream_conversion_tests
    support/TestMain.cpp
    unit/SampleConversionTests.cpp
    unit/TimestampConversionTests.cpp)
target_include_directories(stream_conversion_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(stream_conversion_tests PRIVATE non_hardware_test_options)

add_test(NAME stream-conversion.all COMMAND stream_conversion_tests)
set_tests_properties(stream-conversion.all PROPERTIES LABELS "unit;non-hardware")

# Exercise the integer fallback on hosts that normally select __int128.
add_executable(timestamp_portable_tests
    support/TestMain.cpp unit/TimestampConversionTests.cpp)
target_include_directories(timestamp_portable_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(timestamp_portable_tests PRIVATE
    SOAPYSIDEKIQ_FORCE_PORTABLE_TIMESTAMP=1)
target_link_libraries(timestamp_portable_tests PRIVATE non_hardware_test_options)
add_test(NAME timestamp-portable.all COMMAND timestamp_portable_tests)
set_tests_properties(timestamp-portable.all PROPERTIES LABELS "unit;non-hardware")

# Compile the production writeStream body against a minimal device/SDK fixture.
# Fail configuration if its boundaries change rather than testing a stale copy.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ../Streaming.cpp)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../Streaming.cpp" streaming_source)
string(FIND "${streaming_source}" "int SoapySidekiq::writeStream(" tx_write_start)
string(FIND "${streaming_source}" "int SoapySidekiq::readStreamStatus(" tx_write_end)
if(tx_write_start LESS 0 OR tx_write_end LESS tx_write_start)
    message(FATAL_ERROR "Could not locate the production TX write method for regression tests")
endif()
math(EXPR tx_write_length "${tx_write_end} - ${tx_write_start}")
string(SUBSTRING "${streaming_source}" ${tx_write_start} ${tx_write_length} tx_write_method)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/TxWriteMethod.inc" "${tx_write_method}")

find_package(Threads REQUIRED)
add_executable(tx_write_tests support/TestMain.cpp component/TxWriteStreamTests.cpp)
target_include_directories(tx_write_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(tx_write_tests PRIVATE non_hardware_test_options Threads::Threads)
add_test(NAME tx-write.all COMMAND tx_write_tests)
set_tests_properties(tx-write.all PROPERTIES LABELS "component;non-hardware")

add_custom_target(non_hardware_tests
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
    DEPENDS discovery_tests device_option_tests sensor_tests stream_configuration_tests
            stream_conversion_tests tx_write_tests timestamp_portable_tests
    USES_TERMINAL)
