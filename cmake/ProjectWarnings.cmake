# Shared first-party warnings for the module and standalone non-hardware tests.
# Keep warnings diagnostic until the inherited production warning debt is fixed.
function(soapy_sidekiq_enable_warnings target scope)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} ${scope}
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
    endif()
endfunction()
