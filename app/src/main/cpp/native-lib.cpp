// AudioProbe native audio backends: AAudio + OpenSL ES
// Generates a 440 Hz sine (48 kHz, stereo, PCM 16-bit) to compare output routing.
#include <jni.h>
#include <android/log.h>
#include <aaudio/AAudio.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/system_properties.h>

#define LOG_TAG "AudioProbeNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static const int kRate = 48000;
static const double kFreq = 440.0;

static std::atomic<bool> gAaudioRunning{false};
static std::atomic<double> gAaudioPhase{0.0};
static AAudioStream *gAaudioStream = nullptr;

static std::atomic<bool> gSlRunning{false};
static std::atomic<double> gSlPhase{0.0};
static SLObjectItf gSlEngineObj = nullptr;
static SLEngineItf gSlEngine = nullptr;
static SLObjectItf gSlMixObj = nullptr;
static SLObjectItf gSlPlayerObj = nullptr;
static SLPlayItf gSlPlay = nullptr;
static SLAndroidSimpleBufferQueueItf gSlBq = nullptr;

static const int kSlNumBuf = 4;
static short *gSlBuf[kSlNumBuf] = {nullptr, nullptr, nullptr, nullptr};
static int gSlBufFrames = 1024;
static int gSlNext = 0;
static int gSlPerfMode = 0; // SL_ANDROID_PERFORMANCE_NONE

// Optional PCM asset (48k stereo s16le). When loaded, all backends play it
// instead of the synthetic tone so Dolby effects can be judged on real music.
static short *gPcm = nullptr;
static long gPcmFrames = 0;
static long gPcmPos = 0;
static std::atomic<bool> gUsePcm{false};

static void resetPcmPos() { gPcmPos = 0; }

static void fillSamples(short *out, int frames) {
    if (gUsePcm.load() && gPcm && gPcmFrames > 0) {
        long f = gPcmFrames;
        for (int i = 0; i < frames; ++i) {
            long idx = gPcmPos % f;
            out[i * 2] = gPcm[idx * 2];
            out[i * 2 + 1] = gPcm[idx * 2 + 1];
            gPcmPos++;
        }
        return;
    }
    double p = gAaudioPhase.load(std::memory_order_relaxed);
    const double w1 = 2.0 * M_PI * 60.0 / kRate;
    const double w2 = 2.0 * M_PI * 440.0 / kRate;
    const double w3 = 2.0 * M_PI * 6000.0 / kRate;
    for (int i = 0; i < frames; ++i) {
        double sample = 0.32 * std::sin(p + i * w1)
                      + 0.18 * std::sin(i * w2)
                      + 0.07 * std::sin(i * w3);
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        short sv = (short)(sample * 32767.0 * 0.7);
        out[i * 2] = sv;
        out[i * 2 + 1] = sv;
    }
    p += frames * w1;
    if (p > 2.0 * M_PI) p -= 2.0 * M_PI * std::floor(p / (2.0 * M_PI));
    gAaudioPhase.store(p, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_audioprobe_NativeAudio_loadPcm(JNIEnv *env, jclass clazz, jbyteArray data) {
    (void) clazz;
    jsize n = env->GetArrayLength(data);
    jbyte *b = env->GetByteArrayElements(data, nullptr);
    if (gPcm) { free(gPcm); gPcm = nullptr; }
    gPcm = (short *) malloc(n);
    if (gPcm) {
        memcpy(gPcm, b, n);
        gPcmFrames = n / 4; // 2 channels * 2 bytes
        gPcmPos = 0;
        gUsePcm.store(true);
    }
    env->ReleaseByteArrayElements(data, b, JNI_ABORT);
    LOGI("loadPcm bytes=%d frames=%ld", (int) n, gPcmFrames);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_audioprobe_NativeAudio_pcmFrames(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    return (jint) gPcmFrames;
}

static void fillSine(short *out, int frames, std::atomic<double> &phase) {
    double p = phase.load(std::memory_order_relaxed);
    // Revealing test signal: 60 Hz bass + 440 Hz mid + 6 kHz high.
    const double w1 = 2.0 * M_PI * 60.0 / kRate;
    const double w2 = 2.0 * M_PI * 440.0 / kRate;
    const double w3 = 2.0 * M_PI * 6000.0 / kRate;
    for (int i = 0; i < frames; ++i) {
        double sample = 0.32 * std::sin(p + i * w1)
                      + 0.18 * std::sin(i * w2)
                      + 0.07 * std::sin(i * w3);
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        short sv = (short)(sample * 32767.0 * 0.7);
        out[i * 2] = sv;
        out[i * 2 + 1] = sv;
    }
    p += frames * w1;
    if (p > 2.0 * M_PI) p -= 2.0 * M_PI * std::floor(p / (2.0 * M_PI));
    phase.store(p, std::memory_order_relaxed);
}

// ---------------- AAudio ----------------
static aaudio_data_callback_result_t aaudioCb(AAudioStream *stream, void *userData,
                                              void *audioData, int32_t numFrames) {
    (void) stream; (void) userData;
    if (!gAaudioRunning.load()) return AAUDIO_CALLBACK_RESULT_STOP;
    fillSamples((short *) audioData, numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_result_t tryOpen(int perfMode, int sharingMode, int format, int rate, int channels,
                               AAudioStream **outStream) {
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, format);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setSampleRate(builder, rate);
    AAudioStreamBuilder_setPerformanceMode(builder, perfMode);
    AAudioStreamBuilder_setSharingMode(builder, sharingMode);
    AAudioStreamBuilder_setDataCallback(builder, aaudioCb, nullptr);
    r = AAudioStreamBuilder_openStream(builder, outStream);
    AAudioStreamBuilder_delete(builder);
    return r;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_audioprobe_NativeAudio_startAaudioEx(JNIEnv *env, jobject thiz, jint perfMode,
                                              jint sharingMode, jint format, jint rate, jint channels) {
    (void) env; (void) thiz;
    if (gAaudioStream) return -1;
    AAudioStream *stream = nullptr;
    aaudio_result_t r = tryOpen(perfMode, sharingMode, format, rate, channels, &stream);
    LOGI("AAudio try perf=%d sharing=%d fmt=%d rate=%d ch=%d -> %d",
         perfMode, sharingMode, format, rate, channels, (int) r);
    if (r != AAUDIO_OK || stream == nullptr) return (int) r;
    gAaudioStream = stream;
    int sessionId = AAudioStream_getSessionId(stream);
    LOGI("AAudio OPENED perf=%d sharing=%d fmt=%d rate=%d ch=%d sessionId=%d "
         "(negotiated perf=%d sharing=%d fmt=%d rate=%d ch=%d)",
         perfMode, sharingMode, format, rate, channels, sessionId,
         AAudioStream_getPerformanceMode(stream), AAudioStream_getSharingMode(stream),
         AAudioStream_getFormat(stream), AAudioStream_getSampleRate(stream),
         AAudioStream_getChannelCount(stream));
    gAaudioRunning.store(true);
    gAaudioPhase.store(0.0);
    resetPcmPos();
    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) { LOGI("requestStart failed %d", r); return -4; }
    return sessionId;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_audioprobe_NativeAudio_startAaudio(JNIEnv *env, jobject thiz, jint performanceMode) {
    (void) env; (void) thiz;
    if (gAaudioStream) return -1;
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK) { LOGI("createStreamBuilder failed %d", r); return -2; }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, kRate);
    AAudioStreamBuilder_setPerformanceMode(builder, performanceMode);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, aaudioCb, nullptr);
    r = AAudioStreamBuilder_openStream(builder, &gAaudioStream);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK) {
        LOGI("openStream failed %d", r);
        gAaudioStream = nullptr;
        return -3;
    }
    int gotPerf = AAudioStream_getPerformanceMode(gAaudioStream);
    int gotRate = AAudioStream_getSampleRate(gAaudioStream);
    int gotFmt = AAudioStream_getFormat(gAaudioStream);
    int gotSharing = AAudioStream_getSharingMode(gAaudioStream);
    int sessionId = AAudioStream_getSessionId(gAaudioStream);
    int xrun = AAudioStream_getXRunCount(gAaudioStream);
    LOGI("AAudio stream open: perfMode=%d (req %d) rate=%d fmt=%d sharing=%d sessionId=%d xrun=%d",
         gotPerf, performanceMode, gotRate, gotFmt, gotSharing, sessionId, xrun);
    gAaudioRunning.store(true);
    gAaudioPhase.store(0.0);
    resetPcmPos();
    r = AAudioStream_requestStart(gAaudioStream);
    if (r != AAUDIO_OK) { LOGI("requestStart failed %d", r); return -4; }
    return sessionId;
}

extern "C" JNIEXPORT void JNICALL
Java_com_audioprobe_NativeAudio_stopAaudio(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    if (!gAaudioStream) return;
    gAaudioRunning.store(false);
    AAudioStream_requestStop(gAaudioStream);
    AAudioStream_close(gAaudioStream);
    gAaudioStream = nullptr;
    LOGI("AAudio stopped");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_audioprobe_NativeAudio_aaudioProps(JNIEnv *env, jobject thiz) {
    (void) thiz;
    auto get = [](const char *name) -> std::string {
        char v[128] = {0};
        __system_property_get(name, v);
        return std::string(v);
    };
    std::string s = "aaudio.mmap_policy=" + get("aaudio.mmap_policy") +
                    " mmap_exclusive_policy=" + get("aaudio.mmap_exclusive_policy") +
                    " hw_burst_min_usec=" + get("aaudio.hw_burst_min_usec");
    return env->NewStringUTF(s.c_str());
}

// ---------------- OpenSL ES ----------------
static void slFeed(SLAndroidSimpleBufferQueueItf bq, void *ctx) {
    (void) ctx;
    if (!gSlRunning.load()) return;
    short *buf = gSlBuf[gSlNext];
    gSlNext = (gSlNext + 1) % kSlNumBuf;
    fillSamples(buf, gSlBufFrames);
    (*bq)->Enqueue(bq, buf, (SLuint32)(gSlBufFrames * 2 * sizeof(short)));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_audioprobe_NativeAudio_startOpenSL(JNIEnv *env, jobject thiz);

extern "C" JNIEXPORT jint JNICALL
Java_com_audioprobe_NativeAudio_startOpenSLEx(JNIEnv *env, jobject thiz, jint frames, jint perfMode) {
    if (frames > 0) gSlBufFrames = frames;
    gSlPerfMode = perfMode;
    return Java_com_audioprobe_NativeAudio_startOpenSL(env, thiz);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_audioprobe_NativeAudio_startOpenSL(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    if (gSlPlayerObj) return -1;
    SLresult r;
    r = slCreateEngine(&gSlEngineObj, 0, nullptr, 0, nullptr, nullptr);
    if (r != SL_RESULT_SUCCESS) return -2;
    (*gSlEngineObj)->Realize(gSlEngineObj, SL_BOOLEAN_FALSE);
    (*gSlEngineObj)->GetInterface(gSlEngineObj, SL_IID_ENGINE, &gSlEngine);

    r = (*gSlEngine)->CreateOutputMix(gSlEngine, &gSlMixObj, 0, nullptr, nullptr);
    if (r != SL_RESULT_SUCCESS) return -3;
    (*gSlMixObj)->Realize(gSlMixObj, SL_BOOLEAN_FALSE);

    SLDataLocator_AndroidSimpleBufferQueue lq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kSlNumBuf};
    SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_48, SL_PCMSAMPLEFORMAT_FIXED_16,
                            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&lq, &fmt};
    SLDataLocator_OutputMix lom = {SL_DATALOCATOR_OUTPUTMIX, gSlMixObj};
    SLDataSink sink = {&lom, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME, SL_IID_ANDROIDCONFIGURATION};
    const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
    r = (*gSlEngine)->CreateAudioPlayer(gSlEngine, &gSlPlayerObj, &src, &sink, 3, ids, req);
    if (r != SL_RESULT_SUCCESS) return -4;
    // Must configure BEFORE Realize.
    SLAndroidConfigurationItf cfg = nullptr;
    if ((*gSlPlayerObj)->GetInterface(gSlPlayerObj, SL_IID_ANDROIDCONFIGURATION, &cfg) == SL_RESULT_SUCCESS && cfg) {
        SLint32 streamType = SL_ANDROID_STREAM_MEDIA;
        (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_STREAM_TYPE, &streamType, sizeof(streamType));
        SLuint32 perf = (SLuint32) gSlPerfMode;
        (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_PERFORMANCE_MODE, &perf, sizeof(perf));
    }
    (*gSlPlayerObj)->Realize(gSlPlayerObj, SL_BOOLEAN_FALSE);
    (*gSlPlayerObj)->GetInterface(gSlPlayerObj, SL_IID_PLAY, &gSlPlay);
    (*gSlPlayerObj)->GetInterface(gSlPlayerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &gSlBq);

    if (gSlBuf[0]) { for (int i = 0; i < kSlNumBuf; ++i) { free(gSlBuf[i]); gSlBuf[i] = nullptr; } }
    for (int i = 0; i < kSlNumBuf; ++i) {
        gSlBuf[i] = (short *) malloc(gSlBufFrames * 2 * sizeof(short));
        memset(gSlBuf[i], 0, gSlBufFrames * 2 * sizeof(short));
    }
    gSlNext = 0;
    gSlPhase.store(0.0);
    gSlRunning.store(true);
    (*gSlBq)->RegisterCallback(gSlBq, slFeed, nullptr);
    (*gSlPlay)->SetPlayState(gSlPlay, SL_PLAYSTATE_PLAYING);
    for (int i = 0; i < kSlNumBuf; ++i) slFeed(gSlBq, nullptr);
    LOGI("OpenSL player started, buffers=%d frames=%d rate=%d", kSlNumBuf, gSlBufFrames, kRate);
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_audioprobe_NativeAudio_stopOpenSL(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    if (!gSlPlayerObj) return;
    gSlRunning.store(false);
    if (gSlPlay) (*gSlPlay)->SetPlayState(gSlPlay, SL_PLAYSTATE_STOPPED);
    (*gSlPlayerObj)->Destroy(gSlPlayerObj);
    gSlPlayerObj = nullptr; gSlPlay = nullptr; gSlBq = nullptr;
    (*gSlMixObj)->Destroy(gSlMixObj); gSlMixObj = nullptr;
    (*gSlEngineObj)->Destroy(gSlEngineObj); gSlEngineObj = nullptr; gSlEngine = nullptr;
    for (int i = 0; i < kSlNumBuf; ++i) { free(gSlBuf[i]); gSlBuf[i] = nullptr; }
    LOGI("OpenSL stopped");
}
