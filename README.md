# SoapySidekiq

SoapySDR support module for Epiq Solutions Sidekiq SDR devices.

## Minimum dependencies

- CMake 3.8 or newer
- A C++17-capable compiler
- SoapySDR development files, version 0.4.0 or newer
- Sidekiq SDK version 4.26.0 or newer

## Sidekiq SDK discovery

The build locates the Sidekiq SDK in this order:

1. `-DSIDEKIQ_SDK_DIR=/path/to/sdk`
2. `SIDEKIQ_SDK_DIR` from the environment
3. `$HOME/sidekiq_sdk_current`

The selected SDK must provide `bin/sidekiq-config` and be version 4.26.0 or
newer. The installer does not bundle an SDK.

## Build

Configure and build with CMake:

```bash
cmake -S . -B build
cmake --build build
```

Install the module with:

```bash
sudo cmake --install build
sudo ldconfig
```

The `install.sh` helper builds in `build/install-driver` without deleting other
build directories. Its optional argument is a CMake variable such as
`SIDEKIQ_SDK_DIR=/path/to/sdk`. Set `SOAPYSIDEKIQ_INSTALL_PREFIX` to install
without `sudo` into an isolated location:

```bash
SOAPYSIDEKIQ_INSTALL_PREFIX="$PWD/build/local-install" \
  ./install.sh SIDEKIQ_SDK_DIR=/path/to/sdk
SOAPY_SDR_ROOT="$PWD/build/local-install" SoapySDRUtil --info
```

An explicit prefix also works as `./install.sh CMAKE_INSTALL_PREFIX=/path/to/install`
(or `CMAKE_INSTALL_PREFIX:PATH=/path/to/install`). This argument overrides the
environment prefix. Both forms install without `sudo`; the chosen directory
must be writable by the caller. Without either prefix, the helper clears a
previous isolated prefix, installs into CMake's default system prefix with
`sudo`, and runs `ldconfig`. Set `SOAPYSIDEKIQ_BUILD_DIR` to use a different
build directory.

## Streaming limitations

The current streaming implementation supports:

- at most one active RX stream per device
- at most one active TX stream per device
- one requested channel per stream

Simultaneous RX and TX are supported. Multi-channel Soapy streams are not currently implemented.

## License

- https://github.com/pothosware/SoapySidekiq/blob/master/LICENSE
