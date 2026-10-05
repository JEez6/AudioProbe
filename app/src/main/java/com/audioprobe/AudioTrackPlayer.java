package com.audioprobe;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

/**
 * Plays the same 440 Hz sine (48 kHz stereo PCM16) through different Java AudioTrack
 * construction paths so we can compare AudioFlinger routing.
 */
public final class AudioTrackPlayer {
    private static final String TAG = "AudioProbe";

    public static final int VARIANT_LEGACY_DEFAULT = 0;   // deprecated ctor, STREAM_MUSIC, default buffer
    public static final int VARIANT_BUILDER_NORMAL = 1;   // Builder, PERFORMANCE_MODE_NONE, big buffer
    public static final int VARIANT_BUILDER_LOWLAT = 2;   // Builder, PERFORMANCE_MODE_LOW_LATENCY, small buffer

    private static final int RATE = 48000;
    private static final double FREQ = 440.0;

    // Shared PCM song (48k stereo s16) so Java AudioTrack plays the same material.
    private static short[] SONG;
    private static int songPos;
    public static synchronized void setSong(short[] s) { SONG = s; songPos = 0; }
    public static synchronized void resetSong() { songPos = 0; }

    private AudioTrack track;
    private Thread writer;
    private volatile boolean running;
    private int variant = -1;
    private int sessionId = 0;
    private StringBuilder info = new StringBuilder();

    public int getVariant() { return variant; }
    public int getSessionId() { return sessionId; }
    public String getInfo() { return info.toString(); }
    public boolean isRunning() { return running; }

    public synchronized int start(int which) {
        stop();
        info = new StringBuilder();
        AudioTrack t;
        if (which == VARIANT_LEGACY_DEFAULT) {
            int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT);
            int buf = min * 4;
            t = new AudioTrack(AudioManager.STREAM_MUSIC, RATE, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT, buf, AudioTrack.MODE_STREAM);
            info.append("legacy ctor: min=").append(min).append(" buf=").append(buf);
        } else {
            AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build();
            AudioFormat fmt = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build();
            int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT);
            int perf;
            int buf;
            if (which == VARIANT_BUILDER_LOWLAT) {
                perf = AudioTrack.PERFORMANCE_MODE_LOW_LATENCY;
                buf = min * 2;
            } else {
                perf = AudioTrack.PERFORMANCE_MODE_NONE;
                buf = Math.max(min * 8, 32768);
            }
            t = new AudioTrack.Builder()
                    .setAudioAttributes(attrs)
                    .setAudioFormat(fmt)
                    .setBufferSizeInBytes(buf)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setPerformanceMode(perf)
                    .build();
            info.append("builder: perf=").append(perf).append(" min=").append(min).append(" buf=").append(buf);
        }
        if (t.getState() != AudioTrack.STATE_INITIALIZED) {
            info.append(" state=UNINITIALIZED");
            t.release();
            return -1;
        }
        track = t;
        variant = which;
        sessionId = t.getAudioSessionId();
        info.append(" state=INITIALIZED session=").append(sessionId)
            .append(" rate=").append(t.getSampleRate())
            .append(" fmt=").append(t.getFormat());
        running = true;
        t.play();
        writer = new Thread(this::loop, "ap-writer");
        writer.start();
        Log.i(TAG, "AudioTrack start variant=" + which + " " + info);
        return sessionId;
    }

    private void loop() {
        final int frames = 960;
        short[] buf = new short[frames * 2];
        double p = 0;
        final double w1 = 2.0 * Math.PI * 60.0 / RATE;
        final double w2 = 2.0 * Math.PI * 440.0 / RATE;
        final double w3 = 2.0 * Math.PI * 6000.0 / RATE;
        final short[] songRef = SONG;
        final int songLen = songRef == null ? 0 : songRef.length;
        int pos = songPos;
        while (running) {
            if (songRef != null && songLen > 0) {
                for (int i = 0; i < frames; i++) {
                    int idx = pos % songLen;
                    buf[i * 2] = songRef[idx];
                    buf[i * 2 + 1] = idx + 1 < songLen ? songRef[idx + 1] : songRef[idx];
                    idx += 2;
                    pos = idx;
                }
            } else {
                for (int i = 0; i < frames; i++) {
                    double s = 0.32 * Math.sin(p + i * w1) + 0.18 * Math.sin(i * w2) + 0.07 * Math.sin(i * w3);
                    if (s > 1.0) s = 1.0;
                    if (s < -1.0) s = -1.0;
                    short v = (short) (s * 32767.0 * 0.7);
                    buf[i * 2] = v;
                    buf[i * 2 + 1] = v;
                }
                p += frames * w1;
                if (p > 2.0 * Math.PI) p -= 2.0 * Math.PI * Math.floor(p / (2.0 * Math.PI));
            }
            int w = track.write(buf, 0, buf.length);
            if (w < 0) {
                Log.i(TAG, "write error " + w);
                break;
            }
        }
        songPos = pos;
    }

    public synchronized void stop() {
        running = false;
        if (writer != null) {
            try { writer.join(300); } catch (InterruptedException ignored) {}
            writer = null;
        }
        if (track != null) {
            try { track.pause(); track.flush(); track.stop(); } catch (Throwable ignored) {}
            track.release();
            track = null;
        }
        variant = -1;
        sessionId = 0;
    }
}
