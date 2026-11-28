cmake_minimum_required(VERSION 3.22)

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures")
endif()

project(GmSDeNoise VERSION 1.3.0 LANGUAGES C CXX)

# Windows (MSVC): UTF-8 sources, static C runtime (no Visual C++ redistributable
# needed on the user's PC), symbols kept in the build for crash tracing.
if(MSVC)
  add_compile_options(/utf-8 /bigobj)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ZD_BUILD_PLUGIN "Build the JUCE plugin (VST3)" ON)

# ---- DSP core (framework independent) ---------------------------------
# RNNoise (BSD-3, vendored; see third_party/rnnoise/README.md)
file(GLOB ZD_RNNOISE_SRC third_party/rnnoise/src/*.c)
add_library(zd_rnnoise STATIC ${ZD_RNNOISE_SRC})
target_include_directories(zd_rnnoise PUBLIC third_party/rnnoise/include)
set_target_properties(zd_rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(NOT MSVC)
  target_compile_options(zd_rnnoise PRIVATE -w)
  target_link_libraries(zd_rnnoise PUBLIC m)
endif()

# pffft for the analysis FFTs (BSD-like, vendored; ZD_USE_PFFFT=0 keeps the
# built-in radix-2 FFT of dsp/FFT.h).
option(ZD_USE_PFFFT "Use pffft for the analysis FFTs" ON)
add_library(zd_dsp STATIC dsp/ZeroDenoiser.cpp dsp/AiGains.cpp dsp/DfnRunner.cpp)
target_include_directories(zd_dsp PUBLIC dsp)
target_link_libraries(zd_dsp PUBLIC zd_rnnoise)
if(ZD_USE_PFFFT)
  add_library(zd_pffft STATIC third_party/pffft/pffft.c)
  target_include_directories(zd_pffft PUBLIC third_party/pffft)
  set_target_properties(zd_pffft PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(zd_pffft PRIVATE /O2 /fp:precise)
  else()
    target_compile_options(zd_pffft PRIVATE -O3 -w)
  endif()
  target_link_libraries(zd_dsp PUBLIC zd_pffft)
  target_compile_definitions(zd_dsp PUBLIC ZD_USE_PFFFT=1)
endif()
set_target_properties(zd_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# DeepFilterNet3 modes "AI Deep" / "AI Deep HQ" (Rust bridge, needs cargo).
# OFF: the two modes fall back to the RNNoise AI mode.
option(ZD_WITH_DFN "Build the DeepFilterNet3 modes (needs Rust/cargo)" ON)
if(ZD_WITH_DFN)
  include(cmake/DfBridge.cmake)
  target_link_libraries(zd_dsp PUBLIC zd_dfbridge)
  target_compile_definitions(zd_dsp PUBLIC ZD_HAS_DFN=1)
endif()

add_executable(zd_cli tools/zd_cli.cpp)
target_link_libraries(zd_cli PRIVATE zd_dsp)

enable_testing()
add_executable(zd_tests tests/tests.cpp)
target_link_libraries(zd_tests PRIVATE zd_dsp)
add_test(NAME zd_tests COMMAND zd_tests)

# ---- Plugin ----------------------------------------------------------------
if(ZD_BUILD_PLUGIN)
  include(FetchContent)
  # To use a local checkout: -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.10
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)

  # VST3 on every platform (the user asked for VST3 only on macOS too).
  set(ZD_FORMATS VST3)

  juce_add_plugin(GmSDeNoise
    # しずまる (formerly GmS De-Noise). Hosts see "Shizumaru": a Japanese plug-in
    # name is garbled in the VST3 class info (and the plug-in then fails to load
    # in some hosts), so the name hosts see is ASCII; しずまる is used in the
    # plug-in's own window, the installers and the docs. The codes stay, so
    # projects made with GmS De-Noise load it.
    PRODUCT_NAME "Shizumaru"
    COMPANY_NAME "GmS Audio"
    BUNDLE_ID "com.t2.gmsdenoise"
    PLUGIN_MANUFACTURER_CODE Utwo
    PLUGIN_CODE Ugdn
    DESCRIPTION "Zero-latency voice noise reduction"
    FORMATS ${ZD_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Restoration
    AU_MAIN_TYPE kAudioUnitType_Effect)

  target_sources(GmSDeNoise PRIVATE
    plugin/PluginProcessor.cpp
    plugin/PluginEditor.cpp)

  target_compile_definitions(GmSDeNoise PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

  target_link_libraries(GmSDeNoise
    PRIVATE zd_dsp juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

  # Host-side smoke test: loads the built VST3 like a DAW would.
  juce_add_console_app(zd_host_check PRODUCT_NAME "zd_host_check")
  target_sources(zd_host_check PRIVATE tools/host_check.cpp)
  target_compile_definitions(zd_host_check PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(zd_host_check PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)

  # Click / glitch stress test through the VST3 (block sizes, automation, mode/gate switches, extreme input).
  juce_add_console_app(zd_glitch_check PRODUCT_NAME "zd_glitch_check")
  target_sources(zd_glitch_check PRIVATE tools/glitch_check.cpp)
  target_compile_definitions(zd_glitch_check PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(zd_glitch_check PRIVATE juce::juce_audio_processors juce::juce_audio_formats juce::juce_recommended_config_flags)

  # Renders the editor to PNG (links the plugin's shared code)
  juce_add_console_app(zd_ui_snapshot PRODUCT_NAME "zd_ui_snapshot")
  target_sources(zd_ui_snapshot PRIVATE tools/ui_snapshot.cpp plugin/PluginProcessor.cpp plugin/PluginEditor.cpp)
  target_compile_definitions(zd_ui_snapshot PRIVATE JucePlugin_Name="Shizumaru" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(zd_ui_snapshot PRIVATE zd_dsp juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
endif()
