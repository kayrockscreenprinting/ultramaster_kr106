cmake_minimum_required(VERSION 3.22)
project(KR106 VERSION 2.2.0)

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

add_subdirectory(libs/JUCE)
add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)

set(KR106_FORMATS AU VST3 LV2 Standalone)

juce_add_plugin(KR106
    COMPANY_NAME "Kayrock"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    PLUGIN_MANUFACTURER_CODE Krok
    PLUGIN_CODE Kr16
    FORMATS ${KR106_FORMATS}
    PRODUCT_NAME "Ultramaster KR-106"
    PLUGIN_NAME "Ultramaster KR-106"
    LV2URI "https://kayrock.org/kr106"
    COPY_PLUGIN_AFTER_BUILD TRUE
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

target_link_libraries(KR106 PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    KR106BinaryData
)

clap_juce_extensions_plugin(TARGET KR106
    CLAP_ID "org.kayrock.kr106"
    CLAP_FEATURES instrument synthesizer analog
)
