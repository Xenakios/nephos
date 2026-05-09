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
    set(X86_COMPILE_OPTIONS
        -mavx2
        -mfma
        -Werror=return-type
        $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND X86_COMPILE_OPTIONS
            -Rpass=loop-vectorize
            -Rpass-missed=loop-vectorize
        )
    endif()

    target_compile_options(NephosCLI PRIVATE ${X86_COMPILE_OPTIONS})
    target_compile_options(AWLIMITED PRIVATE ${X86_COMPILE_OPTIONS})

    if(WIN32)
        # Clang on Windows — links against MSVC runtime, not libc++
        target_link_options(NephosCLI PRIVATE -fuse-ld=lld)
        target_link_options(AWLIMITED PRIVATE -fuse-ld=lld)
    elseif(APPLE)
        # Intel Mac — libc++ and clang already defaults, nothing extra needed
        message(STATUS "x86_64 macOS (Intel Mac) build")
    else()
        # Linux x86_64 — add any Linux-specific flags here if needed
    endif()

    message(STATUS "x86_64 build (${CMAKE_SYSTEM_NAME})")

elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(ARM64_COMPILE_OPTIONS
        -march=armv8-a+simd
        -Werror=return-type
        $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>
    )
    set(ARM64_LINK_OPTIONS)

    if(APPLE)
        # Apple Silicon — libc++ and clang are already defaults
        list(APPEND ARM64_COMPILE_OPTIONS -mcpu=apple-m1)
        message(STATUS "ARM64 macOS (Apple Silicon) build")
    elseif(UNIX)
        # Linux ARM64 (e.g. Raspberry Pi)
        list(APPEND ARM64_COMPILE_OPTIONS -stdlib=libc++)
        list(APPEND ARM64_LINK_OPTIONS -stdlib=libc++ -fuse-ld=lld-20)
        message(STATUS "ARM64 Linux build")
    endif()

    target_compile_options(NephosCLI PRIVATE ${ARM64_COMPILE_OPTIONS})
    target_compile_options(AWLIMITED PRIVATE ${ARM64_COMPILE_OPTIONS})
    target_link_options(NephosCLI    PRIVATE ${ARM64_LINK_OPTIONS})
    target_link_options(AWLIMITED    PRIVATE ${ARM64_LINK_OPTIONS})

    message(STATUS "ARM64 build (${CMAKE_SYSTEM_NAME})")

elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    message(WARNING "ARM 32-bit detected — this is not a supported configuration")

endif()
target_link_libraries(NephosCLI PRIVATE AWLIMITED fmt)
