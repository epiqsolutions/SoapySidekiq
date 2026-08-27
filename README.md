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

## License

- https://github.com/pothosware/SoapySidekiq/blob/master/LICENSE
