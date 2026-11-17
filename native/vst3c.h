// VST3 binary interface in plain C, for the plug-in hosts. Written from Steinberg's VST3 interface headers
// (vst3_pluginterfaces, MIT licence, Copyright (c) Steinberg Media Technologies GmbH): the same interface IDs, method
// order and struct layouts, as C structs of function pointers. Only what a host needs is declared.
//
// Before including, define V3_WINDOWS (interface IDs in COM byte order, Windows result codes) or not (Linux), and
// PAPI as the plug-in calling convention: __stdcall on 32-bit Windows, nothing otherwise (64-bit Windows code is
// compiled with -mabi=ms). 32-bit Windows code must be compiled with -malign-double (VST3 packs to 8 bytes there).
#pragma once
#include <stdint.h>

typedef int32_t tresult;
typedef uint8_t TUID[16];
typedef uint16_t char16;
typedef char16 String128[128];
typedef uint32_t ParamID;
typedef double ParamValue;
typedef uint8_t TBool;

#ifdef V3_WINDOWS
enum { kResultOk = 0, kResultTrue = 0, kResultFalse = 1, kNoInterface = (int32_t)0x80004002, kInvalidArgument = (int32_t)0x80070057, kNotImplemented = (int32_t)0x80004001 };
#define V3_IID(a, b, c, d) { (a) & 255, (a) >> 8 & 255, (a) >> 16 & 255, (a) >> 24 & 255, (b) >> 16 & 255, (b) >> 24 & 255, (b) & 255, (b) >> 8 & 255, \
                             (c) >> 24 & 255, (c) >> 16 & 255, (c) >> 8 & 255, (c) & 255, (d) >> 24 & 255, (d) >> 16 & 255, (d) >> 8 & 255, (d) & 255 }
#else
enum { kNoInterface = -1, kResultOk = 0, kResultTrue = 0, kResultFalse = 1, kInvalidArgument = 2, kNotImplemented = 3 };
#define V3_IID(a, b, c, d) { (a) >> 24 & 255, (a) >> 16 & 255, (a) >> 8 & 255, (a) & 255, (b) >> 24 & 255, (b) >> 16 & 255, (b) >> 8 & 255, (b) & 255, \
                             (c) >> 24 & 255, (c) >> 16 & 255, (c) >> 8 & 255, (c) & 255, (d) >> 24 & 255, (d) >> 16 & 255, (d) >> 8 & 255, (d) & 255 }
#endif
static const TUID IID_FUnknown = V3_IID(0x00000000, 0x00000000, 0xC0000000, 0x00000046);
static const TUID IID_IPluginFactory = V3_IID(0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
static const TUID IID_IPluginFactory2 = V3_IID(0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
static const TUID IID_IPluginFactory3 = V3_IID(0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931);
static const TUID IID_IComponent = V3_IID(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
static const TUID IID_IAudioProcessor = V3_IID(0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
static const TUID IID_IEditController = V3_IID(0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
static const TUID IID_IMidiMapping = V3_IID(0xDF0FF9F7, 0x49B74669, 0xB63AB732, 0x7ADBF5E5);
static const TUID IID_IConnectionPoint = V3_IID(0x70A4156F, 0x6E6E4026, 0x989148BF, 0xAA60D8D1);
static const TUID IID_IMessage = V3_IID(0x936F033B, 0xC6C047DB, 0xBB0882F8, 0x13C1E613);
static const TUID IID_IAttributeList = V3_IID(0x1E5F0AEB, 0xCC7F4533, 0xA2544011, 0x38AD5EE4);
static const TUID IID_IBStream = V3_IID(0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B);
static const TUID IID_IHostApplication = V3_IID(0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5);
static const TUID IID_IComponentHandler = V3_IID(0x93A0BEA3, 0x0BD045DB, 0x8E890B0C, 0xC1E46AC6);
static const TUID IID_IParamValueQueue = V3_IID(0x01263A18, 0xED074F6F, 0x98C9D356, 0x4686F9BA);
static const TUID IID_IParameterChanges = V3_IID(0xA4779663, 0x0BB64A56, 0xB44384A8, 0x466FEB9D);
static const TUID IID_IEventList = V3_IID(0x3A2C4214, 0x346349FE, 0xB2C4F397, 0xB9695A44);
static const TUID IID_IPlugView = V3_IID(0x5BC32507, 0xD06049EA, 0xA6151B52, 0x2B755B29);
static const TUID IID_IPlugFrame = V3_IID(0x367FAF01, 0xAFA94693, 0x8D4DA2A0, 0xED0882A3);
static const TUID IID_IRunLoop = V3_IID(0x18C35366, 0x97764F1A, 0x9C5B8385, 0x7A871389);
static const TUID IID_IEventHandler = V3_IID(0x561E65C9, 0x13A0496F, 0x813A2C35, 0x654D7983);
static const TUID IID_ITimerHandler = V3_IID(0x10BDD94F, 0x41424774, 0x821FAD8F, 0xECA72CA9);

// Call method m of interface object o (a pointer whose first field points to the method table of type T).
#define V3(o, T, m, ...) ((*(struct T **)(o))->m((o), ##__VA_ARGS__))
#define V3_UNKNOWN \
  tresult (PAPI *queryInterface)(void *self, const uint8_t *iid, void **obj); \
  uint32_t (PAPI *addRef)(void *self); \
  uint32_t (PAPI *release)(void *self)
#define V3_PLUGINBASE V3_UNKNOWN; tresult (PAPI *initialize)(void *self, void *context); tresult (PAPI *terminate)(void *self)

typedef struct { char vendor[64], url[256], email[128]; int32_t flags; } PFactoryInfo;
typedef struct { TUID cid; int32_t cardinality; char category[32], name[64]; } PClassInfo;
typedef struct { TUID cid; int32_t cardinality; char category[32], name[64]; uint32_t classFlags; char subCategories[128], vendor[64], version[64], sdkVersion[64]; } PClassInfo2;
struct v3_unknown { V3_UNKNOWN; };
struct v3_factory {
  V3_UNKNOWN;
  tresult (PAPI *getFactoryInfo)(void *self, PFactoryInfo *info);
  int32_t (PAPI *countClasses)(void *self);
  tresult (PAPI *getClassInfo)(void *self, int32_t index, PClassInfo *info);
  tresult (PAPI *createInstance)(void *self, const void *cid, const void *iid, void **obj);
  tresult (PAPI *getClassInfo2)(void *self, int32_t index, PClassInfo2 *info);   // IPluginFactory2 only
};
struct v3_pluginbase { V3_PLUGINBASE; };

enum { kAudio = 0, kEvent = 1, kInput = 0, kOutput = 1, kMainBus = 0, kAuxBus = 1 };
typedef struct { int32_t mediaType, direction, channelCount; String128 name; int32_t busType; uint32_t flags; } BusInfo;
struct v3_component {
  V3_PLUGINBASE;
  tresult (PAPI *getControllerClassId)(void *self, TUID cid);
  tresult (PAPI *setIoMode)(void *self, int32_t mode);
  int32_t (PAPI *getBusCount)(void *self, int32_t type, int32_t dir);
  tresult (PAPI *getBusInfo)(void *self, int32_t type, int32_t dir, int32_t index, BusInfo *info);
  tresult (PAPI *getRoutingInfo)(void *self, void *in, void *out);
  tresult (PAPI *activateBus)(void *self, int32_t type, int32_t dir, int32_t index, TBool state);
  tresult (PAPI *setActive)(void *self, TBool state);
  tresult (PAPI *setState)(void *self, void *stream);
  tresult (PAPI *getState)(void *self, void *stream);
};

typedef struct { int32_t processMode, symbolicSampleSize, maxSamplesPerBlock; double sampleRate; } ProcessSetup;
typedef struct { int32_t numChannels; uint64_t silenceFlags; float **channelBuffers32; } AudioBusBuffers;
typedef struct { uint8_t keyNote, rootNote; int16_t chordMask; } V3Chord;
typedef struct { uint32_t framesPerSecond, flags; } V3FrameRate;
typedef struct {
  uint32_t state; double sampleRate; int64_t projectTimeSamples, systemTime, continousTimeSamples;
  double projectTimeMusic, barPositionMusic, cycleStartMusic, cycleEndMusic, tempo;
  int32_t timeSigNumerator, timeSigDenominator; V3Chord chord; int32_t smpteOffsetSubframes; V3FrameRate frameRate; int32_t samplesToNextClock;
} ProcessContext;
enum { kCtxPlaying = 1 << 1, kCtxSystemTimeValid = 1 << 8, kCtxProjectTimeMusicValid = 1 << 9, kCtxTempoValid = 1 << 10, kCtxTimeSigValid = 1 << 13 };
typedef struct {
  int32_t processMode, symbolicSampleSize, numSamples, numInputs, numOutputs;
  AudioBusBuffers *inputs, *outputs;
  void *inputParameterChanges, *outputParameterChanges, *inputEvents, *outputEvents;
  ProcessContext *processContext;
} ProcessData;
struct v3_processor {
  V3_UNKNOWN;
  tresult (PAPI *setBusArrangements)(void *self, uint64_t *inputs, int32_t numIns, uint64_t *outputs, int32_t numOuts);
  tresult (PAPI *getBusArrangement)(void *self, int32_t dir, int32_t index, uint64_t *arr);
  tresult (PAPI *canProcessSampleSize)(void *self, int32_t size);
  uint32_t (PAPI *getLatencySamples)(void *self);
  tresult (PAPI *setupProcessing)(void *self, ProcessSetup *setup);
  tresult (PAPI *setProcessing)(void *self, TBool state);
  tresult (PAPI *process)(void *self, ProcessData *data);
  uint32_t (PAPI *getTailSamples)(void *self);
};
#define V3_SPEAKERS_STEREO 3ull

enum { kParamCanAutomate = 1, kParamIsReadOnly = 1 << 1, kParamIsList = 1 << 3, kParamIsHidden = 1 << 4, kParamIsProgramChange = 1 << 15, kParamIsBypass = 1 << 16 };
typedef struct { ParamID id; String128 title, shortTitle, units; int32_t stepCount; ParamValue defaultNormalizedValue; int32_t unitId, flags; } ParameterInfo;
struct v3_controller {
  V3_PLUGINBASE;
  tresult (PAPI *setComponentState)(void *self, void *stream);
  tresult (PAPI *setState)(void *self, void *stream);
  tresult (PAPI *getState)(void *self, void *stream);
  int32_t (PAPI *getParameterCount)(void *self);
  tresult (PAPI *getParameterInfo)(void *self, int32_t index, ParameterInfo *info);
  tresult (PAPI *getParamStringByValue)(void *self, ParamID id, ParamValue value, char16 *out);
  tresult (PAPI *getParamValueByString)(void *self, ParamID id, char16 *text, ParamValue *value);
  ParamValue (PAPI *normalizedParamToPlain)(void *self, ParamID id, ParamValue value);
  ParamValue (PAPI *plainParamToNormalized)(void *self, ParamID id, ParamValue value);
  ParamValue (PAPI *getParamNormalized)(void *self, ParamID id);
  tresult (PAPI *setParamNormalized)(void *self, ParamID id, ParamValue value);
  tresult (PAPI *setComponentHandler)(void *self, void *handler);
  void *(PAPI *createView)(void *self, const char *name);
};
enum { kCtrlModWheel = 1, kCtrlSustainOnOff = 64, kAfterTouch = 128, kPitchBend = 129 };
struct v3_midimapping { V3_UNKNOWN; tresult (PAPI *getMidiControllerAssignment)(void *self, int32_t bus, int16_t channel, int16_t ctrl, ParamID *id); };
struct v3_connection { V3_UNKNOWN; tresult (PAPI *connect)(void *self, void *other); tresult (PAPI *disconnect)(void *self, void *other); tresult (PAPI *notify)(void *self, void *message); };

typedef struct { int16_t channel, pitch; float tuning, velocity; int32_t length, noteId; } NoteOnEvent;
typedef struct { int16_t channel, pitch; float velocity; int32_t noteId; float tuning; } NoteOffEvent;
typedef struct {
  int32_t busIndex, sampleOffset; double ppqPosition; uint16_t flags, type;
  union {
    NoteOnEvent noteOn; NoteOffEvent noteOff;
    struct { uint32_t typeId; int32_t noteId; double value; } noteExpressionValue;
    struct { uint32_t typeId; int32_t noteId; uint32_t textLen; const char16 *text; } noteExpressionText;   // the largest member: sets the size
    struct { uint32_t size, type; const uint8_t *bytes; } data;
  };
} V3Event;
enum { kNoteOnEvent = 0, kNoteOffEvent = 1 };

typedef struct { int32_t left, top, right, bottom; } ViewRect;
struct v3_view {
  V3_UNKNOWN;
  tresult (PAPI *isPlatformTypeSupported)(void *self, const char *type);
  tresult (PAPI *attached)(void *self, void *parent, const char *type);
  tresult (PAPI *removed)(void *self);
  tresult (PAPI *onWheel)(void *self, float distance);
  tresult (PAPI *onKeyDown)(void *self, char16 key, int16_t code, int16_t mods);
  tresult (PAPI *onKeyUp)(void *self, char16 key, int16_t code, int16_t mods);
  tresult (PAPI *getSize)(void *self, ViewRect *size);
  tresult (PAPI *onSize)(void *self, ViewRect *size);
  tresult (PAPI *onFocus)(void *self, TBool state);
  tresult (PAPI *setFrame)(void *self, void *frame);
  tresult (PAPI *canResize)(void *self);
  tresult (PAPI *checkSizeConstraint)(void *self, ViewRect *rect);
};

// Interfaces the host implements; the plug-in calls these.
struct v3_stream_vt { V3_UNKNOWN; tresult (PAPI *read)(void *, void *, int32_t, int32_t *); tresult (PAPI *write)(void *, void *, int32_t, int32_t *); tresult (PAPI *seek)(void *, int64_t, int32_t, int64_t *); tresult (PAPI *tell)(void *, int64_t *); };
struct v3_hostapp_vt { V3_UNKNOWN; tresult (PAPI *getName)(void *, char16 *); tresult (PAPI *createInstance)(void *, const uint8_t *, const uint8_t *, void **); };
struct v3_handler_vt { V3_UNKNOWN; tresult (PAPI *beginEdit)(void *, ParamID); tresult (PAPI *performEdit)(void *, ParamID, ParamValue); tresult (PAPI *endEdit)(void *, ParamID); tresult (PAPI *restartComponent)(void *, int32_t); };
struct v3_queue_vt { V3_UNKNOWN; ParamID (PAPI *getParameterId)(void *); int32_t (PAPI *getPointCount)(void *); tresult (PAPI *getPoint)(void *, int32_t, int32_t *, ParamValue *); tresult (PAPI *addPoint)(void *, int32_t, ParamValue, int32_t *); };
struct v3_changes_vt { V3_UNKNOWN; int32_t (PAPI *getParameterCount)(void *); void *(PAPI *getParameterData)(void *, int32_t); void *(PAPI *addParameterData)(void *, const ParamID *, int32_t *); };
struct v3_events_vt { V3_UNKNOWN; int32_t (PAPI *getEventCount)(void *); tresult (PAPI *getEvent)(void *, int32_t, V3Event *); tresult (PAPI *addEvent)(void *, V3Event *); };
struct v3_frame_vt { V3_UNKNOWN; tresult (PAPI *resizeView)(void *, void *, ViewRect *); };
struct v3_message_vt { V3_UNKNOWN; const char *(PAPI *getMessageID)(void *); void (PAPI *setMessageID)(void *, const char *); void *(PAPI *getAttributes)(void *); };
struct v3_attrs_vt {
  V3_UNKNOWN;
  tresult (PAPI *setInt)(void *, const char *, int64_t); tresult (PAPI *getInt)(void *, const char *, int64_t *);
  tresult (PAPI *setFloat)(void *, const char *, double); tresult (PAPI *getFloat)(void *, const char *, double *);
  tresult (PAPI *setString)(void *, const char *, const char16 *); tresult (PAPI *getString)(void *, const char *, char16 *, uint32_t);
  tresult (PAPI *setBinary)(void *, const char *, const void *, uint32_t); tresult (PAPI *getBinary)(void *, const char *, const void **, uint32_t *);
};
// Linux only: the plug-in's window asks the host for timers and file-descriptor callbacks through this.
struct v3_runloop_vt { V3_UNKNOWN; tresult (PAPI *registerEventHandler)(void *, void *handler, int fd); tresult (PAPI *unregisterEventHandler)(void *, void *handler); tresult (PAPI *registerTimer)(void *, void *handler, uint64_t ms); tresult (PAPI *unregisterTimer)(void *, void *handler); };
struct v3_eventhandler { V3_UNKNOWN; void (PAPI *onFDIsSet)(void *self, int fd); };
struct v3_timerhandler { V3_UNKNOWN; void (PAPI *onTimer)(void *self); };
