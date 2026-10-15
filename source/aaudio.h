/* aaudio.h -- minimal NDK AAudio ABI shim, backed by libnx audren.
 *
 * libemucore.so (DT_NEEDED libaaudio) drives audio through the AetherSX2
 * "AAudioMod" pull-callback backend: it registers a data callback and the audio
 * device thread calls it to FILL interleaved stereo frames. We implement just
 * the ~19 symbols the core imports over one audren FLOAT stereo voice @ 48000.
 *
 * The enum values + struct layouts must match the NDK <aaudio/AAudio.h> ABI the
 * core was compiled against.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __AAUDIO_H__
#define __AAUDIO_H__

#include <stdint.h>

typedef int32_t aaudio_result_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_stream_state_t;
typedef int32_t aaudio_data_callback_result_t;

enum {
  AAUDIO_OK = 0,
  AAUDIO_ERROR_BASE = -900,
  AAUDIO_ERROR_DISCONNECTED = -899,
  AAUDIO_ERROR_INVALID_STATE = -895,
  AAUDIO_ERROR_INVALID_HANDLE = -892,
  AAUDIO_ERROR_UNAVAILABLE = -878,
  AAUDIO_ERROR_NO_FREE_HANDLES = -877,
  AAUDIO_ERROR_NO_MEMORY = -876,
  AAUDIO_ERROR_NULL = -875,
  AAUDIO_ERROR_TIMEOUT = -874,
};

enum {
  AAUDIO_FORMAT_INVALID = -1,
  AAUDIO_FORMAT_UNSPECIFIED = 0,
  AAUDIO_FORMAT_PCM_I16 = 1,
  AAUDIO_FORMAT_PCM_FLOAT = 2,
};

enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_DIRECTION_INPUT = 1 };
enum { AAUDIO_SHARING_MODE_EXCLUSIVE = 0, AAUDIO_SHARING_MODE_SHARED = 1 };
enum {
  AAUDIO_PERFORMANCE_MODE_NONE = 10,
  AAUDIO_PERFORMANCE_MODE_POWER_SAVING = 11,
  AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
};
enum {
  AAUDIO_STREAM_STATE_UNINITIALIZED = 0,
  AAUDIO_STREAM_STATE_UNKNOWN = 1,
  AAUDIO_STREAM_STATE_OPEN = 2,
  AAUDIO_STREAM_STATE_STARTING = 3,
  AAUDIO_STREAM_STATE_STARTED = 4,
  AAUDIO_STREAM_STATE_PAUSING = 5,
  AAUDIO_STREAM_STATE_PAUSED = 6,
  AAUDIO_STREAM_STATE_FLUSHING = 7,
  AAUDIO_STREAM_STATE_FLUSHED = 8,
  AAUDIO_STREAM_STATE_STOPPING = 9,
  AAUDIO_STREAM_STATE_STOPPED = 10,
  AAUDIO_STREAM_STATE_CLOSING = 11,
  AAUDIO_STREAM_STATE_CLOSED = 12,
  AAUDIO_STREAM_STATE_DISCONNECTED = 13,
};
enum { AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP = 1 };

typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;

typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(
    AAudioStream *stream, void *userData, void *audioData, int32_t numFrames);
typedef void (*AAudioStream_errorCallback)(
    AAudioStream *stream, void *userData, aaudio_result_t error);

// ---- builder ----
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder *b, int32_t deviceId);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t d);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t m);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t sampleRate);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t channelCount);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t format);
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *b, int32_t frames);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t m);
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder *b, int32_t frames);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *user);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *user);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **stream);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b);

// ---- stream ----
aaudio_result_t AAudioStream_requestStart(AAudioStream *s);
aaudio_result_t AAudioStream_requestPause(AAudioStream *s);
aaudio_result_t AAudioStream_requestStop(AAudioStream *s);
aaudio_result_t AAudioStream_close(AAudioStream *s);
int64_t AAudioStream_getFramesRead(AAudioStream *s);
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s, aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t *nextState, int64_t timeoutNanoseconds);

// Plays silence instead of the core's audio while set (tico's menu).
void aaudio_set_muted(int muted);

#endif
