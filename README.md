# AudioProbe

[DolbyRouter](../DolbyRouter) 的配套**测试探针**。用来验证「音频走哪条输出轨、挂不挂杜比」，
本身不是给人听的播放器，是一个可复现的音频路由实验台。

在**小米平板 6（Android 15）**上盲听 + `dumpsys media.audio_flinger` 对照过，8 种后端结果 100% 一致：
**决定有没有杜比的是音频轨落到 FAST 输出还是普通输出，跟用哪个 API 无关。**

---

## 1. 它做什么

同一段音频（内置合成测试音，或你放入的 PCM），用 8 种不同后端播放，覆盖三条播放路径：

| # | 后端 | 路径 | 默认落点 |
| --- | --- | --- | --- |
| 1 | Java AudioTrack 旧构造 (`STREAM_MUSIC`) | Java | 普通轨 → ✅ 杜比 |
| 2 | Java AudioTrack Builder（普通） | Java | 普通轨 → ✅ 杜比 |
| 3 | Java AudioTrack Builder（低延迟） | Java | FAST 轨 → ❌ |
| 4 | AAudio 低延迟 | AAudio | FAST 轨 → ❌ |
| 5 | AAudio 普通 | AAudio | FAST 轨 → ❌ |
| 6 | OpenSL ES（1024） | OpenSL | 普通轨 → ✅ 杜比 |
| 7 | OpenSL ES（8192 大缓冲） | OpenSL | 普通轨 → ✅ 杜比 |
| 8 | OpenSL ES（`LATENCY_EFFECTS`，游戏常用） | OpenSL | FAST 轨 → ❌ |

装上 [DolbyRouter](../DolbyRouter) 并把本应用加入名单后，上表**所有**后端都应回落到普通轨（`AudioOut_15`，
effect chain 里能看到 `DAP_offload`），声音带上杜比。

## 2. 怎么用

1. 安装 [DolbyRouter](../DolbyRouter)：刷入 Zygisk 模块并重启，再用配置 APK 勾选 **AudioProbe**。
2. 杀掉 AudioProbe 重开（让模块在进程启动时注入）。
3. 选一个后端，点「开始播放」。
4. 切到桌面执行抓取命令对照：

```bash
# 看输出线程 / 有没有 FastMixer / effect chain 里有没有 DAP_offload
adb shell dumpsys media.audio_flinger
```

判定：落 `AudioOut_15`（`No FastMixer`）且 effect chain 含 `DAP_offload` = 有杜比；
落 `AudioOut_D`（`PRIMARY|FAST`）且有 FastMixer = 绕过杜比。

也可以脚本化（不用手点）：

```bash
# backend 1..8，play=true 自动播放
adb shell am start -n com.audioprobe/.MainActivity --ei backend 6 --ez play true
```

> 启动时会把媒体音量设成 30%，避免突然很大声。

## 3. 放入自己的音频（可选）

默认播放内置的**合成测试音**（60 Hz 低音 + 440 Hz 中音 + 6 kHz 高音，很容易听出杜比差异）。

想用真实音乐，把一段 PCM 放到 `app/src/main/assets/moonlight.pcm` 再重新编译。格式必须是
**48 kHz / 立体声 / 16-bit little-endian**（代码里所有后端都硬编码这个格式，否则会变调）：

```bash
ffmpeg -i your-song.flac -f s16le -ar 48000 -ac 2 app/src/main/assets/moonlight.pcm
```

`*.pcm` 已在 `.gitignore` 里，不会提交。**请不要把有版权的音乐提交到仓库。**

## 4. 编译

需要 Android SDK + **NDK r27c**（`27.2.12479018`），JDK 17+。

```bash
# 在项目根目录创建 local.properties 指向你的 SDK
echo "sdk.dir=$ANDROID_HOME" > local.properties

JAVA_HOME=/path/to/jdk-21 ./gradlew :app:assembleDebug
# 产物：app/build/outputs/apk/debug/app-debug.apk
```

native 部分（AAudio / OpenSL ES）在 `app/src/main/cpp/`，CMake 构建，仅 `arm64-v8a`。

## 5. 目录

```
app/src/main/java/com/audioprobe/
  MainActivity.java      # UI + 后端选择 + 脚本化入口 (backend/play extra)
  AudioTrackPlayer.java  # Java AudioTrack 三种变体
  NativeAudio.java       # AAudio / OpenSL ES 的 JNI 声明
app/src/main/cpp/
  native-lib.cpp         # AAudio + OpenSL ES 实现，合成测试音/PCM 填充
app/src/main/assets/
  moonlight.pcm          # 可选，自备（gitignore）
```

## 6. 说明

- 仅 `arm64-v8a`。
- 依赖 `dumpsys media.audio_flinger` 的输出来判断落点，请对照 [DolbyRouter 的文档](../DolbyRouter/tree/master/docs)。
- 这个仓库只做验证，真正让应用挂杜比的是 [DolbyRouter](../DolbyRouter)。
