package com.audioprobe;

public final class NativeAudio {
    static {
        System.loadLibrary("audioprobe");
    }

    private NativeAudio() {}

    // AAudio
    public static native int startAaudio(int performanceMode);
    public static native void stopAaudio();
    public static native String aaudioProps();

    // OpenSL ES
    public static native int startOpenSL();
    public static native int startOpenSLEx(int frames, int perfMode);
    public static native void stopOpenSL();

    // PCM asset (48k stereo s16le) - when loaded, replaces the synthetic tone
    public static native void loadPcm(byte[] data);
    public static native int pcmFrames();

    // AAudio experimental: direct control of perf/sharing/format/rate/channels
    public static native int startAaudioEx(int perfMode, int sharingMode, int format, int rate, int channels);
}
