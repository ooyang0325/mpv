#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AUCocoaUIView.h>
#import <CoreAudioKit/AUGenericView.h>

#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <strings.h>
#include <unistd.h>

#include "audio/aframe.h"
#include "audio/format.h"
#include "common/common.h"
#include "filters/f_autoconvert.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"

struct f_opts {
    char *component;
    char *state;
    bool bypass;
};

struct priv {
    struct f_opts *opts;
    struct mp_pin *in_pin;
    struct mp_aframe *format;
    struct mp_aframe_pool *out_pool;
    AudioComponentDescription desc;
    AudioUnit unit;
    CFPropertyListRef memory_state;
    NSWindow *window;
    struct mp_aframe *input;
    int64_t sample_time;
    UInt32 max_frames;
    double latency;
    double tail;
    bool bypass;
};

static bool parse_component(const char *value, AudioComponentDescription *desc)
{
    if (!value || strlen(value) != 24)
        return false;

    UInt32 fields[3] = {0};
    for (int n = 0; n < 3; n++) {
        char part[9] = {0};
        memcpy(part, value + n * 8, 8);
        char *end = NULL;
        unsigned long parsed = strtoul(part, &end, 16);
        if (!end || *end || parsed > UINT32_MAX)
            return false;
        fields[n] = parsed;
    }

    if (fields[0] != kAudioUnitType_Effect &&
        fields[0] != kAudioUnitType_MusicEffect)
        return false;

    *desc = (AudioComponentDescription) {
        .componentType = fields[0],
        .componentSubType = fields[1],
        .componentManufacturer = fields[2],
    };
    return true;
}

static NSString *component_name(AudioComponent component)
{
    CFStringRef value = NULL;
    if (AudioComponentCopyName(component, &value) != noErr || !value)
        return @"Audio Unit";
    NSString *name = [NSString stringWithString:(NSString *)value];
    CFRelease(value);
    return name;
}

static bool set_state(struct mp_filter *f, CFPropertyListRef state)
{
    struct priv *p = f->priv;
    UInt32 size = sizeof(state);
    OSStatus err = AudioUnitSetProperty(p->unit, kAudioUnitProperty_ClassInfo,
                                        kAudioUnitScope_Global, 0, &state, size);
    if (err != noErr)
        MP_ERR(f, "Could not restore Audio Unit state (%d).\n", (int)err);
    return err == noErr;
}

static bool load_state(struct mp_filter *f, const char *path)
{
    struct priv *p = f->priv;
    if (!p->unit)
        return false;
    if (!path || !path[0] || access(path, R_OK) != 0)
        return true;

    @autoreleasepool {
        NSError *error = nil;
        NSData *data = [NSData dataWithContentsOfFile:
            [NSString stringWithUTF8String:path] options:0 error:&error];
        if (!data) {
            MP_ERR(f, "Could not read Audio Unit state: %s\n",
                   error.localizedDescription.UTF8String);
            return false;
        }

        id state = [NSPropertyListSerialization propertyListWithData:data
            options:NSPropertyListImmutable format:nil error:&error];
        if (![state isKindOfClass:NSDictionary.class]) {
            MP_ERR(f, "Invalid Audio Unit state: %s\n",
                   error.localizedDescription.UTF8String ?: "not a dictionary");
            return false;
        }
        return set_state(f, (CFPropertyListRef)state);
    }
}

static bool save_state(struct mp_filter *f, const char *path)
{
    struct priv *p = f->priv;
    if (!p->unit || !path || !path[0])
        return false;

    CFPropertyListRef state = NULL;
    UInt32 size = sizeof(state);
    OSStatus err = AudioUnitGetProperty(p->unit, kAudioUnitProperty_ClassInfo,
                                        kAudioUnitScope_Global, 0, &state, &size);
    if (err != noErr || !state) {
        MP_ERR(f, "Could not read Audio Unit state (%d).\n", (int)err);
        return false;
    }

    bool result = false;
    @autoreleasepool {
        NSError *error = nil;
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:
            (id)state format:NSPropertyListBinaryFormat_v1_0 options:0 error:&error];
        result = data && [data writeToFile:[NSString stringWithUTF8String:path]
                                   options:NSDataWritingAtomic error:&error];
        if (!result) {
            MP_ERR(f, "Could not save Audio Unit state: %s\n",
                   error.localizedDescription.UTF8String);
        }
    }
    CFRelease(state);
    return result;
}

static void capture_state(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->memory_state) {
        CFRelease(p->memory_state);
        p->memory_state = NULL;
    }
    if (!p->unit)
        return;

    UInt32 size = sizeof(p->memory_state);
    if (AudioUnitGetProperty(p->unit, kAudioUnitProperty_ClassInfo,
                             kAudioUnitScope_Global, 0, &p->memory_state,
                             &size) != noErr)
        p->memory_state = NULL;
}

static void close_window(struct priv *p)
{
    if (!p->window)
        return;
    void (^close)(void) = ^{
        [p->window orderOut:nil];
        [p->window setContentView:nil];
        [p->window release];
        p->window = nil;
    };
    if (NSThread.isMainThread)
        close();
    else
        dispatch_sync(dispatch_get_main_queue(), close);
}

static void close_unit(struct priv *p)
{
    close_window(p);
    if (p->unit) {
        AudioUnitUninitialize(p->unit);
        AudioComponentInstanceDispose(p->unit);
        p->unit = NULL;
    }
}

static OSStatus input_cb(void *ctx, AudioUnitRenderActionFlags *flags,
                         const AudioTimeStamp *timestamp, UInt32 bus,
                         UInt32 frames, AudioBufferList *data)
{
    struct priv *p = ctx;
    int channels = p->input ? mp_aframe_get_channels(p->input) : 0;
    int samples = p->input ? mp_aframe_get_size(p->input) : 0;
    uint8_t **planes = p->input ? mp_aframe_get_data_ro(p->input) : NULL;
    int64_t offset = timestamp && (timestamp->mFlags & kAudioTimeStampSampleTimeValid)
                   ? llrint(timestamp->mSampleTime) - p->sample_time : 0;

    bool silence = !planes || offset < 0 || offset + frames > samples;
    for (UInt32 n = 0; n < data->mNumberBuffers; n++) {
        AudioBuffer *buffer = &data->mBuffers[n];
        buffer->mNumberChannels = 1;
        buffer->mDataByteSize = frames * sizeof(float);
        if (silence || n >= channels) {
            if (buffer->mData)
                memset(buffer->mData, 0, buffer->mDataByteSize);
        } else if (buffer->mData) {
            memcpy(buffer->mData, planes[n] + offset * sizeof(float),
                   buffer->mDataByteSize);
        } else {
            buffer->mData = planes[n] + offset * sizeof(float);
        }
    }
    if (silence)
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

static AudioUnit instantiate(AudioComponent component)
{
    __block AudioUnit unit = NULL;
    __block OSStatus status = noErr;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    AudioComponentInstantiate(component, 0, ^(AudioComponentInstance instance,
                                               OSStatus error) {
        unit = instance;
        status = error;
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
#if !OS_OBJECT_USE_OBJC
    dispatch_release(done);
#endif
    return status == noErr ? unit : NULL;
}

static bool configure(struct mp_filter *f, struct mp_aframe *frame)
{
    struct priv *p = f->priv;
    int channels = mp_aframe_get_channels(frame);
    int rate = mp_aframe_get_rate(frame);
    UInt32 frames = MPMAX(mp_aframe_get_size(frame), 4096);

    if (p->unit && mp_aframe_config_equals(frame, p->format) &&
        frames <= p->max_frames)
        return true;

    if (p->unit)
        capture_state(f);
    close_unit(p);

    AudioComponent component = AudioComponentFindNext(NULL, &p->desc);
    if (!component) {
        MP_ERR(f, "Audio Unit component is not installed.\n");
        return false;
    }
    p->unit = instantiate(component);
    if (!p->unit) {
        MP_ERR(f, "Could not instantiate %s.\n",
               component_name(component).UTF8String);
        return false;
    }

    AudioStreamBasicDescription asbd = {
        .mSampleRate = rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked |
                        kAudioFormatFlagIsNonInterleaved,
        .mBytesPerPacket = sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = sizeof(float),
        .mChannelsPerFrame = channels,
        .mBitsPerChannel = 8 * sizeof(float),
    };
    AURenderCallbackStruct callback = {
        .inputProc = input_cb,
        .inputProcRefCon = p,
    };
    OSStatus err = AudioUnitSetProperty(p->unit, kAudioUnitProperty_StreamFormat,
                                        kAudioUnitScope_Input, 0,
                                        &asbd, sizeof(asbd));
    if (err == noErr)
        err = AudioUnitSetProperty(p->unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Output, 0,
                                   &asbd, sizeof(asbd));
    if (err == noErr)
        err = AudioUnitSetProperty(p->unit,
                                   kAudioUnitProperty_MaximumFramesPerSlice,
                                   kAudioUnitScope_Global, 0,
                                   &frames, sizeof(frames));
    if (err == noErr)
        err = AudioUnitSetProperty(p->unit,
                                   kAudioUnitProperty_SetRenderCallback,
                                   kAudioUnitScope_Input, 0,
                                   &callback, sizeof(callback));
    if (err != noErr) {
        MP_ERR(f, "Audio Unit does not support %d Hz, %d channels (%d).\n",
               rate, channels, (int)err);
        close_unit(p);
        return false;
    }

    if (p->memory_state) {
        set_state(f, p->memory_state);
        CFRelease(p->memory_state);
        p->memory_state = NULL;
    } else if (!load_state(f, p->opts->state)) {
        close_unit(p);
        return false;
    }

    err = AudioUnitInitialize(p->unit);
    if (err != noErr) {
        MP_ERR(f, "Could not initialize Audio Unit (%d).\n", (int)err);
        close_unit(p);
        return false;
    }

    UInt32 size = sizeof(p->latency);
    if (AudioUnitGetProperty(p->unit, kAudioUnitProperty_Latency,
                             kAudioUnitScope_Global, 0,
                             &p->latency, &size) != noErr)
        p->latency = 0;
    size = sizeof(p->tail);
    if (AudioUnitGetProperty(p->unit, kAudioUnitProperty_TailTime,
                             kAudioUnitScope_Global, 0,
                             &p->tail, &size) != noErr)
        p->tail = 0;

    p->max_frames = frames;
    mp_aframe_config_copy(p->format, frame);
    MP_INFO(f, "Audio Unit %s: %d Hz, %d channels, %.3f ms latency, %.3f s tail.\n",
            component_name(component).UTF8String, rate, channels,
            p->latency * 1000, p->tail);
    return true;
}

static bool render(struct mp_filter *f, struct mp_aframe *input,
                   struct mp_aframe **result)
{
    struct priv *p = f->priv;
    int channels = mp_aframe_get_channels(input);
    int samples = mp_aframe_get_size(input);
    struct mp_aframe *out = mp_aframe_new_ref(p->format);
    if (mp_aframe_pool_allocate(p->out_pool, out, samples) < 0) {
        talloc_free(out);
        return false;
    }
    mp_aframe_copy_attributes(out, input);

    size_t list_size = offsetof(AudioBufferList, mBuffers) +
                       channels * sizeof(AudioBuffer);
    AudioBufferList *buffers = alloca(list_size);
    buffers->mNumberBuffers = channels;
    uint8_t **planes = mp_aframe_get_data_rw(out);
    for (int n = 0; n < channels; n++) {
        buffers->mBuffers[n] = (AudioBuffer) {
            .mNumberChannels = 1,
            .mDataByteSize = samples * sizeof(float),
            .mData = planes[n],
        };
    }

    AudioTimeStamp timestamp = {
        .mSampleTime = p->sample_time,
        .mFlags = kAudioTimeStampSampleTimeValid,
    };
    p->input = input;
    AudioUnitRenderActionFlags flags = 0;
    OSStatus err = AudioUnitRender(p->unit, &flags, &timestamp, 0,
                                   samples, buffers);
    p->input = NULL;
    if (err != noErr) {
        MP_ERR(f, "Audio Unit render failed (%d).\n", (int)err);
        talloc_free(out);
        return false;
    }

    double pts = mp_aframe_get_pts(out);
    if (pts != MP_NOPTS_VALUE)
        mp_aframe_set_pts(out, pts - p->latency);
    p->sample_time += samples;
    *result = out;
    return true;
}

static void process(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    struct mp_frame frame = mp_pin_out_read(p->in_pin);
    if (!frame.type)
        return;
    if (frame.type != MP_FRAME_AUDIO) {
        // ponytail: tail time is reported; add EOF draining if cut effect tails
        // become a real playback problem.
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    struct mp_aframe *input = frame.data;
    if (mp_aframe_get_format(input) != AF_FORMAT_FLOATP ||
        !configure(f, input))
        goto error;

    if (p->bypass) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    struct mp_aframe *out = NULL;
    if (!render(f, input, &out))
        goto error;
    mp_frame_unref(&frame);
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, out));
    return;

error:
    mp_frame_unref(&frame);
    mp_filter_internal_mark_failed(f);
}

static NSView *custom_view(AudioUnit unit)
{
    UInt32 size = 0;
    if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI,
                                 kAudioUnitScope_Global, 0, &size, NULL) != noErr)
        return nil;
    if (size < offsetof(AudioUnitCocoaViewInfo, mCocoaAUViewClass) +
               sizeof(CFStringRef))
        return nil;

    AudioUnitCocoaViewInfo *info = malloc(size);
    if (!info)
        return nil;
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI,
                             kAudioUnitScope_Global, 0, info, &size) != noErr) {
        free(info);
        return nil;
    }

    int classes = (size - offsetof(AudioUnitCocoaViewInfo, mCocoaAUViewClass)) /
                  sizeof(CFStringRef);
    NSBundle *bundle = [NSBundle bundleWithURL:(NSURL *)
                        info->mCocoaAUViewBundleLocation];
    [bundle load];
    NSView *view = nil;
    for (int n = 0; n < classes && !view; n++) {
        Class factory_class = [bundle classNamed:(NSString *)
                               info->mCocoaAUViewClass[n]];
        id factory = [[[factory_class alloc] init] autorelease];
        if ([factory conformsToProtocol:@protocol(AUCocoaUIBase)])
            view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(640, 480)];
    }

    CFRelease(info->mCocoaAUViewBundleLocation);
    for (int n = 0; n < classes; n++)
        CFRelease(info->mCocoaAUViewClass[n]);
    free(info);
    return view;
}

static void show_ui(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->window) {
        [p->window makeKeyAndOrderFront:nil];
        return;
    }

    @autoreleasepool {
        NSView *view = custom_view(p->unit);
        if (!view)
            view = [[[AUGenericView alloc] initWithAudioUnit:p->unit] autorelease];
        NSSize size = view.frame.size;
        if (size.width < 100 || size.height < 100)
            size = NSMakeSize(640, 480);
        p->window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        p->window.releasedWhenClosed = NO;
        p->window.title = component_name(AudioComponentInstanceGetComponent(p->unit));
        p->window.contentView = view;
        [p->window center];
        [p->window makeKeyAndOrderFront:nil];
    }
}

static bool command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type == MP_FILTER_COMMAND_IS_ACTIVE) {
        cmd->is_active = !p->bypass;
        return true;
    }
    if (cmd->type != MP_FILTER_COMMAND_TEXT)
        return false;

    if (!strcmp(cmd->cmd, "show-ui")) {
        if (!p->unit)
            return false;
        if (NSThread.isMainThread)
            show_ui(f);
        else
            dispatch_sync(dispatch_get_main_queue(), ^{ show_ui(f); });
        return true;
    }
    if (!strcmp(cmd->cmd, "save-state"))
        return save_state(f, cmd->arg[0] ? cmd->arg : p->opts->state);
    if (!strcmp(cmd->cmd, "load-state")) {
        bool ok = load_state(f, cmd->arg[0] ? cmd->arg : p->opts->state);
        if (ok)
            AudioUnitReset(p->unit, kAudioUnitScope_Global, 0);
        return ok;
    }
    if (!strcmp(cmd->cmd, "set-bypass")) {
        if (!strcasecmp(cmd->arg, "toggle"))
            p->bypass = !p->bypass;
        else if (!strcasecmp(cmd->arg, "yes"))
            p->bypass = true;
        else if (!strcasecmp(cmd->arg, "no"))
            p->bypass = false;
        else
            return false;
        if (p->unit)
            AudioUnitReset(p->unit, kAudioUnitScope_Global, 0);
        return true;
    }
    return false;
}

static void reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    p->sample_time = 0;
    p->input = NULL;
    if (p->unit)
        AudioUnitReset(p->unit, kAudioUnitScope_Global, 0);
}

static void destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    close_unit(p);
    if (p->memory_state)
        CFRelease(p->memory_state);
}

static const struct mp_filter_info filter = {
    .name = "audiounit",
    .priv_size = sizeof(struct priv),
    .process = process,
    .command = command,
    .reset = reset,
    .destroy = destroy,
};

static struct mp_filter *create(struct mp_filter *parent, void *options)
{
    struct f_opts *opts = options;
    AudioComponentDescription desc;
    if (!parse_component(opts->component, &desc)) {
        MP_ERR(parent, "Audio Unit component must be 24 hexadecimal digits "
               "and identify an effect.\n");
        talloc_free(options);
        return NULL;
    }

    struct mp_filter *f = mp_filter_create(parent, &filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, opts);
    p->desc = desc;
    p->bypass = opts->bypass;
    p->format = talloc_steal(p, mp_aframe_create());
    p->out_pool = mp_aframe_pool_create(p);

    struct mp_autoconvert *conv = mp_autoconvert_create(f);
    if (!conv)
        abort();
    mp_autoconvert_add_afmt(conv, AF_FORMAT_FLOATP);
    mp_pin_connect(conv->f->pins[0], f->ppins[0]);
    p->in_pin = conv->f->pins[1];
    return f;
}

#define OPT_BASE_STRUCT struct f_opts

const struct mp_user_filter_entry af_audiounit = {
    .desc = {
        .name = "audiounit",
        .description = "macOS Audio Unit effect host",
        .priv_size = sizeof(struct f_opts),
        .options = (const struct m_option[]) {
            {"component", OPT_STRING(component)},
            {"state", OPT_STRING(state)},
            {"bypass", OPT_BOOL(bypass)},
            {0}
        },
    },
    .create = create,
};
