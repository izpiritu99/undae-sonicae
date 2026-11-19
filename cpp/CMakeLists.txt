# Undae Sonicae, native edition (C++ / JUCE). Build:
#   cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build cpp/build
cmake_minimum_required(VERSION 3.22)
# macOS: runs on 11 (Big Sur) and later; cpp/build-mac.sh builds one universal app (Apple Silicon + Intel).
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS the app runs on")
project(UndaeSonicae VERSION 0.3.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_definitions(_USE_MATH_DEFINES)   # M_PI and friends
  add_compile_options(/utf-8)                  # the sources are UTF-8
endif()

add_subdirectory(third_party/JUCE EXCLUDE_FROM_ALL)

# The engine: no user interface, shared by the app and the command-line renderer.
add_library(us_engine STATIC
  src/engine/Dsp.cpp src/engine/Project.cpp src/engine/Samples.cpp src/engine/Voices.cpp src/engine/Effects.cpp src/engine/Engine.cpp src/engine/Analysis.cpp src/engine/SoundDesign.cpp src/engine/Mp3.cpp src/engine/PluginHost.cpp)
target_include_directories(us_engine PUBLIC src)
target_link_libraries(us_engine
  PUBLIC juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp
  PRIVATE juce::juce_recommended_config_flags)
if(MSVC)
  target_compile_options(us_engine PRIVATE /W3)
  target_compile_definitions(us_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(us_engine PUBLIC ws2_32 psapi)
else()
  target_compile_options(us_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
target_link_libraries(us_engine PUBLIC ${CMAKE_DL_LIBS})
target_compile_definitions(us_engine PUBLIC JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1)
set_target_properties(us_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
  target_compile_options(us_engine PRIVATE -O3)
endif()

juce_add_console_app(undae-render PRODUCT_NAME "undae-render")
target_sources(undae-render PRIVATE src/render/RenderMain.cpp)
target_link_libraries(undae-render PRIVATE us_engine juce::juce_recommended_config_flags)

set(US_APPLE_OPTS)
if(APPLE)   # macOS asks before any app may use the microphone or an audio interface's inputs; without this text it refuses
  set(US_APPLE_OPTS
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Undae Sonicae records from your microphone or audio interface when you arm recording.")
  # No hardened runtime: the app is signed ad hoc and not notarised, and the runtime's library validation would stop
  # the plug-in host from loading other companies' plug-ins. Turn it on (with entitlements) only for notarised releases.
endif()
juce_add_gui_app(UndaeSonicae
  PRODUCT_NAME "Undae Sonicae"
  COMPANY_NAME "Undae Sonicae"
  BUNDLE_ID "app.undaesonicae.native"
  ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../app/icons/icon-512.png"
  ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/../app/icons/icon-64.png"
  ${US_APPLE_OPTS})
target_sources(UndaeSonicae PRIVATE src/app/AppMain.cpp src/app/Doc.cpp)
target_include_directories(UndaeSonicae PRIVATE src/app)
target_link_libraries(UndaeSonicae PRIVATE us_engine juce::juce_gui_extra juce::juce_audio_utils juce::juce_audio_devices
  juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
target_compile_definitions(UndaeSonicae PRIVATE JUCE_APPLICATION_NAME_STRING="Undae Sonicae" JUCE_APPLICATION_VERSION_STRING="0.3.0")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(UndaeSonicae PRIVATE JUCE_JACK=1 JUCE_ALSA=1)
endif()

# The plug-in host built on JUCE (src/pluginhost): Audio Units, VST3 and VST2. The macOS app ships it in
# Contents/Helpers; elsewhere it is built for testing (UNDAE_PLUGIN_HOST=juce makes the app use it). No Dock icon on macOS.
juce_add_gui_app(undae-pluginhost
  PRODUCT_NAME "undae-pluginhost"
  COMPANY_NAME "Undae Sonicae"
  BUNDLE_ID "app.undaesonicae.pluginhost"
  PLIST_TO_MERGE "<plist><dict><key>LSUIElement</key><true/></dict></plist>")
target_sources(undae-pluginhost PRIVATE src/pluginhost/HostMain.cpp)
target_compile_definitions(undae-pluginhost PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
  JUCE_STANDALONE_APPLICATION=1 JUCE_APPLICATION_NAME_STRING="undae-pluginhost" JUCE_APPLICATION_VERSION_STRING="1")
if(APPLE)
  target_compile_definitions(undae-pluginhost PRIVATE JUCE_PLUGINHOST_AU=1)
endif()
target_link_libraries(undae-pluginhost PRIVATE juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags)

juce_add_console_app(undae-compare PRODUCT_NAME "undae-compare")
target_sources(undae-compare PRIVATE src/render/CompareMain.cpp)
target_link_libraries(undae-compare PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_recommended_config_flags)
target_compile_definitions(undae-compare PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1 JUCE_USE_MP3AUDIOFORMAT=1)

juce_add_console_app(undae-selftest PRODUCT_NAME "undae-selftest")
target_sources(undae-selftest PRIVATE src/render/SelfTest.cpp src/app/Doc.cpp)
target_include_directories(undae-selftest PRIVATE src src/app)
target_link_libraries(undae-selftest PRIVATE us_engine juce::juce_events juce::juce_gui_basics juce::juce_recommended_config_flags)

juce_add_console_app(undae-plugtest PRODUCT_NAME "undae-plugtest")
target_sources(undae-plugtest PRIVATE src/render/PlugTest.cpp)
target_include_directories(undae-plugtest PRIVATE src)
target_link_libraries(undae-plugtest PRIVATE us_engine juce::juce_events juce::juce_gui_basics juce::juce_recommended_config_flags)
