# Linux AArch64 cross-build

Run these commands from the SoapySidekiq repository root on an x86 Linux host.
Use a SoapySDR source checkout and a Sidekiq SDK installation containing the
`aarch64` build configuration and its target support libraries. The target
SoapySDR library must be built for AArch64 too.

On Ubuntu 22.04, install the GNU cross compiler:

```bash
sudo apt-get install g++-aarch64-linux-gnu
```

Set the paths for your source checkout, SDK, and isolated target prefix:

```bash
SOAPYSIDEKIQ_SOAPY_SOURCE=/path/to/SoapySDR
SOAPYSIDEKIQ_ARM_SDK="$HOME/sidekiq_sdk_current"
SOAPYSIDEKIQ_ARM_PREFIX="$PWD/build/aarch64-prefix"
```

## Build target SoapySDR

This builds the library and development files. Optional bindings, applications,
tests, and documentation are disabled for this compile/link check.

```bash
cmake -S "$SOAPYSIDEKIQ_SOAPY_SOURCE" -B build/aarch64-soapy \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/aarch64-linux-gnu.cmake" \
  -DCMAKE_INSTALL_PREFIX="$SOAPYSIDEKIQ_ARM_PREFIX" \
  -DENABLE_PYTHON2=OFF -DENABLE_PYTHON3=OFF \
  -DENABLE_LUAJIT=OFF -DENABLE_CSHARP=OFF -DENABLE_DOCS=OFF \
  -DENABLE_APPS=OFF -DENABLE_TESTS=OFF
cmake --build build/aarch64-soapy --parallel 4
cmake --install build/aarch64-soapy
```

## Build the module with target SDK libraries

`sidekiq-config` runs on the host. Set its build configuration explicitly so it
emits AArch64 SDK/support library paths. Retain that setting for builds and
installs too, since CMake can automatically reconfigure during those steps.

```bash
SIDEKIQ_CONFIG_BUILD_CONFIG=aarch64 \
  cmake -S . -B build/aarch64-module \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/aarch64-linux-gnu.cmake" \
  -DSoapySDR_DIR="$SOAPYSIDEKIQ_ARM_PREFIX/share/cmake/SoapySDR" \
  -DCMAKE_INSTALL_PREFIX="$SOAPYSIDEKIQ_ARM_PREFIX" \
  -DSIDEKIQ_SDK_DIR="$SOAPYSIDEKIQ_ARM_SDK" \
  -DCMAKE_BUILD_TYPE=Release
SIDEKIQ_CONFIG_BUILD_CONFIG=aarch64 \
  cmake --build build/aarch64-module --parallel 4
SIDEKIQ_CONFIG_BUILD_CONFIG=aarch64 \
  cmake --install build/aarch64-module
```

Check that configure reports `Sidekiq_BUILD_CONFIG - aarch64`. The SDK link
flags must select `libsidekiq__aarch64.a` and the AArch64 support directory.
The GNU module link uses `--no-undefined` to require resolved symbols.

## Verify artifacts

```bash
aarch64-linux-gnu-readelf -h build/aarch64-module/libSidekiqSupport.so
aarch64-linux-gnu-readelf -h "$SOAPYSIDEKIQ_ARM_PREFIX"/lib/libSoapySDR.so
```

Both headers must report `ELF64` and `Machine: AArch64`. CMake installs the
module beneath the target prefix's `lib/SoapySDR/modules<ABI>/` directory.
Keep configure/build/install logs and the module's `CMakeFiles/SidekiqSupport.dir/link.txt`
with the validation evidence.

A build/link/install check does not execute the target library or validate
hardware behavior. Run module discovery and the applicable hardware validators
on the target separately.

## Recorded validation

On 2026-10-20, cross GCC 11.4.0 on Ubuntu 22.04 built SoapySDR 0.8.1 and the
full SoapySidekiq module at commit `270dc4c` against SDK 4.26.0's AArch64
archive and support libraries. Linking with `--no-undefined`, isolated
installation, and ELF architecture checks passed. Target execution was not
performed. New production changes should repeat this check.
