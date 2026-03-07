cmake_minimum_required(VERSION 3.22)
project(KR106 VERSION 1.0.0)

add_subdirectory(libs/JUCE)

juce_add_plugin(KR106
    COMPANY_NAME "Kayrock"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    PLUGIN_MANUFACTURER_CODE Krok
    PLUGIN_CODE Kr16
    FORMATS AU VST3 LV2 Standalone
    PRODUCT_NAME "KR106"
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
)

target_link_libraries(KR106 PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    KR106BinaryData
)
