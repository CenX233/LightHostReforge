cmake_minimum_required(VERSION 3.22)
project(LightHostReforge VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(JUCE CONFIG REQUIRED)

#==============================================================================
# JUCE VST3PluginFormat patch (optional): accept "AudioEffect" category (WaveShell)
#==============================================================================
# WaveShell VST3 uses "AudioEffect" as its class category instead of the
# standard kVstAudioEffectClass ("Audio Module Class"). JUCE 8 only accepts
# the standard category, so WaveShell sub-plugins are invisible.
#
# This patch adds "AudioEffect" as an accepted category at all three filter
# locations in VST3PluginFormat.cpp. The REPLACE is idempotent — if the file
# is already patched, no changes.
#
# The path is auto-detected from JUCE_MODULES_DIR when available (set by
# JUCEConfig.cmake). Override with -DJUCE_VST3_FORMAT_SRC=<path> or set
# to empty to skip patching.
#==============================================================================

# Auto-detect from JUCE's module directory
if(JUCE_MODULES_DIR AND EXISTS "${JUCE_MODULES_DIR}/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp")
    set(_JUCE_VST3_DEFAULT "${JUCE_MODULES_DIR}/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp")
else()
    set(_JUCE_VST3_DEFAULT "")
endif()
set(JUCE_VST3_FORMAT_SRC "${_JUCE_VST3_DEFAULT}"
    CACHE FILEPATH "Full path to VST3PluginFormat.cpp for WaveShell patch (empty = skip)")

if(JUCE_VST3_FORMAT_SRC)
    if(NOT EXISTS "${JUCE_VST3_FORMAT_SRC}")
        message(WARNING "JUCE_VST3_FORMAT_SRC set to non-existent path: ${JUCE_VST3_FORMAT_SRC}. Skipping WaveShell patch.")
    else()
        file(READ "${JUCE_VST3_FORMAT_SRC}" JUCE_VST3_CONTENT)

        # Patch 1: moduleinfo.json fast path (c.category string comparison)
        string(REPLACE
            "if (c.category != kVstAudioEffectClass)"
            "if (c.category != kVstAudioEffectClass && c.category != \"AudioEffect\")"
            JUCE_VST3_PATCHED "${JUCE_VST3_CONTENT}")

        # Patch 2: DLL slow path + instance creation (std::strcmp — matches 2 locations)
        string(REPLACE
            "if (std::strcmp (info.category, kVstAudioEffectClass) != 0)"
            "if (std::strcmp (info.category, kVstAudioEffectClass) != 0 && std::strcmp (info.category, \"AudioEffect\") != 0)"
            JUCE_VST3_PATCHED "${JUCE_VST3_PATCHED}")

        if(NOT JUCE_VST3_CONTENT STREQUAL JUCE_VST3_PATCHED)
            message(STATUS "Patching VST3PluginFormat.cpp to accept \"AudioEffect\" category (WaveShell)")
            file(WRITE "${JUCE_VST3_FORMAT_SRC}" "${JUCE_VST3_PATCHED}")
        endif()
    endif()
else()
    message(STATUS "JUCE_VST3_FORMAT_SRC not found — skipping WaveShell VST3 patch")
endif()

#==============================================================================
# Application target
#==============================================================================
juce_add_gui_app(LightHost
    PRODUCT_NAME "Light Host"
    COMPANY_NAME "Rolando Islas"
    COMPANY_WEBSITE "https://www.rolandoislas.com"
    COMPANY_EMAIL "admin@rolandoislas.com"
    VERSION "1.2.0"
    ICON_BIG "Resources/icon.png"
    ICON_SMALL "Resources/icon.png"
)

#==============================================================================
# Feature configuration
#==============================================================================
target_compile_definitions(LightHost PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_ASIO=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LightHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LightHost,JUCE_VERSION>"
)

# ASIO SDK — locate iasiodrv.h for JUCE_ASIO=1
find_path(ASIOSDK_INCLUDE_DIR iasiodrv.h PATH_SUFFIXES asiosdk/common)
if(ASIOSDK_INCLUDE_DIR)
    target_include_directories(LightHost PRIVATE ${ASIOSDK_INCLUDE_DIR})
    message(STATUS "ASIO SDK found: ${ASIOSDK_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "ASIO SDK not found. "
        "Install via: vcpkg install asiosdk  "
        "Or set CMAKE_PREFIX_PATH to the SDK location")
endif()

# Audio Unit hosting — macOS only
if(APPLE)
    target_compile_definitions(LightHost PRIVATE JUCE_PLUGINHOST_AU=1)
else()
    target_compile_definitions(LightHost PRIVATE JUCE_PLUGINHOST_AU=0)
endif()

#==============================================================================
# Source files
#==============================================================================
target_sources(LightHost PRIVATE
    Source/HostStartup.cpp
    Source/IconMenu.cpp
    Source/AudioSettingsComponent.cpp
    Source/PluginChain.cpp
    Source/PluginWindow.cpp
    Source/PresetManager.cpp
)

#==============================================================================
# JUCE modules
#==============================================================================
target_link_libraries(LightHost PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_data_structures
)

#==============================================================================
# Recommended flags (optimisation, LTO, warnings — from JUCE CMake API)
#==============================================================================
target_link_libraries(LightHost PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

#==============================================================================
# Binary resources (icons used via BinaryData:: in runtime code)
#==============================================================================
juce_add_binary_data(LightHost_Resources
    NAMESPACE BinaryData
    SOURCES
        Resources/icon.png
        Resources/menu_icon.png
        Resources/menu_icon_white.png
)
target_link_libraries(LightHost PRIVATE LightHost_Resources)
