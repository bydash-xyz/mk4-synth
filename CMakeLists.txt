cmake_minimum_required(VERSION 3.16)
project(mk4_synth LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Support macOS Homebrew search paths (both Apple Silicon /opt/homebrew and Intel /usr/local)
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")

# Include directories for SynthEngine and dependencies
set(SYNTH_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/MpkMiniMk4Display/include
    /opt/homebrew/include
    /usr/local/include
)

# Engine source files
set(SYNTH_ENGINE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/Oscillator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/Filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/Envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/Voice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/VoiceEngines.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/Wavetables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/SynthEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/MidiParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/Chorus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/SysExProtocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/src/SineLut.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/MpkMiniMk4Display/src/MpkMiniMk4Display.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/MpkMiniMk4Display/src/MpkMiniMk4ControlSurface.cpp
)

# Find PortAudio (optional for simulator)
find_path(PORTAUDIO_INCLUDE_DIR NAMES portaudio.h PATHS /opt/homebrew/include /usr/local/include)
find_library(PORTAUDIO_LIB NAMES portaudio PATHS /opt/homebrew/lib /usr/local/lib)

# Find RtMidi (optional for simulator & MPK display)
find_path(RTMIDI_INCLUDE_DIR NAMES rtmidi/RtMidi.h RtMidi.h PATHS /opt/homebrew/include /usr/local/include)
find_library(RTMIDI_LIB NAMES rtmidi PATHS /opt/homebrew/lib /usr/local/lib)

# -------------------------------------------------------------
# Standalone Library: MpkMiniMk4Display (Generic MK4 OLED & Control Surface)
# -------------------------------------------------------------
add_library(MpkMiniMk4Display STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/MpkMiniMk4Display/src/MpkMiniMk4Display.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/MpkMiniMk4Display/src/MpkMiniMk4ControlSurface.cpp
)

target_include_directories(MpkMiniMk4Display PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/MpkMiniMk4Display/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SynthEngine/include
)

if(RTMIDI_LIB)
    target_include_directories(MpkMiniMk4Display PUBLIC ${RTMIDI_INCLUDE_DIR})
    target_compile_definitions(MpkMiniMk4Display PUBLIC MPK_DISPLAY_WITH_RTMIDI=1)
    target_link_libraries(MpkMiniMk4Display PUBLIC ${RTMIDI_LIB})
    if(APPLE)
        target_link_libraries(MpkMiniMk4Display PUBLIC "-framework CoreMIDI" "-framework CoreFoundation")
    endif()
endif()

# -------------------------------------------------------------
# Unit Test: Core DSP Engine
# -------------------------------------------------------------
add_executable(synth_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_engine.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(synth_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
)

# -------------------------------------------------------------
# Unit Test: Knob defaults match preset values (--list prints mismatches)
# -------------------------------------------------------------
add_executable(knob_defaults_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_knob_defaults.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(knob_defaults_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
)

# -------------------------------------------------------------
# Unit Test: DSP Fidelity (authenticity regressions)
# -------------------------------------------------------------
add_executable(dsp_fidelity_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsp_fidelity.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(dsp_fidelity_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
)

# -------------------------------------------------------------
# Unit Test: Wavetable / Phase Distortion / Pluck / Additive engines
# -------------------------------------------------------------
add_executable(synth_engines_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_synth_engines.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(synth_engines_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
)

# -------------------------------------------------------------
# Unit Test: SysEx Protocol & NVS Persistence
# -------------------------------------------------------------
add_executable(sysex_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sysex.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(sysex_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_hal
)

# -------------------------------------------------------------
# Unit Test: ESP32-S3 Rev 2.0 Hardware Driver
# -------------------------------------------------------------
add_executable(esp32s3_driver_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_esp32s3_driver.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(esp32s3_driver_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_hal
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(esp32s3_driver_test PRIVATE
    MOCK_HAL=1
)

# -------------------------------------------------------------
# Unit Test: Rev 2.0 Peripherals (OLED, USB MPK, BLE/NVS)
# -------------------------------------------------------------
add_executable(hardware_driver_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hardware_drivers.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(hardware_driver_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_hal
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(hardware_driver_test PRIVATE
    MOCK_HAL=1
)

# -------------------------------------------------------------
# Unit Test: MIDI toy OLED animations (prototypes: https://bydash.xyz/mk4-synth/animations/)
# -------------------------------------------------------------
add_executable(toy_animation_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_toy_animations.cpp
)
target_include_directories(toy_animation_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_hal
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_definitions(toy_animation_test PRIVATE
    MOCK_HAL=1
)

# -------------------------------------------------------------
# Unit Test: MPK Mini MK4 Control Surface & Hardware Pre-Seeding
# -------------------------------------------------------------
add_executable(mpk_control_surface_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mpk_control_surface.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(mpk_control_surface_test PRIVATE
    ${SYNTH_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_hal
)
target_compile_definitions(mpk_control_surface_test PRIVATE
    MOCK_HAL=1
)

# -------------------------------------------------------------
# Unit Test: Simulator & Renderer Helpers (keyboard play mode, note spec, WAV, SMF)
# -------------------------------------------------------------
add_executable(sim_tools_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sim_tools.cpp
)
target_include_directories(sim_tools_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sim
)

# -------------------------------------------------------------
# Tool: Offline Renderer (preset + notes / MIDI file -> WAV, no audio/MIDI hardware)
# -------------------------------------------------------------
add_executable(synth_render
    ${CMAKE_CURRENT_SOURCE_DIR}/sim/synth_render.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(synth_render PRIVATE
    ${SYNTH_INCLUDE_DIRS}
)

# -------------------------------------------------------------
# Tool: Akai MPK Mini IV Display Tool (if RtMidi available)
# -------------------------------------------------------------

if(RTMIDI_LIB)
    add_executable(mpk_display
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/mpk_display.cpp
        ${SYNTH_ENGINE_SOURCES}
    )
    target_include_directories(mpk_display PRIVATE
        ${RTMIDI_INCLUDE_DIR}
        ${SYNTH_INCLUDE_DIRS}
    )
    target_link_libraries(mpk_display PRIVATE
        MpkMiniMk4Display
        ${RTMIDI_LIB}
    )
    if(APPLE)
        target_link_libraries(mpk_display PRIVATE
            "-framework CoreMIDI"
            "-framework CoreFoundation"
        )
    endif()
endif()

# -------------------------------------------------------------
# Desktop Simulator: PortAudio + RtMidi (if available)
# -------------------------------------------------------------
if(PORTAUDIO_LIB AND RTMIDI_LIB)
    add_executable(synth_sim
        ${CMAKE_CURRENT_SOURCE_DIR}/sim/main_sim.cpp
        ${SYNTH_ENGINE_SOURCES}
    )
    target_include_directories(synth_sim PRIVATE
        ${SYNTH_INCLUDE_DIRS}
        ${PORTAUDIO_INCLUDE_DIR}
        ${RTMIDI_INCLUDE_DIR}
    )
    target_compile_definitions(synth_sim PRIVATE
        MPK_DISPLAY_WITH_RTMIDI=1
    )
    target_link_libraries(synth_sim PRIVATE
        MpkMiniMk4Display
        ${PORTAUDIO_LIB}
        ${RTMIDI_LIB}
    )
    if(APPLE)
        target_link_libraries(synth_sim PRIVATE
            "-framework CoreAudio"
            "-framework CoreMIDI"
            "-framework CoreFoundation"
        )
    endif()
endif()

# -------------------------------------------------------------
# Tool: Manual Generator & Documentation Sync Constraint
# -------------------------------------------------------------
add_executable(manual_generator
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/manual_generator.cpp
    ${SYNTH_ENGINE_SOURCES}
)
target_include_directories(manual_generator PRIVATE
    ${SYNTH_INCLUDE_DIRS}
)

add_custom_target(generate_manual
    COMMAND manual_generator --write ${CMAKE_CURRENT_SOURCE_DIR}/docs/mpk_mini_mk4_manual.html
    DEPENDS manual_generator
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating and syncing Akai MPK Mini MK4 manual from Presets.h"
)

# -------------------------------------------------------------
# CTest Registration
# -------------------------------------------------------------
enable_testing()
add_test(NAME CoreDspTest COMMAND synth_test)
add_test(NAME DspFidelityTest COMMAND dsp_fidelity_test)
add_test(NAME SynthEnginesTest COMMAND synth_engines_test)
add_test(NAME KnobDefaultsTest COMMAND knob_defaults_test)
add_test(NAME SysExProtocolTest COMMAND sysex_test)
add_test(NAME Esp32S3DriverTest COMMAND esp32s3_driver_test)
add_test(NAME HardwareDriverTest COMMAND hardware_driver_test)
add_test(NAME ToyAnimationTest COMMAND toy_animation_test)
add_test(NAME MpkControlSurfaceTest COMMAND mpk_control_surface_test)
add_test(NAME SimToolsTest COMMAND sim_tools_test)
# Every factory preset: chord + low/high single notes, checked for NaN/inf, overs and silence
add_test(NAME SynthRenderSmokeTest COMMAND synth_render --preset all --check
    --notes "C3,E3,G3,C4:0.6 A1:0.3 C6:0.3" --tail 0.5)
# Switching presets must not jump the volume: chords then a single-note line at velocity 110 (how the MK4
# is usually played) stays within 1.5 LU across every factory preset. getModelLevelDb in Presets.h is
# calibrated on this phrase.
set(PresetLoudnessPhrase "C3,E3,G3:1 r:0.3 C4,E4,G4:0.5 r:0.2 G3,B3,D4:0.5 r:0.2 F3,A3,C4:1 A2:0.25 C3:0.25 E3:0.25 A3:0.25 E4:0.25 C4:0.25 A3:0.5")
add_test(NAME PresetLoudnessTest COMMAND synth_render --preset all --notes ${PresetLoudnessPhrase}
    --velocity 110 --tail 0.3 --max-spread 1.5)

add_test(NAME ManualSyncTest COMMAND manual_generator --verify ${CMAKE_CURRENT_SOURCE_DIR}/docs/mpk_mini_mk4_manual.html)
if(TARGET mpk_display)
    add_test(NAME MpkHardwareVerifyTest COMMAND mpk_display --verify-hw)
    set_tests_properties(MpkHardwareVerifyTest PROPERTIES LABELS hardware)
endif()
