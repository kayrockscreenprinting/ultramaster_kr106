#pragma once

#include "aeffect.h"

#if defined(_WIN32)
 #pragma pack(push, 8)
#elif defined(__APPLE__)
 #if defined(__LP64__) || defined(_LP64)
  #pragma options align=power
 #else
  #pragma options align=mac68k
 #endif
#else
 #pragma pack(push, 8)
#endif

enum Vst2StringConstants
{
    kVstMaxNameLen       = 64,
    kVstMaxLabelLen      = 64,
    kVstMaxShortLabelLen = 8,
    kVstMaxCategLabelLen = 24,
    kVstMaxFileNameLen   = 100
};

enum AEffectXOpcodes
{
    effProcessEvents = effSetChunk + 1,       // 25
    effCanBeAutomated,                        // 26
    effString2Parameter,                      // 27
    effGetNumProgramCategories,               // 28
    effGetProgramNameIndexed,                 // 29
    effCopyProgram,                           // 30
    effConnectInput,                          // 31
    effConnectOutput,                         // 32
    effGetInputProperties,                    // 33
    effGetOutputProperties,                   // 34
    effGetPlugCategory,                       // 35
    effGetCurrentPosition,                    // 36
    effGetDestinationBuffer,                  // 37
    effOfflineNotify,                         // 38
    effOfflinePrepare,                        // 39
    effOfflineRun,                            // 40
    effProcessVarIo,                          // 41
    effSetSpeakerArrangement,                 // 42
    effSetBlockSizeAndSampleRate,             // 43
    effSetBypass,                             // 44
    effGetEffectName,                         // 45
    effGetErrorText,                          // 46
    effGetVendorString,                       // 47
    effGetProductString,                      // 48
    effGetVendorVersion,                      // 49
    effVendorSpecific,                        // 50
    effCanDo,                                 // 51
    effGetTailSize,                           // 52
    effIdle,                                  // 53
    effGetIcon,                               // 54
    effSetViewPosition,                       // 55
    effGetParameterProperties,                // 56
    effKeysRequired,                          // 57
    effGetVstVersion,                         // 58
    effEditKeyDown,                           // 59
    effEditKeyUp,                             // 60
    effSetEditKnobMode,                       // 61
    effGetMidiProgramName,                    // 62
    effGetCurrentMidiProgram,                 // 63
    effGetMidiProgramCategory,                // 64
    effHasMidiProgramsChanged,                // 65
    effGetMidiKeyName,                        // 66
    effBeginSetProgram,                       // 67
    effEndSetProgram,                         // 68
    effGetSpeakerArrangement,                 // 69
    effShellGetNextPlugin,                    // 70
    effStartProcess,                          // 71
    effStopProcess,                           // 72
    effSetTotalSampleToProcess,               // 73
    effSetPanLaw,                             // 74
    effBeginLoadBank,                         // 75
    effBeginLoadProgram,                      // 76
    effSetProcessPrecision,                   // 77
    effGetNumMidiInputChannels,               // 78
    effGetNumMidiOutputChannels               // 79
};

enum AudioMasterOpcodesX
{
    audioMasterWantMidi = audioMasterPinConnected + 2, // 6
    audioMasterGetTime,                                // 7
    audioMasterProcessEvents,                          // 8
    audioMasterSetTime,                                // 9
    audioMasterTempoAt,                                // 10
    audioMasterGetNumAutomatableParameters,            // 11
    audioMasterGetParameterQuantization,               // 12
    audioMasterIOChanged,                              // 13
    audioMasterNeedIdle,                               // 14
    audioMasterSizeWindow,                             // 15
    audioMasterGetSampleRate,                          // 16
    audioMasterGetBlockSize,                           // 17
    audioMasterGetInputLatency,                        // 18
    audioMasterGetOutputLatency,                       // 19
    audioMasterGetPreviousPlug,                        // 20
    audioMasterGetNextPlug,                            // 21
    audioMasterWillReplaceOrAccumulate,                // 22
    audioMasterGetCurrentProcessLevel,                 // 23
    audioMasterGetAutomationState,                     // 24
    audioMasterOfflineStart,                           // 25
    audioMasterOfflineRead,                            // 26
    audioMasterOfflineWrite,                           // 27
    audioMasterOfflineGetCurrentPass,                  // 28
    audioMasterOfflineGetCurrentMetaPass,              // 29
    audioMasterSetOutputSampleRate,                    // 30
    audioMasterGetOutputSpeakerArrangement,            // 31
    audioMasterGetVendorString,                        // 32
    audioMasterGetProductString,                       // 33
    audioMasterGetVendorVersion,                       // 34
    audioMasterVendorSpecific,                         // 35
    audioMasterSetIcon,                                // 36
    audioMasterCanDo,                                  // 37
    audioMasterGetLanguage,                            // 38
    audioMasterOpenWindow,                             // 39
    audioMasterCloseWindow,                            // 40
    audioMasterGetDirectory,                           // 41
    audioMasterUpdateDisplay,                          // 42
    audioMasterBeginEdit,                              // 43
    audioMasterEndEdit,                                // 44
    audioMasterOpenFileSelector,                       // 45
    audioMasterCloseFileSelector,                      // 46
    audioMasterEditFile,                               // 47
    audioMasterGetChunkFile,                           // 48
    audioMasterGetInputSpeakerArrangement              // 49
};

enum VstPlugCategory
{
    kPlugCategUnknown = 0,
    kPlugCategEffect,
    kPlugCategSynth,
    kPlugCategAnalysis,
    kPlugCategMastering,
    kPlugCategSpacializer,
    kPlugCategRoomFx,
    kPlugSurroundFx,
    kPlugCategRestoration,
    kPlugCategOfflineProcess,
    kPlugCategShell,
    kPlugCategGenerator,
    kPlugCategMaxCount
};

enum VstProcessPrecision
{
    kVstProcessPrecision32 = 0,
    kVstProcessPrecision64
};

struct VstPinProperties
{
    char label[kVstMaxLabelLen];
    VstInt32 flags;
    VstInt32 arrangementType;
    char shortLabel[kVstMaxShortLabelLen];
    char future[48];
};

enum VstPinPropertiesFlags
{
    kVstPinIsActive   = 1 << 0,
    kVstPinIsStereo   = 1 << 1,
    kVstPinUseSpeaker = 1 << 2
};

struct VstEvent
{
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    char data[16];
};

enum VstEventTypes
{
    kVstMidiType  = 1,
    kVstAudioType = 2,
    kVstVideoType = 3,
    kVstParameterType = 4,
    kVstTriggerType = 5,
    kVstSysExType = 6
};

struct VstEvents
{
    VstInt32 numEvents;
    VstIntPtr reserved;
    VstEvent* events[2];
};

struct VstMidiEvent
{
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 noteLength;
    VstInt32 noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

enum VstMidiEventFlags
{
    kVstMidiEventIsRealtime = 1 << 0
};

struct VstMidiSysexEvent
{
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 dumpBytes;
    VstIntPtr resvd1;
    char* sysexDump;
    VstIntPtr resvd2;
};

struct VstTimeInfo
{
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    VstInt32 timeSigNumerator;
    VstInt32 timeSigDenominator;
    VstInt32 smpteOffset;
    VstInt32 smpteFrameRate;
    VstInt32 samplesToNextClock;
    VstInt32 flags;
};

enum VstTimeInfoFlags
{
    kVstTransportChanged     = 1,
    kVstTransportPlaying     = 1 << 1,
    kVstTransportCycleActive = 1 << 2,
    kVstTransportRecording   = 1 << 3,
    kVstAutomationWriting    = 1 << 6,
    kVstAutomationReading    = 1 << 7,
    kVstNanosValid           = 1 << 8,
    kVstPpqPosValid          = 1 << 9,
    kVstTempoValid           = 1 << 10,
    kVstBarsValid            = 1 << 11,
    kVstCyclePosValid        = 1 << 12,
    kVstTimeSigValid         = 1 << 13,
    kVstSmpteValid           = 1 << 14,
    kVstClockValid           = 1 << 15
};

enum VstSmpteFrameRate
{
    kVstSmpte24fps    = 0,
    kVstSmpte25fps    = 1,
    kVstSmpte2997fps  = 2,
    kVstSmpte30fps    = 3,
    kVstSmpte2997dfps = 4,
    kVstSmpte30dfps   = 5,
    kVstSmpteFilm16mm = 6,
    kVstSmpteFilm35mm = 7,
    kVstSmpte239fps   = 10,
    kVstSmpte249fps   = 11,
    kVstSmpte599fps   = 12,
    kVstSmpte60fps    = 13
};

struct VstSpeakerProperties
{
    float azimuth;
    float elevation;
    float radius;
    float reserved;
    char name[kVstMaxNameLen];
    VstInt32 type;
    char future[28];
};

struct VstSpeakerArrangement
{
    VstInt32 type;
    VstInt32 numChannels;
    VstSpeakerProperties speakers[8];
};

enum VstSpeakerType
{
    kSpeakerUndefined = 0x7fffffff,
    kSpeakerM = 0,
    kSpeakerL,
    kSpeakerR,
    kSpeakerC,
    kSpeakerLfe,
    kSpeakerLs,
    kSpeakerRs,
    kSpeakerLc,
    kSpeakerRc,
    kSpeakerS,
    kSpeakerCs = kSpeakerS,
    kSpeakerSl,
    kSpeakerSr,
    kSpeakerTm,
    kSpeakerTfl,
    kSpeakerTfc,
    kSpeakerTfr,
    kSpeakerTrl,
    kSpeakerTrc,
    kSpeakerTrr,
    kSpeakerLfe2
};

enum VstSpeakerArrangementType
{
    kSpeakerArrUserDefined = -2,
    kSpeakerArrEmpty = -1,
    kSpeakerArrMono  =  0,
    kSpeakerArrStereo,
    kSpeakerArrStereoSurround,
    kSpeakerArrStereoCenter,
    kSpeakerArrStereoSide,
    kSpeakerArrStereoCLfe,
    kSpeakerArr30Cine,
    kSpeakerArr30Music,
    kSpeakerArr31Cine,
    kSpeakerArr31Music,
    kSpeakerArr40Cine,
    kSpeakerArr40Music,
    kSpeakerArr41Cine,
    kSpeakerArr41Music,
    kSpeakerArr50,
    kSpeakerArr51,
    kSpeakerArr60Cine,
    kSpeakerArr60Music,
    kSpeakerArr61Cine,
    kSpeakerArr61Music,
    kSpeakerArr70Cine,
    kSpeakerArr70Music,
    kSpeakerArr71Cine,
    kSpeakerArr71Music,
    kSpeakerArr80Cine,
    kSpeakerArr80Music,
    kSpeakerArr81Cine,
    kSpeakerArr81Music,
    kSpeakerArr102,
    kNumSpeakerArr
};

struct MidiKeyName
{
    VstInt32 thisProgramIndex;
    VstInt32 thisKeyNumber;
    char keyName[kVstMaxNameLen];
    VstInt32 reserved;
    VstInt32 flags;
};

#if defined(_WIN32)
 #pragma pack(pop)
#elif defined(__APPLE__)
 #pragma options align=reset
#else
 #pragma pack(pop)
#endif

