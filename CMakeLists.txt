cmake_minimum_required(VERSION 3.22)
project(KR106 VERSION 2.5.8)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")

add_subdirectory(libs/JUCE)
add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)

set(KR106_FORMATS AU VST3 LV2 Standalone)
option(KR106_COPY_AFTER_BUILD "Copy plugins to system directories after build" ON)

juce_add_plugin(KR106
    COMPANY_NAME "Kayrock"
    COMPANY_WEBSITE "https://kayrock.org/kr106"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    PLUGIN_MANUFACTURER_CODE Krok
    PLUGIN_CODE Kr16
    PLUGIN_DESCRIPTION "Juno-6/60/106 emulation v${CMAKE_PROJECT_VERSION}"
    FORMATS ${KR106_FORMATS}
    PRODUCT_NAME "Ultramaster KR-106"
    PLUGIN_NAME "Ultramaster KR-106"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/img/icon_1024.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/img/icon_1024.png"
    LV2URI "https://kayrock.org/kr106"
    COPY_PLUGIN_AFTER_BUILD ${KR106_COPY_AFTER_BUILD}
)

juce_add_binary_data(KR106BinaryData SOURCES
    resources/img/kr106_background@2x.png
    resources/img/knob@2x.png
    resources/img/smallknob@2x.png
    resources/img/switch_3way@2x.png
    resources/img/switch_3way_horizontal@2x.png
    resources/img/led_red@2x.png
    resources/img/kr106_bender_gradient@2x.png
    resources/img/transpose_chevron@2x.png
    resources/img/new_knob@2x.png
    resources/img/slider_handle@2x.png
    resources/fonts/Segment14.otf
)

target_sources(KR106 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.h
    Source/KR106PresetManager.h
    Source/KR106_Presets_JUCE.h
    Source/Controls/KR106Bender.h
    Source/Controls/KR106Button.h
    Source/Controls/KR106Keyboard.h
    Source/Controls/KR106MenuSheet.h
    Source/Controls/KR106Knob.h
    Source/Controls/KR106Misc.h
    Source/Controls/KR106PresetDisplay.h
    Source/Controls/KR106Scope.h
    Source/Controls/KR106Slider.h
    Source/Controls/KR106Switch.h
    Source/Controls/KR106Tooltip.h
    Source/Controls/KR106VarianceSheet.h
    Source/DSP/BBDFilter.h
    Source/DSP/KR106ADSR.h
    Source/DSP/KR106Arpeggiator.h
    Source/DSP/KR106Chorus.h
    Source/DSP/KR106LFO.h
    Source/DSP/KR106Noise.h
    Source/DSP/KR106Resampler.h

    Source/DSP/KR106VCF.h
    Source/DSP/KR106VCF_OPTIMIZED.h
    Source/DSP/KR106VcfFreqJ106.h
    Source/DSP/KR106VcfFreqJ6.h
    Source/DSP/KR106Voice.h
    Source/DSP/KR106_DSP.h
    Source/DSP/KR106_DSP_SetParam.h
    Source/DSP/KR106_HPF.h
)

target_include_directories(KR106 PRIVATE
    Source
    Source/DSP
    Source/Controls
)

target_compile_definitions(KR106 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    _USE_MATH_DEFINES
)

# Release DSP optimization flags: FMA fusion + skip errno on math calls.
# Safe for audio DSP (no NaN checks, no errno reads). ~3-4% measured improvement.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(KR106 PRIVATE
    $<$<CONFIG:Release>:
      -ffp-contract=fast
      -fno-math-errno
    >
  )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(KR106 PRIVATE
    $<$<CONFIG:Release>:
      /fp:contract
    >
  )
endif()

target_link_libraries(KR106 PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    KR106BinaryData
)

clap_juce_extensions_plugin(TARGET KR106
    CLAP_ID "org.kayrock.kr106"
    CLAP_FEATURES instrument synthesizer analog
)

# --- AU SysEx variant (aumf) for Logic Pro SysEx support ---
# Logic blocks SysEx for aumu (instrument) plugins. This builds a second AU
# as kAudioUnitType_MusicEffect (aumf), which receives full MIDI including SysEx.
# Only builds AU format -- VST3/CLAP/LV2 don't need this workaround.
if(APPLE)
    juce_add_plugin(KR106SysEx
        COMPANY_NAME "Kayrock"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        AU_MAIN_TYPE "kAudioUnitType_MusicEffect"
        PLUGIN_MANUFACTURER_CODE Krok
        PLUGIN_CODE KrSx
        FORMATS AU
        PRODUCT_NAME "Ultramaster KR-106 (SysEx)"
        PLUGIN_NAME "Ultramaster KR-106 (SysEx)"
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/img/icon_1024.png"
        ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/img/icon_1024.png"
        COPY_PLUGIN_AFTER_BUILD ${KR106_COPY_AFTER_BUILD}
    )

    target_sources(KR106SysEx PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.h
        Source/KR106PresetManager.h
        Source/KR106_Presets_JUCE.h
        Source/Controls/KR106Bender.h
        Source/Controls/KR106Button.h
        Source/Controls/KR106Keyboard.h
        Source/Controls/KR106MenuSheet.h
        Source/Controls/KR106Knob.h
        Source/Controls/KR106Misc.h
        Source/Controls/KR106PresetDisplay.h
        Source/Controls/KR106Scope.h
        Source/Controls/KR106Slider.h
        Source/Controls/KR106Switch.h
        Source/Controls/KR106Tooltip.h
        Source/Controls/KR106VarianceSheet.h
        Source/DSP/BBDFilter.h
        Source/DSP/KR106ADSR.h
        Source/DSP/KR106Arpeggiator.h
        Source/DSP/KR106Chorus.h
        Source/DSP/KR106LFO.h
        Source/DSP/KR106Noise.h
        Source/DSP/KR106Resampler.h

        Source/DSP/KR106VCF.h
        Source/DSP/KR106VCF_OPTIMIZED.h
        Source/DSP/KR106VcfFreqJ106.h
        Source/DSP/KR106VcfFreqJ6.h
        Source/DSP/KR106Voice.h
        Source/DSP/KR106_DSP.h
        Source/DSP/KR106_DSP_SetParam.h
        Source/DSP/KR106_HPF.h
    )

    target_include_directories(KR106SysEx PRIVATE
        Source
        Source/DSP
        Source/Controls
    )

    target_compile_definitions(KR106SysEx PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        _USE_MATH_DEFINES
        KR106_AUMF_BUILD=1
    )

    target_link_libraries(KR106SysEx PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        KR106BinaryData
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      target_compile_options(KR106SysEx PRIVATE
        $<$<CONFIG:Release>:-ffp-contract=fast -fno-math-errno>
      )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
      target_compile_options(KR106SysEx PRIVATE
        $<$<CONFIG:Release>:/fp:contract>
      )
    endif()
endif()

# macOS: replace JUCE's auto-generated icon with our custom .icns (rounded corners + transparency)
if(APPLE)
    set(CUSTOM_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/img/KR106.icns")
    add_custom_command(TARGET KR106_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${CUSTOM_ICNS}"
            "$<TARGET_BUNDLE_DIR:KR106_Standalone>/Contents/Resources/Icon.icns"
    )
endif()
