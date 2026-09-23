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
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined;-fno-omit-frame-pointer>)
    target_link_options(non_hardware_test_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined>)
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

add_custom_target(non_hardware_tests
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
    DEPENDS discovery_tests device_option_tests sensor_tests stream_configuration_tests
    USES_TERMINAL)
