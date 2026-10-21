# Compile the concrete driver, using installed SDK headers but no SDK library.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The native scripted-SDK integration test requires Linux")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Threads REQUIRED)
option(SOAPYSIDEKIQ_SDK_INTEGRATION_ASAN "Instrument native scripted-SDK tests with ASan/UBSan" OFF)
option(SOAPYSIDEKIQ_SDK_INTEGRATION_TSAN "Instrument native scripted-SDK tests with TSan" OFF)
if(SOAPYSIDEKIQ_SDK_INTEGRATION_ASAN AND SOAPYSIDEKIQ_SDK_INTEGRATION_TSAN)
    message(FATAL_ERROR "Native ASan/UBSan and TSan require separate build directories")
endif()

get_target_property(driver_sources SidekiqSupport SOURCES)
get_target_property(driver_source_dir SidekiqSupport SOURCE_DIR)
set(native_sources)
foreach(source IN LISTS driver_sources)
    # The test includes Registration.cpp directly to call its real discovery/factory
    # functions without invoking SoapySDR's automatic external plugin loader.
    if(source MATCHES "(^|/)Registration\\.cpp$")
        continue()
    endif()
    if(NOT IS_ABSOLUTE "${source}")
        get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${driver_source_dir}")
    endif()
    list(APPEND native_sources "${source}")
endforeach()

add_library(scripted_driver_objects OBJECT ${native_sources}
    FakeSdk.cpp DriverIntegrationTests.cpp ../non_hardware/support/TestMain.cpp)
target_compile_features(scripted_driver_objects PRIVATE cxx_std_17)
set_target_properties(scripted_driver_objects PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(scripted_driver_objects PRIVATE
    "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/tests/non_hardware/support")
target_include_directories(scripted_driver_objects SYSTEM PRIVATE
    ${Sidekiq_INCLUDE_DIRS} ${SoapySDR_INCLUDE_DIRS})
soapy_sidekiq_enable_warnings(scripted_driver_objects PRIVATE)

set(fallback_source "${CMAKE_CURRENT_BINARY_DIR}/UnexpectedSdkCalls.cpp")
add_custom_command(OUTPUT "${fallback_source}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_stubs.py"
        --nm "${CMAKE_NM}" --output "${fallback_source}"
        --sdk-headers ${Sidekiq_INCLUDE_DIRS}
        --objects $<TARGET_OBJECTS:scripted_driver_objects>
    DEPENDS $<TARGET_OBJECTS:scripted_driver_objects>
        "${CMAKE_CURRENT_SOURCE_DIR}/generate_stubs.py"
    COMMAND_EXPAND_LISTS VERBATIM)
add_executable(scripted_driver_tests
    $<TARGET_OBJECTS:scripted_driver_objects> "${fallback_source}")
target_compile_features(scripted_driver_tests PRIVATE cxx_std_17)
set_target_properties(scripted_driver_tests PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(scripted_driver_tests SYSTEM PRIVATE ${Sidekiq_INCLUDE_DIRS})
# Do not link Sidekiq::sidekiq: every SDK symbol is scripted or a fail-fast stub.
target_link_libraries(scripted_driver_tests PRIVATE ${SoapySDR_LIBRARIES} Threads::Threads)
if(SOAPYSIDEKIQ_SDK_INTEGRATION_ASAN)
    target_compile_options(scripted_driver_objects PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    target_compile_options(scripted_driver_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    target_link_options(scripted_driver_tests PRIVATE -fsanitize=address,undefined -no-pie)
endif()
if(SOAPYSIDEKIQ_SDK_INTEGRATION_TSAN)
    target_compile_options(scripted_driver_objects PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_compile_options(scripted_driver_tests PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(scripted_driver_tests PRIVATE -fsanitize=thread)
endif()
add_test(NAME scripted-driver.all COMMAND scripted_driver_tests)
set_tests_properties(scripted-driver.all PROPERTIES LABELS "integration;non-hardware;sdk-headers"
    TIMEOUT 30)
