rm -rf build && mkdir build && cd build

export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX11.3.sdk

cmake .. \
  -DCMAKE_C_COMPILER=/opt/local/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/local/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX11.3.sdk \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . --target NephosCLI