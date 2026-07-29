/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

// Steinberg ASIO host-side interface re-declared in C using the public
// vtbl ABI (IASIO derives from IUnknown). No Steinberg SDK header is
// included; this header is a clean-room ABI redeclaration, similar to
// the approach used by PortAudio's pa_asio.cpp and JACK's ASIO host.

#ifndef MP_AO_ASIO_H_
#define MP_AO_ASIO_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <windows.h>
#include <objbase.h>

#include "common/msg.h"
#include "osdep/windows_utils.h"
#include "internal.h"
#include "ao.h"

// Sample-type converter signature: writes `n` samples from planar f32
// src into the ASIO output buffer dst, scaled by `vol`.
struct asio_state;
typedef void (*asio_convert_fn)(void *dst, const float *src, int n, float vol);

// --- ASIO ABI constants (clean-room redeclarations) ---

typedef long ASIOBool;
#define ASIOFalse 0L
#define ASIOTrue  1L

typedef long ASIOError;
enum {
    ASE_OK               = 0,
    ASE_SUCCESS          = 0x3f4847a0,
    ASE_NotPresent       = -1000,
    ASE_HWMalfunction    = -999,
    ASE_InvalidParameter = -998,
    ASE_InvalidMode      = -997,
    ASE_SPNotAdvancing   = -996,
    ASE_NoClock          = -995,
    ASE_NoMemory         = -994,
};

typedef long ASIOSampleType;
enum {
    ASIOSTInt16MSB   = 0,
    ASIOSTInt24MSB   = 1,
    ASIOSTInt32MSB   = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,

    ASIOSTInt16LSB   = 16,
    ASIOSTInt24LSB   = 17,
    ASIOSTInt32LSB   = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,
};

// On Win32, NATIVE_INT64 and IEEE754_64FLOAT are both effectively true.
typedef int64_t ASIOSamples;
typedef int64_t ASIOTimeStamp;
typedef double  ASIOSampleRate;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long     channelNum;
    void    *buffers[2];
} ASIOBufferInfo;

typedef struct ASIOChannelInfo {
    long           channel;
    ASIOBool       isInput;
    ASIOBool       isActive;
    long           channelGroup;
    ASIOSampleType type;
    char           name[32];
} ASIOChannelInfo;

typedef struct ASIOClockSource {
    long      index;
    long      associatedChannel;
    long      associatedGroup;
    ASIOBool  isCurrentSource;
    char      name[32];
} ASIOClockSource;

typedef struct ASIOTimeCode {
    double         speed;
    ASIOSamples    timeCodeSamples;
    unsigned long  flags;
    char           future[64];
} ASIOTimeCode;

typedef struct AsioTimeInfo {
    double          speed;
    ASIOTimeStamp   systemTime;
    ASIOSamples     samplePosition;
    ASIOSampleRate  sampleRate;
    unsigned long   flags;
    char            reserved[12];
} AsioTimeInfo;

typedef struct ASIOTime {
    long          reserved[4];
    AsioTimeInfo  timeInfo;
    ASIOTimeCode  timeCode;
} ASIOTime;

typedef struct ASIODriverInfo {
    long  asioVersion;
    long  driverVersion;
    char  name[32];
    char  errorMessage[124];
    void *sysRef;
} ASIODriverInfo;

typedef struct ASIOCallbacks {
    void      (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void      (*sampleRateDidChange)(ASIOSampleRate sRate);
    long      (*asioMessage)(long selector, long value, void *message,
                             double *opt);
    ASIOTime *(*bufferSwitchTimeInfo)(ASIOTime *params, long doubleBufferIndex,
                                      ASIOBool directProcess);
} ASIOCallbacks;

// asioMessage selectors
enum {
    kAsioSelectorSupported    = 1,
    kAsioEngineVersion        = 2,
    kAsioResetRequest         = 3,
    kAsioBufferSizeChange     = 4,
    kAsioResyncRequest        = 5,
    kAsioLatenciesChanged     = 6,
    kAsioSupportsTimeInfo     = 7,
    kAsioSupportsTimeCode     = 8,
    kAsioMMCCommand           = 9,
    kAsioSupportsInputMonitor = 10,
    kAsioSupportsInputGain    = 11,
    kAsioSupportsInputMeter   = 12,
    kAsioSupportsOutputGain   = 13,
    kAsioSupportsOutputMeter  = 14,
    kAsioOverload             = 15,
};

// AsioTimeInfo.flags
enum {
    kSystemTimeValid     = 1 << 0,
    kSamplePositionValid = 1 << 1,
    kSampleRateValid     = 1 << 2,
    kSpeedValid          = 1 << 3,
    kSampleRateChanged   = 1 << 4,
    kClockSourceChanged  = 1 << 5,
};

// --- IASIO interface in C (COM-style vtbl ABI) ---
//
// Each ASIO driver implements IASIO with a vtbl in the same layout the
// C++ "interface IASIO : public IUnknown" produces on MSVC/MinGW (vtbl
// pointer first, IUnknown methods at slots 0..2, IASIO methods after).

typedef struct IASIO IASIO;

typedef struct IASIOVtbl {
    // IUnknown
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IASIO *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IASIO *);
    ULONG   (STDMETHODCALLTYPE *Release)(IASIO *);
    // IASIO
    ASIOBool (STDMETHODCALLTYPE *init)(IASIO *, void *sysHandle);
    void     (STDMETHODCALLTYPE *getDriverName)(IASIO *, char *name);
    long     (STDMETHODCALLTYPE *getDriverVersion)(IASIO *);
    void     (STDMETHODCALLTYPE *getErrorMessage)(IASIO *, char *string);
    ASIOError(STDMETHODCALLTYPE *start)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *stop)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *getChannels)(IASIO *, long *in, long *out);
    ASIOError(STDMETHODCALLTYPE *getLatencies)(IASIO *, long *in, long *out);
    ASIOError(STDMETHODCALLTYPE *getBufferSize)(IASIO *, long *min, long *max,
                                                long *pref, long *gran);
    ASIOError(STDMETHODCALLTYPE *canSampleRate)(IASIO *, ASIOSampleRate);
    ASIOError(STDMETHODCALLTYPE *getSampleRate)(IASIO *, ASIOSampleRate *);
    ASIOError(STDMETHODCALLTYPE *setSampleRate)(IASIO *, ASIOSampleRate);
    ASIOError(STDMETHODCALLTYPE *getClockSources)(IASIO *, ASIOClockSource *,
                                                  long *num);
    ASIOError(STDMETHODCALLTYPE *setClockSource)(IASIO *, long ref);
    ASIOError(STDMETHODCALLTYPE *getSamplePosition)(IASIO *, ASIOSamples *,
                                                    ASIOTimeStamp *);
    ASIOError(STDMETHODCALLTYPE *getChannelInfo)(IASIO *, ASIOChannelInfo *);
    ASIOError(STDMETHODCALLTYPE *createBuffers)(IASIO *, ASIOBufferInfo *,
                                                long n, long size,
                                                ASIOCallbacks *);
    ASIOError(STDMETHODCALLTYPE *disposeBuffers)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *controlPanel)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *future)(IASIO *, long selector, void *opt);
    ASIOError(STDMETHODCALLTYPE *outputReady)(IASIO *);
} IASIOVtbl;

struct IASIO {
    const IASIOVtbl *lpVtbl;
};

#define IASIO_QueryInterface(p,iid,out)   ((p)->lpVtbl->QueryInterface((p),(iid),(out)))
#define IASIO_AddRef(p)                   ((p)->lpVtbl->AddRef((p)))
#define IASIO_Release(p)                  ((p)->lpVtbl->Release((p)))
#define IASIO_Init(p,h)                   ((p)->lpVtbl->init((p),(h)))
#define IASIO_GetDriverName(p,buf)        ((p)->lpVtbl->getDriverName((p),(buf)))
#define IASIO_GetDriverVersion(p)         ((p)->lpVtbl->getDriverVersion((p)))
#define IASIO_GetErrorMessage(p,buf)      ((p)->lpVtbl->getErrorMessage((p),(buf)))
#define IASIO_Start(p)                    ((p)->lpVtbl->start((p)))
#define IASIO_Stop(p)                     ((p)->lpVtbl->stop((p)))
#define IASIO_GetChannels(p,i,o)          ((p)->lpVtbl->getChannels((p),(i),(o)))
#define IASIO_GetLatencies(p,i,o)         ((p)->lpVtbl->getLatencies((p),(i),(o)))
#define IASIO_GetBufferSize(p,a,b,c,d)    ((p)->lpVtbl->getBufferSize((p),(a),(b),(c),(d)))
#define IASIO_CanSampleRate(p,r)          ((p)->lpVtbl->canSampleRate((p),(r)))
#define IASIO_GetSampleRate(p,r)          ((p)->lpVtbl->getSampleRate((p),(r)))
#define IASIO_SetSampleRate(p,r)          ((p)->lpVtbl->setSampleRate((p),(r)))
#define IASIO_GetSamplePosition(p,sp,ts)  ((p)->lpVtbl->getSamplePosition((p),(sp),(ts)))
#define IASIO_GetChannelInfo(p,info)      ((p)->lpVtbl->getChannelInfo((p),(info)))
#define IASIO_CreateBuffers(p,bi,n,s,cb)  ((p)->lpVtbl->createBuffers((p),(bi),(n),(s),(cb)))
#define IASIO_DisposeBuffers(p)           ((p)->lpVtbl->disposeBuffers((p)))
#define IASIO_ControlPanel(p)             ((p)->lpVtbl->controlPanel((p)))
#define IASIO_Future(p,sel,opt)           ((p)->lpVtbl->future((p),(sel),(opt)))
#define IASIO_OutputReady(p)              ((p)->lpVtbl->outputReady((p)))

#endif /* MP_AO_ASIO_H_ */
