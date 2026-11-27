cmake_minimum_required(VERSION 3.20)
project(ScarlettScout VERSION 0.0.8 LANGUAGES C CXX RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the GUI, worker, installer, and package on one version source.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp"
    @ONLY
)

if(NOT WIN32)
    message(FATAL_ERROR "Scarlett Scout is a native Windows application. Cross-compile with the supplied WSL toolchain.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio.h")
    message(FATAL_ERROR "miniaudio.h is missing. Run ./install.sh to download the pinned dependency.")
endif()

add_executable(ScarlettScout WIN32
    src/main.cpp
    src/audio_engine.cpp
    src/dsp.cpp
    src/interesting_detector.cpp
    src/wav.cpp
    src/miniaudio_impl.c
)

target_include_directories(ScarlettScout PRIVATE include vendor "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(ScarlettScout PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ScarlettScout PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wno-unknown-pragmas>
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unknown-pragmas>
)
target_link_libraries(ScarlettScout PRIVATE
    comctl32 comdlg32 shell32 shlwapi ole32 uuid winmm avrt user32 gdi32
)

# A separate native worker process hosts OpenVINO through its stable C API.
# Runtime DLLs are deployed beside the app; there is no Python or OpenVINO
# import-library dependency in this MinGW cross-build.
add_executable(ScarlettScoutNpuWorker src/npu_worker.cpp)
target_include_directories(ScarlettScoutNpuWorker PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(ScarlettScoutNpuWorker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ScarlettScoutNpuWorker PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(ScarlettScoutNpuWorker PRIVATE shell32)
set_target_properties(ScarlettScoutNpuWorker PROPERTIES
    OUTPUT_NAME "ScarlettScoutNpuWorker"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MINGW)
    target_link_options(ScarlettScout PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--dynamicbase -Wl,--nxcompat)
    target_link_options(ScarlettScoutNpuWorker PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--dynamicbase -Wl,--nxcompat)
endif()

set_target_properties(ScarlettScout PROPERTIES
    OUTPUT_NAME "ScarlettScout"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(ScarlettScoutDspTests tests/dsp_tests.cpp src/dsp.cpp)
    target_include_directories(ScarlettScoutDspTests PRIVATE include)
    add_test(NAME dsp_tests COMMAND ScarlettScoutDspTests)
    add_executable(ScarlettScoutWavTests tests/wav_tests.cpp src/wav.cpp)
    target_include_directories(ScarlettScoutWavTests PRIVATE include)
    add_test(NAME wav_tests COMMAND ScarlettScoutWavTests)

    add_executable(ScarlettScoutInterestingDetectorTests
        tests/interesting_detector_tests.cpp
        src/interesting_detector.cpp
    )
    target_include_directories(ScarlettScoutInterestingDetectorTests PRIVATE include)
    add_test(NAME interesting_detector_tests COMMAND ScarlettScoutInterestingDetectorTests)
endif()
