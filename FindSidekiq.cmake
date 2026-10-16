# Resolve the selected SDK through sidekiq-config, preserving its static link
# order on the imported target. SDK-specific paths/flags stay off global state.
# Compatibility outputs: Sidekiq_INCLUDE_DIRS, Sidekiq_LIBRARIES,
# Sidekiq_PKG_LIBRARY_DIRS, Sidekiq_BUILD_CONFIG, and Sidekiq_VERSION.

if(DEFINED SIDEKIQ_SDK_DIR AND NOT "${SIDEKIQ_SDK_DIR}" STREQUAL "")
    set(_Sidekiq_hint "${SIDEKIQ_SDK_DIR}")
elseif(DEFINED ENV{SIDEKIQ_SDK_DIR} AND NOT "$ENV{SIDEKIQ_SDK_DIR}" STREQUAL "")
    set(_Sidekiq_hint "$ENV{SIDEKIQ_SDK_DIR}")
else()
    set(_Sidekiq_hint "$ENV{HOME}/sidekiq_sdk_current")
endif()
get_filename_component(_Sidekiq_hint "${_Sidekiq_hint}" ABSOLUTE)

# A cached public executable from a prior configure must not override a new
# SDK root. Resolve a fresh, uncached local variable each configure instead.
find_program(_Sidekiq_config_tool NAMES sidekiq-config
    PATHS "${_Sidekiq_hint}/bin" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
set(Sidekiq_CONFIG_EXECUTABLE "${_Sidekiq_config_tool}" CACHE FILEPATH
    "sidekiq-config resolved from the selected SDK root" FORCE)

function(_sidekiq_config output flag)
    execute_process(COMMAND "${Sidekiq_CONFIG_EXECUTABLE}" "${flag}"
        RESULT_VARIABLE status OUTPUT_VARIABLE value ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "${Sidekiq_CONFIG_EXECUTABLE} ${flag} failed: ${error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

# Clear outputs so a missing SDK cannot inherit a previous successful lookup.
set(Sidekiq_INCLUDE_DIRS "")
set(Sidekiq_COMPILE_OPTIONS "")
set(Sidekiq_LINK_LIBRARIES "")
set(Sidekiq_VERSION "")
set(Sidekiq_PKG_LIBRARY_DIRS "")
set(Sidekiq_BUILD_CONFIG "")
if(Sidekiq_CONFIG_EXECUTABLE)
    _sidekiq_config(Sidekiq_CFLAGS "--cflags")
    _sidekiq_config(Sidekiq_LINK_FLAGS "--libs-static")
    _sidekiq_config(Sidekiq_SDK_DIR "--prefix")
    _sidekiq_config(Sidekiq_PKG_LIBRARY_DIRS "--support-dir")
    _sidekiq_config(Sidekiq_BUILD_CONFIG "--build-config")
    _sidekiq_config(Sidekiq_VERSION "--version")
    separate_arguments(_Sidekiq_cflags UNIX_COMMAND "${Sidekiq_CFLAGS}")
    separate_arguments(Sidekiq_LINK_LIBRARIES UNIX_COMMAND "${Sidekiq_LINK_FLAGS}")

    while(_Sidekiq_cflags)
        list(POP_FRONT _Sidekiq_cflags flag)
        if(flag STREQUAL "-I" OR flag STREQUAL "-isystem")
            if(NOT _Sidekiq_cflags)
                message(FATAL_ERROR "sidekiq-config emitted ${flag} without a directory")
            endif()
            list(POP_FRONT _Sidekiq_cflags directory)
            list(APPEND Sidekiq_INCLUDE_DIRS "${directory}")
        elseif(flag MATCHES "^-I(.+)")
            list(APPEND Sidekiq_INCLUDE_DIRS "${CMAKE_MATCH_1}")
        else()
            list(APPEND Sidekiq_COMPILE_OPTIONS "${flag}")
        endif()
    endwhile()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Sidekiq
    REQUIRED_VARS Sidekiq_CONFIG_EXECUTABLE Sidekiq_INCLUDE_DIRS Sidekiq_LINK_LIBRARIES
    VERSION_VAR Sidekiq_VERSION)

if(Sidekiq_FOUND)
    if(NOT TARGET Sidekiq::sidekiq)
        add_library(Sidekiq::sidekiq INTERFACE IMPORTED)
    endif()
    # Keep the SDK's ordered static link flags together: moving group or
    # whole-archive switches to a different link-line position changes meaning.
    set_target_properties(Sidekiq::sidekiq PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Sidekiq_INCLUDE_DIRS}"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${Sidekiq_INCLUDE_DIRS}"
        INTERFACE_COMPILE_OPTIONS "${Sidekiq_COMPILE_OPTIONS}"
        INTERFACE_LINK_LIBRARIES "${Sidekiq_LINK_LIBRARIES}")
    set(Sidekiq_LIBRARIES Sidekiq::sidekiq)
else()
    set(Sidekiq_LIBRARIES "")
endif()
mark_as_advanced(Sidekiq_CONFIG_EXECUTABLE)
