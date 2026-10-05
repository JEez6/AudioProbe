package com.audioprobe;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.media.AudioManager;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.Toast;

public class MainActivity extends Activity {
    private static final String TAG = "AudioProbe";

    // Backend ids chosen in the radio group
    private static final int ID_AT_LEGACY = 1;
    private static final int ID_AT_NORMAL = 2;
    private static final int ID_AT_LOWLAT = 3;
    private static final int ID_AAUDIO_LOWLAT = 4;
    private static final int ID_AAUDIO_NORMAL = 5;
    private static final int ID_OPENSL = 6;
    private static final int ID_OPENSL_LARGE = 7;
    private static final int ID_OPENSL_LE = 8;

    private AudioTrackPlayer javaPlayer;
    private int currentBackend = -1;
    private boolean running = false;

    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        javaPlayer = new AudioTrackPlayer();
        status = findViewById(R.id.status);
        setVolumeMax();

        ((RadioGroup) findViewById(R.id.backendGroup)).setOnCheckedChangeListener(
                (group, checkedId) -> status.setText("已选择: " + backendName(radioToBackend(checkedId)) + "\n未播放。"));

        findViewById(R.id.toggleBtn).setOnClickListener(v -> {
            if (running) stopPlayback();
            else startPlayback();
        });
        findViewById(R.id.copyBtn).setOnClickListener(v -> copyStatus());

        // Scripted control: am start -n com.audioprobe/.MainActivity --ei backend 6 --ez play true
        int b = getIntent().getIntExtra("backend", -1);
        boolean autoplay = getIntent().getBooleanExtra("play", false);
        android.util.Log.i(TAG, "onCreate backend=" + b + " play=" + autoplay);
        if (b > 0) {
            int rb = backendToRadio(b);
            if (rb != 0) ((RadioGroup) findViewById(R.id.backendGroup)).check(rb);
            if (autoplay) status.setText("收到 backend=" + b + "，加载歌曲后播放...");
        } else {
            status.setText("未检测到 backend extra (backend=" + b + ")");
        }

        new Thread(() -> {
            loadSong();
            if (b > 0 && autoplay) runOnUiThread(this::startPlayback);
        }, "ap-songload").start();
    }

    private volatile boolean songLoaded = false;

    private void loadSong() {
        try {
            java.io.InputStream is = getAssets().open("moonlight.pcm");
            java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream(26 * 1024 * 1024);
            byte[] tmp = new byte[1 << 16];
            int n;
            while ((n = is.read(tmp)) > 0) bos.write(tmp, 0, n);
            is.close();
            byte[] data = bos.toByteArray();
            NativeAudio.loadPcm(data);
            // Also share with Java AudioTrack (little-endian s16 stereo)
            short[] shorts = new short[data.length / 2];
            for (int i = 0; i < shorts.length; i++) {
                shorts[i] = (short) ((data[i * 2] & 0xff) | (data[i * 2 + 1] << 8));
            }
            AudioTrackPlayer.setSong(shorts);
            songLoaded = true;
            final int frames = NativeAudio.pcmFrames();
            runOnUiThread(() -> status.append("\n[歌曲已加载 moonlight.pcm frames=" + frames + "]"));

        } catch (Throwable t) {
            runOnUiThread(() -> status.append("\n[歌曲加载失败, 用测试音: " + t + "]"));
        }
    }

    private static int backendToRadio(int backend) {
        switch (backend) {
            case ID_AT_LEGACY: return R.id.rbAtLegacy;
            case ID_AT_NORMAL: return R.id.rbAtNormal;
            case ID_AT_LOWLAT: return R.id.rbAtLowlat;
            case ID_AAUDIO_LOWLAT: return R.id.rbAaudioLowlat;
            case ID_AAUDIO_NORMAL: return R.id.rbAaudioNormal;
            case ID_OPENSL: return R.id.rbOpensl;
            case ID_OPENSL_LARGE: return R.id.rbOpenslLarge;
            case ID_OPENSL_LE: return R.id.rbOpenslLe;
            default: return 0;
        }
    }

    private static int radioToBackend(int radioId) {
        if (radioId == R.id.rbAtLegacy) return ID_AT_LEGACY;
        if (radioId == R.id.rbAtNormal) return ID_AT_NORMAL;
        if (radioId == R.id.rbAtLowlat) return ID_AT_LOWLAT;
        if (radioId == R.id.rbAaudioLowlat) return ID_AAUDIO_LOWLAT;
        if (radioId == R.id.rbAaudioNormal) return ID_AAUDIO_NORMAL;
        if (radioId == R.id.rbOpensl) return ID_OPENSL;
        if (radioId == R.id.rbOpenslLarge) return ID_OPENSL_LARGE;
        if (radioId == R.id.rbOpenslLe) return ID_OPENSL_LE;
        return -1;
    }

    private void setVolumeMax() {
        try {
            AudioManager am = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
            int max = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
            am.setStreamVolume(AudioManager.STREAM_MUSIC, Math.min(max, max * 30 / 100), 0);
        } catch (Throwable ignored) {}
    }

    private int selectedBackend() {
        int id = ((RadioGroup) findViewById(R.id.backendGroup)).getCheckedRadioButtonId();
        return id == View.NO_ID ? ID_AT_NORMAL : radioToBackend(id);
    }

    private static String backendName(int id) {
        switch (id) {
            case ID_AT_LEGACY: return "Java AudioTrack 旧构造(STREAM_MUSIC)";
            case ID_AT_NORMAL: return "Java AudioTrack Builder(普通)";
            case ID_AT_LOWLAT: return "Java AudioTrack Builder(低延迟)";
            case ID_AAUDIO_LOWLAT: return "AAudio低延迟";
            case ID_AAUDIO_NORMAL: return "AAudio普通";
            case ID_OPENSL: return "OpenSL ES";
            case ID_OPENSL_LARGE: return "OpenSL ES(大缓冲)";
            case ID_OPENSL_LE: return "OpenSL ES(LATENCY_EFFECTS)";
            default: return "?";
        }
    }

    private int aaLastSid = -1;

    private String probeAaudio(int perf) {
        // AAudio enums (perf modes are 10/11/12)
        final int SHARED = 1;
        final int FMT_I16 = 1;
        int sid = NativeAudio.startAaudioEx(perf, SHARED, FMT_I16, 48000, 2);
        aaLastSid = sid;
        return "AAudio perf=" + perf + " sharing=SHARED fmt=I16 -> " + sid;
    }

    private void startPlayback() {
        if (running) stopPlayback();
        AudioTrackPlayer.resetSong();
        int id = selectedBackend();
        android.util.Log.i(TAG, "startPlayback backend=" + id);
        int sid;
        String detail;
        try {
            switch (id) {
                case ID_AT_LEGACY:
                    sid = javaPlayer.start(AudioTrackPlayer.VARIANT_LEGACY_DEFAULT);
                    detail = javaPlayer.getInfo();
                    break;
                case ID_AT_NORMAL:
                    sid = javaPlayer.start(AudioTrackPlayer.VARIANT_BUILDER_NORMAL);
                    detail = javaPlayer.getInfo();
                    break;
                case ID_AT_LOWLAT:
                    sid = javaPlayer.start(AudioTrackPlayer.VARIANT_BUILDER_LOWLAT);
                    detail = javaPlayer.getInfo();
                    break;
                case ID_AAUDIO_LOWLAT:
                    detail = probeAaudio(12);
                    sid = aaLastSid;
                    break;
                case ID_AAUDIO_NORMAL:
                    detail = probeAaudio(10);
                    sid = aaLastSid;
                    break;
                case ID_OPENSL:
                    sid = NativeAudio.startOpenSLEx(1024, 0);
                    detail = "OpenSL 1024 perf=NONE";
                    break;
                case ID_OPENSL_LARGE:
                    sid = NativeAudio.startOpenSLEx(8192, 0);
                    detail = "OpenSL 8192 perf=NONE(大缓冲)";
                    break;
                case ID_OPENSL_LE:
                    sid = NativeAudio.startOpenSLEx(1024, 2);
                    detail = "OpenSL 1024 perf=LATENCY_EFFECTS(游戏常用)";
                    break;
                default:
                    return;
            }
        } catch (Throwable t) {
            status.setText("启动失败: " + t);
            return;
        }
        if (sid < 0) {
            status.setText("启动失败(" + sid + "): " + backendName(id) + "\n" + detail);
            return;
        }
        currentBackend = id;
        running = true;
        ((Button) findViewById(R.id.toggleBtn)).setText("停止");
        String props = "";
        try { props = "\n" + NativeAudio.aaudioProps(); } catch (Throwable ignored) {}
        status.setText("正在播放: " + backendName(id)
                + "\nsessionId=" + sid
                + "\n" + detail
                + props
                + "\n\n保持前台播放，去桌面执行抓取命令。");
    }

    private void stopPlayback() {
        if (currentBackend == ID_AAUDIO_LOWLAT || currentBackend == ID_AAUDIO_NORMAL) {
            NativeAudio.stopAaudio();
        } else if (currentBackend == ID_OPENSL || currentBackend == ID_OPENSL_LARGE
                   || currentBackend == ID_OPENSL_LE) {
            NativeAudio.stopOpenSL();
        } else {
            javaPlayer.stop();
        }
        currentBackend = -1;
        running = false;
        ((Button) findViewById(R.id.toggleBtn)).setText("开始播放");
        status.setText("已停止。");
    }

    private void copyStatus() {
        String text = status.getText().toString();
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("AudioProbe", text));
        Toast.makeText(this, "已复制到剪贴板", Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopPlayback();
    }
}
