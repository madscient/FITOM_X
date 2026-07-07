# FITOM リファクタリング 完了ステータス

## 完成したファイル一覧

### プラグイン SDK (`plugin_sdk/`)
| ファイル | 状態 | 内容 |
|---|---|---|
| `IHWPlugin.h` | ✅ | HW I/F DLL C API |
| `IMidiPlugin.h` | ✅ | MIDI バックエンド DLL C API |

### コアライブラリ (`core/`)

#### インフラ層
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/fitom_core.h` | ✅ | stdafx.h 代替・共通インクルード |
| `include/fitom/IPort.h` / `IPort.cpp` | ✅ | ハードウェア I/O 抽象 |
| `include/fitom/PluginLoader.h` | ✅ | DLL 動的ロード RAII |
| `include/fitom/HWPort.h` / `.cpp` | ✅ | HWPlugin → IPort アダプター (実機/エミュレータ共通、HWPluginRegistryで複数管理) |
| `include/fitom/MidiManager.h` / `.cpp` | ✅ | MIDI バックエンド DLL 管理 |
| `include/fitom/Log.h` / `Log.cpp` | ✅ | Boost.Log ラッパー |
| `include/fitom/VolumeUtils.h` / `.cpp` | ✅ | CalcLinearLevel / Linear2dB / ROM テーブル |
| `include/fitom/FnumUtils.h` | ✅ | F-number テーブルキャッシュ |
| `include/fitom/AudioEngine.h` / `.cpp` | 🗑️ 廃止 | fitom_fmhwif DLL に移管 |

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
| `OPN_new.cpp` | ✅ | COPN (YM2203, FXモード対応) |
| `OPM_new.cpp` | ✅ | OPM / OPP / OPZ (REV/EGS対応) |
| `OPN2_new.cpp` | ✅ | COPNA/COPN2 (CSpanDevice、6ch) / COPNARhythm |
| `OPL_new.cpp` | ✅ | OPL/OPL2/COPL3(4OPモード)/COPL3_2(2OP、CSpanDevice) |
| `OPLL_new.cpp` | ✅ | OPLL/OPLL2/OPLLP/OPLLX/VRC7/COPLLRhythm |
| `PSG_new.cpp` | ✅ | SSG/DCSG/SCC (CPSGBaseはSW-EG/SW-LFO共通化のみ) |
| `MultiDev_new.cpp` / `include/fitom/MultiDevice.h` | ✅ | CMultiDevice/CSpanDevice/CUnison (ヘッダー化済み) |
| `ADPCM_new.cpp` | ✅ | CYmDelta(Y8950/OPNA/OPNB)/CAdPcm2610A/CAdPcmZ280 |
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
| `backends/midi_winmm/` | ✅ | クラシック WinMM API (ランタイム不要) |
| `backends/midi_alsa/` | ✅ | ALSA MIDI |
| `backends/hw_if/CMakeLists.txt` | ✅ | FitomIFTest submodule 統合 |

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
| `docs/chip-driver-architecture.md` | ✅ |
| `docs/patch-structure-design.md` | ✅ |
| `docs/voice-data-design.md` | ✅ |
| `docs/config-design.md` | ✅ |
| `docs/plugin-hwif.md` | ✅ |
| `docs/plugin-midi.md` | ✅ |
| `docs/midi-implementation-status.md` | ✅ |

---

## 音源機能の実装状況（追加セッション分）

以下の機能は初期リファクタリング完了後、追加セッションで実装・修正した。

| 機能 | 状態 | 関連ドキュメント |
|---|---|---|
| ベロシティ感度 (VTL + VAR〜VRR、全FMチップ + PSG) | ✅ | `voice-data-design.md` |
| ソフトウェアLFO 全面再設計 (LfoControl) | ✅ | `voice-data-design.md` |
| CC#1 Modulation → LFR=0音色専用のCC駆動LFO | ✅ | `midi-implementation-status.md` |
| マスターピッチ可変 (430-450Hz) + OPM算出バグ修正 | ✅ | — |
| ダイナミックボイスアサイン (findBestCh 1パス化) | ✅ | — |
| Sustain (CC#64) チップ依存実装 + MIDI配線バグ修正 | ✅ | `midi-implementation-status.md` |
| Sostenuto (CC#66) | ✅ | `midi-implementation-status.md` |
| Portamento/Legato モノフォニック専用化 + バグ修正 | ✅ | `midi-implementation-status.md` |
| Portamento Rate テーブル刷新 (GM2グラフ準拠) + fine_ セント単位遷移対応 | ✅ | `midi-implementation-status.md` |
| CC#120 forceDamp (全チップ、ALGキャリア判定込み) | ✅ | `midi-implementation-status.md` |
| VoicePatchType システム (音色パッチ互換性分類) | ✅ | `patch-structure-design.md` |
| バンクセレクトLSB直接指定モード | ✅ | `patch-structure-design.md` |
| PSGソフトウェアエンベロープ (SoftEnvelope, FM実機準拠ADSR) | ✅ | `voice-data-design.md` |
| AY-3-8910 HW EGレジスタ仕様修正 (ext.HWEP) | ✅ | `voice-data-design.md` |
| OPLLX / VRC7 (6ch専用) チップドライバ | ✅ | — |
| リズムモード汎用フィールド (`rhythm_mode`) | ✅ | `config-design.md` |
| RtAudio削除 (fitom_fmhwif DLLへ移管) | ✅ | `plugin-hwif.md` |
| HWデバイス レイテンシ同期 (GetLatencySamples/SetDelaySamples) | ✅ | `plugin-hwif.md` |
| Sub-device自動生成 (OPNA→FM+SSG+ADPCM-B+Rhythm 等) | ✅ | `chip-driver-architecture.md` |
| 同種デバイス自動束ね (CSpanDevice、VoicePatchType基準) | ✅ | `chip-driver-architecture.md` |
| OPL3 4OPモード (COPL3) + 疑似デチューン(DT2転用) | ✅ | `chip-driver-architecture.md` |
| OPN FXモード (3rd channel special mode、疑似デチューン/非整数倍率/固定周波数) | ✅ | `chip-driver-architecture.md`, `voice-data-design.md` |
| COPNARhythm / COPLLRhythm (内蔵リズム音源、独立レジスタ体系) | ✅ | `chip-driver-architecture.md` |
| CPSGBase 責務整理 (SW-EG/SW-LFO共通化のみに純化、SSG固有コードをCSSGへ移動) | ✅ | `chip-driver-architecture.md` |
| リリース中再トリガー対策 (wasReleasing、OPM/OPN/OPL/OPL3) | ✅ | `chip-driver-architecture.md` |
| ADPCM RegMap 全面修正 (Y8950/OPNA/OPNB個別マップ、memory/panmaskフィールド追加) | ✅ | `chip-driver-architecture.md` |
| OPLL Fnumberビットシフト修正・EGT/RR技法適用 | ✅ | `chip-driver-architecture.md` |

---

## 既知の未対応・将来課題

- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal（FM音源に対応するパラメータがないため意図的に非対応）
- RPN 0x0002 Coarse Tuning、RPN 0x7F7F Null
- CC#2/CC#4 の変数分離
- VoicePatchType 未実装チップ (MA3系列, SAA1099, AWM) のドライバ実装
- OPL/OPL2/OPL3自体のリズムモード対応（現状OPLL系のみ対応。COPL_new.cppにリズム関連コードなし）
- VoicePatchType 完全一致以外へのフォールバック（旧FITOMの互換リスト相当、将来実装予定）
- GUI (Qt6) 実装
- OPZ の2系統LFOリソース対応（旧FITOMも未完成のため現状維持）
- CAdPcmZ280 (YMZ280B/PCMD8) の旧FITOM実装との詳細突き合わせ未完了

### FitomIFTest 側の追加作業

`plugin_sdk/include/fitom/IHWPlugin.h` に従った共有ライブラリ (`fitom_hw.dll`) のビルドターゲットを FitomIFTest に追加する。
`docs/DESIGN.md` の「FitomIFTest 側の追加作業」セクションに実装パターン付きで記載済み。

---

## ビルド手順

**依存関係の取得（既定: vcpkg不要）**

```bash
# nlohmann-json は git submodule (初回のみ)
git submodule update --init --recursive

# boost (thread/log/log_setup/format/interprocess) はシステムパッケージマネージャで取得
# Ubuntu/Debian の例:
apt install libboost-dev libboost-log-dev libboost-thread-dev
# Windows は公式バイナリ配布や MSYS2 等で入手するか、
# 後述の vcpkg プリセットを使う
```

**CMake 設定・ビルド**

```bash
cmake --preset linux-ninja .          # Linux (vcpkg不要)
cmake --preset windows-vs2022-x64 .   # Windows (vcpkg不要、boostは別途用意)

cmake --build build/linux-ninja
ctest --preset linux-test
```

**vcpkg を使いたい場合（任意）**

boost をシステムに用意しづらい場合など、vcpkg 経由でも取得できる。

```bash
cmake --preset windows-vs2022-x64-vcpkg .
```

このプリセットは `FITOM_USE_VCPKG_JSON=ON` を指定し、`vcpkg.json`（`boost-thread`/`boost-format`/`boost-log`/`boost-interprocess`のみ、`nlohmann-json`/`boost-asio`/`libftdi1`は含まない）経由でboostとnlohmann-jsonの両方を取得する。
