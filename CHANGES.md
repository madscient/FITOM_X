# RtAudio 削除 変更内容

## 削除するファイル（リポジトリから git rm すること）

```
git rm core/src/AudioEngine.cpp
git rm core/include/fitom/AudioEngine.h
git rm tests/test_audio_engine.cpp
git submodule deinit extern/rtaudio
git rm extern/rtaudio
git rm .gitmodules
```

## 変更するファイル（本 ZIP に含まれる）

| ファイル | 変更内容 |
|---|---|
| `core/CMakeLists.txt` | RtAudio 検出・ビルド・リンクをすべて削除 |
| `core/include/fitom/CFITOM.h` | `AudioEngine` forward宣言を削除 |
| `gui/mfc_shim/FITOMBridge.cpp` | `FITOM_HAS_RTAUDIO` ガードを削除、`enumerateAudioDevices` を整理 |
| `docs/plugin-hwif.md` | hwif 仕様を全面改訂（fmhwif 設計・レイテンシ同期を追加） |
| `docs/STATUS.md` | AudioEngine を廃止済みとしてマーク |
| `docs/config-design.md` | RtAudio 記述を fmhwif 委譲に更新 |

## 方針

RtAudio によるオーディオ出力は `fitom_fmhwif.dll` に移管する。
fitom_core は `IHWPlugin` 経由で `write()` を呼ぶだけ。
レイテンシ同期は `HWPlugin_GetLatencySamples` / `HWPlugin_SetDelaySamples` で対応。
