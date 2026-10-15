# Non-hardware test build: compiles portable driver logic without the Sidekiq SDK.
cmake_minimum_required(VERSION 3.16)
project(SoapySidekiqNonHardwareTests LANGUAGES CXX)

include(CTest)
# RxSampleQueue tests exercise real producer/consumer wake-ups with std::thread.
find_package(Threads REQUIRED)

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
    unit/SampleScaleTests.cpp
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

# Keep the queue test independent of hardware and the proprietary Sidekiq runtime.
add_executable(rx_sample_queue_tests
    support/TestMain.cpp
    unit/RxSampleQueueTests.cpp
    ../RxSampleQueue.cpp)
target_include_directories(rx_sample_queue_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(rx_sample_queue_tests PRIVATE
    non_hardware_test_options Threads::Threads)

add_test(NAME rx-sample-queue.all COMMAND rx_sample_queue_tests)
set_tests_properties(rx-sample-queue.all PROPERTIES LABELS "unit;non-hardware")

# Exercise the complete RX worker lifecycle through a blocking fake backend.
add_executable(rx_stream_session_tests
    support/TestMain.cpp
    component/RxStreamSessionTests.cpp
    ../RxSampleQueue.cpp
    ../RxStreamSession.cpp)
target_include_directories(rx_stream_session_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(rx_stream_session_tests PRIVATE
    non_hardware_test_options Threads::Threads)

add_test(NAME rx-stream-session.all COMMAND rx_stream_session_tests)
set_tests_properties(rx-stream-session.all PROPERTIES LABELS "component;non-hardware")
add_executable(rx_sample_queue_portable_tests
    support/TestMain.cpp unit/RxSampleQueueTests.cpp ../RxSampleQueue.cpp)
target_include_directories(rx_sample_queue_portable_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(rx_sample_queue_portable_tests PRIVATE
    SOAPYSIDEKIQ_FORCE_PORTABLE_TIMESTAMP=1)
target_link_libraries(rx_sample_queue_portable_tests PRIVATE
    non_hardware_test_options Threads::Threads)
add_test(NAME rx-sample-queue-portable.all COMMAND rx_sample_queue_portable_tests)
set_tests_properties(rx-sample-queue-portable.all PROPERTIES LABELS "unit;non-hardware")
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

string(FIND "${streaming_source}" "std::string SoapySidekiq::getNativeStreamFormat(" native_start)
string(FIND "${streaming_source}" "SoapySDR::ArgInfoList SoapySidekiq::getStreamArgsInfo(" native_end)
if(native_start LESS 0 OR native_end LESS native_start)
    message(FATAL_ERROR "Could not locate production native stream format method")
endif()
math(EXPR native_length "${native_end} - ${native_start}")
string(SUBSTRING "${streaming_source}" ${native_start} ${native_length} native_method)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/NativeStreamFormat.inc" "${native_method}")

find_package(Threads REQUIRED)
add_executable(tx_write_tests support/TestMain.cpp component/TxWriteStreamTests.cpp)
target_include_directories(tx_write_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(tx_write_tests PRIVATE non_hardware_test_options Threads::Threads)
add_test(NAME tx-write.all COMMAND tx_write_tests)
set_tests_properties(tx-write.all PROPERTIES LABELS "component;non-hardware")

# Reuse production lifecycle methods with a fake SDK, as in the TX write fixture.
function(extract_stream_method output start_marker end_marker)
    string(FIND "${streaming_source}" "${start_marker}" method_start)
    string(FIND "${streaming_source}" "${end_marker}" method_end)
    if(method_start LESS 0 OR method_end LESS method_start)
        message(FATAL_ERROR "Could not locate lifecycle method: ${start_marker}")
    endif()
    math(EXPR method_length "${method_end} - ${method_start}")
    string(SUBSTRING "${streaming_source}" ${method_start} ${method_length} method)
    set(${output} "${method}" PARENT_SCOPE)
endfunction()
extract_stream_method(rx_close "void SoapySidekiq::closeStream("
    "size_t SoapySidekiq::getStreamMTU(")
extract_stream_method(rx_deactivate "int SoapySidekiq::deactivateStream("
    "int SoapySidekiq::readStream(")
extract_stream_method(activate_method "int SoapySidekiq::activateStream("
    "int SoapySidekiq::deactivateStream(")
string(FIND "${activate_method}" "    else if (stream_handle->direction == SOAPY_SDR_TX)" tx_branch)
if(tx_branch LESS 0)
    message(FATAL_ERROR "Could not locate RX activation branch")
endif()
string(SUBSTRING "${activate_method}" 0 ${tx_branch} rx_activate)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/RxLifecycleMethods.inc"
    "${rx_close}\n${rx_deactivate}\n${rx_activate}\n    return 0;\n}\n")
add_executable(rx_lifecycle_tests
    support/TestMain.cpp component/RxLifecycleTests.cpp ../RxSampleQueue.cpp ../RxStreamSession.cpp)
target_include_directories(rx_lifecycle_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(rx_lifecycle_tests PRIVATE non_hardware_test_options Threads::Threads)
add_test(NAME rx-lifecycle.all COMMAND rx_lifecycle_tests)
set_tests_properties(rx-lifecycle.all PROPERTIES LABELS "component;non-hardware")

add_custom_target(non_hardware_tests
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
    DEPENDS discovery_tests device_option_tests sensor_tests stream_configuration_tests
            stream_conversion_tests rx_sample_queue_tests rx_sample_queue_portable_tests
            rx_stream_session_tests tx_write_tests timestamp_portable_tests rx_lifecycle_tests
    USES_TERMINAL)
