# FITOM リファクタリング 完了ステータス

## 完成したファイル一覧

### プラグイン SDK (`plugin_sdk/`)
| ファイル | 状態 | 内容 |
|---|---|---|
| `IFmEnginePlugin.h` | ✅ | FmEngine DLL C API |
| `IHWPlugin.h` | ✅ | HW I/F DLL C API |
| `IMidiPlugin.h` | ✅ | MIDI バックエンド DLL C API |

### コアライブラリ (`core/`)

#### インフラ層
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/fitom_core.h` | ✅ | stdafx.h 代替・共通インクルード |
| `include/fitom/IPort.h` / `IPort.cpp` | ✅ | ハードウェア I/O 抽象 |
| `include/fitom/PluginLoader.h` | ✅ | DLL 動的ロード RAII |
| `include/fitom/FmEnginePort.h` / `.cpp` | ✅ | FmEngine → IPort アダプター |
| `include/fitom/HWPort.h` / `.cpp` | ✅ | HWPlugin → IPort アダプター |
| `include/fitom/MidiManager.h` / `.cpp` | ✅ | MIDI バックエンド DLL 管理 |
| `include/fitom/Log.h` / `Log.cpp` | ✅ | Boost.Log ラッパー |
| `include/fitom/VolumeUtils.h` / `.cpp` | ✅ | CalcLinearLevel / Linear2dB / ROM テーブル |
| `include/fitom/FnumUtils.h` | ✅ | F-number テーブルキャッシュ |
| `include/fitom/AudioEngine.h` / `.cpp` | ✅ | RtAudio ラッパー |

#### ボイスデータ
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/VoiceData.h` | ✅ | HwPatch / SwPatch / FmHwOp / FmSwOp |
| `include/fitom/VoiceProcessor.h` / `.cpp` | ✅ | ベロシティ感度・ソフト LFO 処理 |
| `include/fitom/PatchData.h` | ✅ | Patch / ToneLayer / HwBank / SwBank |
| `include/fitom/PatchManager.h` / `.cpp` | ✅ | バンク管理・JSON I/O |

#### 音源デバイス
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/ISoundDevice.h` / `SoundDevImpl.cpp` | ✅ | CSoundDevice 共通実装 |
| `OPN_new.cpp` | ✅ | OPN / OPNA / OPN2 (factory 付き) |
| `OPM_new.cpp` | ✅ | OPM / OPP / OPZ |
| `OPN2_new.cpp` | ✅ | OPN2 / COPNA 6ch |
| `OPL_new.cpp` | ✅ | OPL / OPL2 / OPL3 |
| `OPLL_new.cpp` | ✅ | OPLL / OPLL2 / VRC7 |
| `PSG_new.cpp` | ✅ | SSG / DCSG / SCC |
| `MultiDev_new.cpp` | ✅ | CSpanDevice / CUnison |
| `ADPCM_new.cpp` | ✅ | CYmDelta / CAdPcmZ280 |
| `include/fitom/DeviceFactory.h` / `.cpp` | ✅ | IPort → ISoundDevice ファクトリ |

#### MIDI 処理
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/MidiCh.h` / `MidiCh.cpp` | ✅ | CInstCh / CRhythmCh (マルチレイヤー) |

#### 設定・コア
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/Config.h` / `Config.cpp` | ✅ | FITOMConfig (ISoundDevice 対応版) |
| `include/fitom/CFITOM.h` / `CFITOM.cpp` | ✅ | コアシングルトン / MidiProcessor |

### バックエンド DLL

| ファイル | 状態 | 内容 |
|---|---|---|
| `backends/midi_wms/` | ✅ | Windows MIDI Services (C++/WinRT) |
| `backends/midi_alsa/` | ✅ | ALSA MIDI |
| `backends/hw_if/CMakeLists.txt` | ✅ | FitomIFTest submodule 統合 |
| `backends/fm_engine/CMakeLists.txt` | ✅ | FmEngineApi.h INTERFACE |

### GUI

| ファイル | 状態 | 内容 |
|---|---|---|
| `gui/mfc_shim/FITOMBridge.h` | ✅ | MFC ↔ コアブリッジ API |
| `gui/mfc_shim/FITOMBridge.cpp` | ✅ | ブリッジ実装 |

### 設定スキーマ・ドキュメント

| ファイル | 状態 |
|---|---|
| `fitom.conf.schema.json` | ✅ |
| `profile.schema.json` | ✅ |
| `hwbank.schema.json` | ✅ |
| `swbank.schema.json` | ✅ |
| `patchbank.schema.json` | ✅ |
| `docs/DESIGN.md` | ✅ |
| `docs/chip-driver-migration.md` | ✅ |
| `docs/patch-structure-design.md` | ✅ |
| `docs/voice-data-design.md` | ✅ |

---

## 残作業 (実装で完結できなかったもの)

### TODO コメントになっている箇所

1. **`DeviceFactory.cpp`**: `createDevices()` 内の `DeviceFactory::create()` 呼び出し
   - `Config.cpp` で DeviceFactory.h を include して `entry.device = DeviceFactory::create(...)` を呼ぶ
   - 現在は循環依存を避けてコメントアウト

2. **`FITOMBridge.cpp`**: 音色エディタ連携の TODO
   - `getHwPatchJson()` / `setHwPatchJson()` の完全実装
   - HwBankRegistry からの JSON 変換

3. **`PatchManager.cpp`**: `saveSwBankJson()` の実装
   - SwBankRegistry に `find()` 追加後に実装

4. **`CFITOM.cpp`**: `getDevice()` の完全実装
   - `FITOMConfig::getDevice()` が ISoundDevice* を返すよう Config.cpp の `createDevices()` を完成させる

5. **`CRhythmCh`**: `getDrum()` / `getDrumPatch()` の実装
   - `CFITOM::getDrum()` の DrumBank → PatchManager 統合

### FitomIFTest 側の追加作業

`plugin_sdk/include/fitom/IHWPlugin.h` に従った共有ライブラリ (`fitom_hw.dll`) のビルドターゲットを FitomIFTest に追加する。
`docs/DESIGN.md` の「FitomIFTest 側の追加作業」セクションに実装パターン付きで記載済み。

---

## ビルド手順 (フェーズ1完了後)

```bash
# vcpkg で依存関係をインストール
vcpkg install nlohmann-json boost-log boost-format boost-thread boost-system boost-asio boost-interprocess

# CMake 設定
cmake --preset linux-ninja .   # Linux
cmake --preset windows-vs2022-x64 .  # Windows

# ビルド
cmake --build build/linux-ninja

# テスト
ctest --preset linux-test
```
