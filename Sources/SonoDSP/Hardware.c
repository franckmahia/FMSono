#include "SonoAudio.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 8192u
#define MAX_LISTENERS 12u
typedef struct { AudioObjectID object; AudioObjectPropertyAddress address; } Listener;
struct SonoAudio {
    AudioUnit input, output;
    SonoProcessor *processor;
    float input_samples[MAX_FRAMES], output_samples[MAX_FRAMES];
    double input_rate, output_rate, estimated_latency;
    _Atomic int32_t error_code;
    _Atomic bool route_changed;
    Listener listeners[MAX_LISTENERS];
    uint32_t listener_count;
};

static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    return (AudioObjectPropertyAddress){selector, scope, kAudioObjectPropertyElementMain};
}

static UInt32 read_uint(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    UInt32 value = 0, size = sizeof(value);
    AudioObjectPropertyAddress property = address(selector, scope);
    AudioObjectGetPropertyData(object, &property, 0, NULL, &size, &value);
    return value;
}

static double sample_rate(AudioDeviceID device) {
    Float64 rate = 0; UInt32 size = sizeof(rate);
    AudioObjectPropertyAddress property = address(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    AudioObjectGetPropertyData(device, &property, 0, NULL, &size, &rate);
    return rate;
}

static uint32_t channels(AudioDeviceID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress property = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &property, 0, NULL, &size) != noErr || size < sizeof(AudioBufferList)) return 0;
    AudioBufferList *list = malloc(size);
    if (!list) return 0;
    uint32_t count = 0;
    if (AudioObjectGetPropertyData(device, &property, 0, NULL, &size, list) == noErr)
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) count += list->mBuffers[i].mNumberChannels;
    free(list);
    return count;
}

static void read_string(AudioDeviceID device, AudioObjectPropertySelector selector, char *destination, size_t capacity) {
    CFStringRef value = NULL; UInt32 size = sizeof(value);
    AudioObjectPropertyAddress property = address(selector, kAudioObjectPropertyScopeGlobal);
    destination[0] = '\0';
    if (AudioObjectGetPropertyData(device, &property, 0, NULL, &size, &value) == noErr && value) {
        CFStringGetCString(value, destination, (CFIndex)capacity, kCFStringEncodingUTF8);
        CFRelease(value);
    }
}

uint32_t sono_default_input(void) {
    return read_uint(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal);
}
uint32_t sono_default_output(void) {
    return read_uint(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
}

int32_t sono_devices(SonoDeviceInfo *devices, uint32_t capacity) {
    AudioObjectPropertyAddress property = address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0, NULL, &size) != noErr) return -1;
    if (size == 0) return 0;
    AudioDeviceID *ids = malloc(size);
    if (!ids) return -1;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL, &size, ids) != noErr) { free(ids); return -1; }
    uint32_t total = size / sizeof(AudioDeviceID), count = 0;
    AudioDeviceID default_input = sono_default_input(), default_output = sono_default_output();
    for (uint32_t i = 0; i < total && count < capacity; i++) {
        if (!read_uint(ids[i], kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal)) continue;
        SonoDeviceInfo info = {0}; info.id = ids[i];
        info.input_channels = channels(ids[i], kAudioObjectPropertyScopeInput);
        info.output_channels = channels(ids[i], kAudioObjectPropertyScopeOutput);
        if (!info.input_channels && !info.output_channels) continue;
        info.sample_rate = sample_rate(ids[i]);
        UInt32 transport = read_uint(ids[i], kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
        info.bluetooth = transport == kAudioDeviceTransportTypeBluetooth || transport == kAudioDeviceTransportTypeBluetoothLE;
        info.default_input = ids[i] == default_input; info.default_output = ids[i] == default_output;
        read_string(ids[i], kAudioObjectPropertyName, info.name, sizeof(info.name));
        read_string(ids[i], kAudioDevicePropertyDeviceUID, info.uid, sizeof(info.uid));
        devices[count++] = info;
    }
    free(ids);
    return (int32_t)count;
}

static void fail_audio(SonoAudio *audio, OSStatus error) {
    atomic_store(&audio->error_code, error ? error : -1);
    sono_set_muted(audio->processor, true);
}

static OSStatus input_callback(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                               UInt32 bus, UInt32 frames, AudioBufferList *unused) {
    (void)bus; (void)unused;
    SonoAudio *audio = context;
    if (frames > MAX_FRAMES) { fail_audio(audio, kAudioUnitErr_TooManyFramesToProcess); return noErr; }
    if (atomic_load(&audio->error_code) || atomic_load(&audio->route_changed)) return noErr;
    AudioBufferList list = { .mNumberBuffers = 1,
        .mBuffers = {{ .mNumberChannels = 1, .mDataByteSize = frames * sizeof(float), .mData = audio->input_samples }} };
    OSStatus status = AudioUnitRender(audio->input, flags, timestamp, 1, frames, &list);
    if (status != noErr) { fail_audio(audio, status); return noErr; }
    sono_push(audio->processor, audio->input_samples, frames);
    return noErr;
}

static OSStatus output_callback(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                                UInt32 bus, UInt32 frames, AudioBufferList *data) {
    (void)timestamp; (void)bus;
    SonoAudio *audio = context;
    if (!data) return noErr;
    for (UInt32 i = 0; i < data->mNumberBuffers; i++)
        if (data->mBuffers[i].mData) memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
    if (frames > MAX_FRAMES) { fail_audio(audio, kAudioUnitErr_TooManyFramesToProcess); return noErr; }
    if (atomic_load(&audio->error_code) || atomic_load(&audio->route_changed)) {
        *flags |= kAudioUnitRenderAction_OutputIsSilence; return noErr;
    }
    sono_render(audio->processor, audio->output_samples, frames);
    for (UInt32 i = 0; i < data->mNumberBuffers; i++) {
        AudioBuffer *buffer = &data->mBuffers[i];
        if (!buffer->mData || buffer->mNumberChannels == 0) continue;
        if ((uint64_t)frames * buffer->mNumberChannels * sizeof(float) > buffer->mDataByteSize) {
            fail_audio(audio, kAudioUnitErr_InvalidPropertyValue); continue;
        }
        float *destination = buffer->mData;
        for (UInt32 frame = 0; frame < frames; frame++)
            for (UInt32 channel = 0; channel < buffer->mNumberChannels; channel++)
                destination[frame * buffer->mNumberChannels + channel] = audio->output_samples[frame];
    }
    return noErr;
}

static OSStatus changed(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *properties, void *context) {
    (void)object; (void)count; (void)properties;
    SonoAudio *audio = context;
    atomic_store(&audio->route_changed, true);
    sono_set_muted(audio->processor, true);
    return noErr;
}

static OSStatus add_listener(SonoAudio *audio, AudioObjectID object,
                             AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    if (audio->listener_count >= MAX_LISTENERS) return kAudioUnitErr_InvalidProperty;
    Listener listener = {object, address(selector, scope)};
    OSStatus status = AudioObjectAddPropertyListener(object, &listener.address, changed, audio);
    if (status == noErr) audio->listeners[audio->listener_count++] = listener;
    return status;
}

static AudioStreamBasicDescription float_format(double rate, uint32_t count) {
    return (AudioStreamBasicDescription){ .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
        .mBytesPerPacket = sizeof(float), .mFramesPerPacket = 1, .mBytesPerFrame = sizeof(float),
        .mChannelsPerFrame = count, .mBitsPerChannel = 32 };
}

SonoAudio *sono_audio_open(uint32_t input, uint32_t output, uint32_t channel,
                          bool follows_default, char *error, size_t error_capacity) {
    const char *stage = "Préparer les périphériques"; OSStatus status = noErr;
    SonoAudio *audio = calloc(1, sizeof(*audio));
    if (!audio) { snprintf(error, error_capacity, "Mémoire audio indisponible."); return NULL; }
    atomic_init(&audio->error_code, 0); atomic_init(&audio->route_changed, false);
    audio->input_rate = sample_rate(input); audio->output_rate = sample_rate(output);
    uint32_t input_count = channels(input, kAudioObjectPropertyScopeInput);
    uint32_t output_count = channels(output, kAudioObjectPropertyScopeOutput);
    if (channel >= input_count || !output_count) { status = kAudioUnitErr_InvalidElement; goto failure; }
    UInt32 input_frames = read_uint(input, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    UInt32 output_frames = read_uint(output, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    double ratio = audio->output_rate > 0 ? audio->input_rate / audio->output_rate : 0;
    UInt32 target = (UInt32)fmax(256, fmax(input_frames, output_frames * ratio));
    audio->processor = sono_create(audio->input_rate, audio->output_rate, target);
    if (!audio->processor) { status = kAudioUnitErr_FormatNotSupported; goto failure; }
    audio->estimated_latency = 1000 * (target + input_frames) / audio->input_rate +
        1000 * output_frames / audio->output_rate;
    audio->estimated_latency += 1000 * (read_uint(input, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) +
        read_uint(input, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput)) / audio->input_rate;
    audio->estimated_latency += 1000 * (read_uint(output, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) +
        read_uint(output, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput)) / audio->output_rate;
    AudioComponentDescription description = { .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput, .componentManufacturer = kAudioUnitManufacturer_Apple };
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    if (!component) { status = kAudioUnitErr_InvalidProperty; goto failure; }
#define CHECK(action, label) do { stage = label; status = (action); if (status != noErr) goto failure; } while (0)
    CHECK(AudioComponentInstanceNew(component, &audio->input), "Ouvrir l’entrée audio");
    CHECK(AudioComponentInstanceNew(component, &audio->output), "Ouvrir la sortie audio");
    UInt32 yes = 1, no = 0, maximum = MAX_FRAMES;
    CHECK(AudioUnitSetProperty(audio->input, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &yes, sizeof(yes)), "Activer le micro");
    CHECK(AudioUnitSetProperty(audio->input, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &no, sizeof(no)), "Configurer l’entrée");
    CHECK(AudioUnitSetProperty(audio->input, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &input, sizeof(input)), "Sélectionner le micro");
    CHECK(AudioUnitSetProperty(audio->output, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &output, sizeof(output)), "Sélectionner la sortie");
    AudioStreamBasicDescription input_format = float_format(audio->input_rate, 1);
    AudioStreamBasicDescription output_format = float_format(audio->output_rate, output_count > 1 ? 2 : 1);
    CHECK(AudioUnitSetProperty(audio->input, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &input_format, sizeof(input_format)), "Configurer le format du micro");
    SInt32 map = (SInt32)channel;
    CHECK(AudioUnitSetProperty(audio->input, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1, &map, sizeof(map)), "Sélectionner le canal du micro");
    CHECK(AudioUnitSetProperty(audio->output, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &output_format, sizeof(output_format)), "Configurer le format de sortie");
    SInt32 *output_map = malloc(output_count * sizeof(SInt32));
    if (!output_map) { stage = "Préparer les canaux de sortie"; status = kAudioUnitErr_FailedInitialization; goto failure; }
    for (UInt32 i = 0; i < output_count; i++) output_map[i] = i < 2 ? (SInt32)i : -1;
    stage = "Sélectionner les canaux de sortie";
    status = AudioUnitSetProperty(audio->output, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Input, 0,
                                  output_map, output_count * sizeof(SInt32));
    free(output_map);
    if (status != noErr) goto failure;
    CHECK(AudioUnitSetProperty(audio->input, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximum, sizeof(maximum)), "Dimensionner l’entrée");
    CHECK(AudioUnitSetProperty(audio->output, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximum, sizeof(maximum)), "Dimensionner la sortie");
    CHECK(AudioUnitSetProperty(audio->input, kAudioUnitProperty_ShouldAllocateBuffer, kAudioUnitScope_Output, 1, &no, sizeof(no)), "Préparer les buffers");
    AURenderCallbackStruct capture = {input_callback, audio}, render = {output_callback, audio};
    CHECK(AudioUnitSetProperty(audio->input, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &capture, sizeof(capture)), "Brancher la capture");
    CHECK(AudioUnitSetProperty(audio->output, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render, sizeof(render)), "Brancher la diffusion");
    CHECK(AudioUnitInitialize(audio->input), "Initialiser le micro");
    CHECK(AudioUnitInitialize(audio->output), "Initialiser la sortie");
    AudioDeviceID selected[2] = {input, output};
    for (int i = 0; i < 2; i++) {
        if (i == 1 && input == output) continue;
        CHECK(add_listener(audio, selected[i], kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal), "Surveiller les connexions");
        CHECK(add_listener(audio, selected[i], kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal), "Surveiller les formats");
        CHECK(add_listener(audio, selected[i], kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal), "Surveiller les buffers");
    }
    if (follows_default)
        CHECK(add_listener(audio, kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal), "Surveiller la sortie système");
    CHECK(AudioOutputUnitStart(audio->input), "Démarrer la capture");
    CHECK(AudioOutputUnitStart(audio->output), "Démarrer la sortie");
    // Remain muted until the UI explicitly starts diffusion.
    return audio;
failure:
    snprintf(error, error_capacity, "%s : erreur Core Audio %d.", stage, (int)status);
    sono_audio_close(audio);
    return NULL;
#undef CHECK
}

void sono_audio_close(SonoAudio *audio) {
    if (!audio) return;
    sono_set_muted(audio->processor, true);
    for (uint32_t i = 0; i < audio->listener_count; i++)
        AudioObjectRemovePropertyListener(audio->listeners[i].object, &audio->listeners[i].address, changed, audio);
    if (audio->output) { AudioOutputUnitStop(audio->output); AudioUnitUninitialize(audio->output); AudioComponentInstanceDispose(audio->output); }
    if (audio->input) { AudioOutputUnitStop(audio->input); AudioUnitUninitialize(audio->input); AudioComponentInstanceDispose(audio->input); }
    sono_destroy(audio->processor);
    free(audio);
}
void sono_audio_gain(SonoAudio *audio, float db) { if (audio) sono_set_gain(audio->processor, db); }
void sono_audio_mute(SonoAudio *audio, bool muted) { if (audio) sono_set_muted(audio->processor, muted); }
void sono_audio_protection(SonoAudio *audio, bool enabled) { if (audio) sono_set_feedback_enabled(audio->processor, enabled); }
SonoAudioStatus sono_audio_status(SonoAudio *audio) {
    if (!audio) return (SonoAudioStatus){0};
    return (SonoAudioStatus){ .dsp = sono_metrics(audio->processor), .error_code = atomic_load(&audio->error_code),
        .route_changed = atomic_load(&audio->route_changed), .input_rate = audio->input_rate,
        .output_rate = audio->output_rate, .estimated_latency_ms = audio->estimated_latency };
}
