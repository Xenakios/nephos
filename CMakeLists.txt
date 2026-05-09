cmake_minimum_required(VERSION 4.3)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(AudioPluginHost2 VERSION 0.0.1)
set(JUCE_AVAILABLE False)
if (JUCE_AVAILABLE)
add_subdirectory(JUCE)
endif()

include_directories(
    libs/clap/include
    libs/clap-helpers/include
    libs/choc/choc
    libs/sst-basic-blocks/include
    libs/fmt/include

        
    libs/sst-filters/include
    # libs/sst-effects/include
    # libs/sst-waveshapers
    # libs/oscpkt
    libs/simde
    # libs/thread-pool/include
    
    libs/sst-basic-blocks/libs
    libs/airwindows_limited
    libs/airwindows_limited/plugins
)

add_subdirectory(libs/fmt)

add_library(AWLIMITED
    libs/airwindows_limited/airwin_consolidated_base.cpp
    libs/airwindows_limited/plugins/BezEQ.cpp
    libs/airwindows_limited/plugins/BezEQProc.cpp
    libs/airwindows_limited/plugins/HipCrush.cpp
    libs/airwindows_limited/plugins/HipCrushProc.cpp
    libs/airwindows_limited/plugins/kWoodRoom.cpp
    libs/airwindows_limited/plugins/kWoodRoomProc.cpp
    libs/airwindows_limited/plugins/RingModulator.cpp
    libs/airwindows_limited/plugins/RingModulatorProc.cpp
    libs/airwindows_limited/plugins/PrimeFIR.cpp
    libs/airwindows_limited/plugins/PrimeFIRProc.cpp
    libs/airwindows_limited/plugins/Hypersoft.cpp
    libs/airwindows_limited/plugins/HypersoftProc.cpp
    libs/airwindows_limited/plugins/DeRez3.cpp
    libs/airwindows_limited/plugins/DeRez3Proc.cpp
    libs/airwindows_limited/plugins/CrunchCoat.cpp
    libs/airwindows_limited/plugins/CrunchCoatProc.cpp
    # libs/airwindows_limited/plugins/Galactic3Ambisonic.cpp
    # libs/airwindows_limited/plugins/Galactic3AmbisonicProc.cpp
    libs/airwindows_limited/plugins/BitGlitter.cpp
    libs/airwindows_limited/plugins/BitGlitterProc.cpp
    libs/airwindows_limited/plugins/ToTape9.cpp
    libs/airwindows_limited/plugins/ToTape9Proc.cpp
    libs/airwindows_limited/plugins/Donut.cpp
    libs/airwindows_limited/plugins/DonutProc.cpp
    libs/airwindows_limited/plugins/GlitchShifter.cpp
    libs/airwindows_limited/plugins/GlitchShifterProc.cpp
    libs/airwindows_limited/plugins/WoodenBoxProc.cpp
    libs/airwindows_limited/plugins/WoodenBox.cpp
    libs/airwindows_limited/plugins/PitchNasty.cpp
    libs/airwindows_limited/plugins/PitchNastyProc.cpp
)

target_compile_definitions(AWLIMITED PUBLIC _USE_MATH_DEFINES=1)

add_executable(NephosCLI
    src/nephosmain.cpp
    src/easing.cpp
    src/grainfx.cpp
)
target_compile_definitions(NephosCLI PUBLIC _USE_MATH_DEFINES=1)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(NephosCLI PRIVATE -mavx2 -mfma -Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Werror=return-type "$<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    target_compile_options(NephosCLI PRIVATE -stdlib=libc++)
    target_compile_options(AWLIMITED PRIVATE -stdlib=libc++)
    target_link_options(NephosCLI PRIVATE -stdlib=libc++ -fuse-ld=lld-20)
    target_link_options(AWLIMITED PRIVATE -stdlib=libc++ -fuse-ld=lld-20)
    message("ARM64")
    message(STATUS "Compile options: ${CMAKE_CXX_FLAGS}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    message("ARM 32-bit, should not be getting this message")
endif()

target_link_libraries(NephosCLI PRIVATE AWLIMITED fmt)
