rm -rf build && mkdir build && cd build

export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX11.3.sdk

cmake .. \
  -DCMAKE_C_COMPILER=/opt/local/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/local/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX11.3.sdk \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . --target NephosPlugin_VST3
# clone into libs : choc, clap, clap-helpers, fmt, juce, simde, sst-basic-blocks, sst-cpputils
# sst-filters, git clone https://github.com/surge-synthesizer/tuning-library.git
# JUCE commit 91ad83ae34a81e0833b1a2b0866f54846370ae53
#  https://github.com/Xenakios/sst-basic-blocks.git
# fmt commit b5c4e795bba50793eee7e4ccce44a48c3310adca
