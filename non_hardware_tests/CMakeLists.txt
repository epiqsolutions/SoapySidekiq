cmake_minimum_required(VERSION 3.16)
project(SoapySidekiqNonHardwareTests LANGUAGES CXX)

include(CTest)
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

add_executable(unit_tests
    support/TestMain.cpp
    unit/AsyncBufferPoolTests.cpp
    unit/DeviceConfigurationTests.cpp
    unit/SampleConversionTests.cpp
    unit/TimestampConversionTests.cpp
    ../DeviceOptions.cpp)
target_include_directories(unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(unit_tests PRIVATE non_hardware_test_options Threads::Threads)
add_test(NAME unit.all COMMAND unit_tests)
set_tests_properties(unit.all PROPERTIES LABELS "unit;non-hardware")

add_executable(component_tests
    support/TestMain.cpp
    component/DeviceDiscoveryTests.cpp
    component/FakeAsyncTransmitterTests.cpp
    component/FakeSidekiqDeviceTests.cpp
    ../DeviceDiscovery.cpp)
target_include_directories(component_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(component_tests PRIVATE non_hardware_test_options Threads::Threads)
add_test(NAME component.all COMMAND component_tests)
set_tests_properties(component.all PROPERTIES LABELS "component;non-hardware")

add_custom_target(non_hardware_tests
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
    DEPENDS unit_tests component_tests
    USES_TERMINAL)
