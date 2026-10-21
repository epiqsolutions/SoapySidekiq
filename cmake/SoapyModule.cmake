# Preserve relocatable install rules with the SoapySDR 0.4.0/0.4.1 helper.
# Those releases default to an absolute destination beside libSoapySDR,
# ignoring the caller's configured prefix. A relative PREFIX keeps DESTDIR
# and cmake --install --prefix effective. Newer helpers already do this and
# must retain their normal ABI directory and GNUInstallDirs behavior.
function(soapy_sidekiq_add_module)
    if(SoapySDR_VERSION VERSION_LESS "0.4.2")
        SOAPY_SDR_MODULE_UTIL(PREFIX "." ${ARGN})
    else()
        SOAPY_SDR_MODULE_UTIL(${ARGN})
    endif()
endfunction()
