# FITOM_X 実装状況

このファイルには現在の状態だけを書く。変更・修正・調査の経緯は`docs/worklog/`
(月ごとに1ファイル)に、設計の詳細は各設計文書に書く。

確度の印: **確認済み**(何で確かめたかを添える) / **未検証** / **推測**(根拠を添える)。
印の無い記述は、過去のセッションが実装・テストした結果の記録である。

---

## 1. 構成要素

### プラグイン SDK (`plugin_sdk/`)
| ファイル | 状態 | 内容 |
|---|---|---|
| `IHWPlugin.h` | ✅ | HW I/F DLL C API。部位ごとのゲイン(任意の組)を含む |
| `IMidiPlugin.h` | ✅ | MIDI バックエンド DLL C API |

### コアライブラリ (`core/`)

#### インフラ層
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/fitom_core.h` | ✅ | stdafx.h 代替・共通インクルード |
| `include/fitom/IPort.h` / `IPort.cpp` | ✅ | ハードウェア I/O 抽象 |
| `include/fitom/PluginLoader.h` | ✅ | DLL 動的ロード RAII |
| `include/fitom/HWPort.h` / `.cpp` | ✅ | HWPlugin → IPort アダプター (実機/エミュレータ共通、HWPluginRegistryで複数管理)。シャドウレジスタ(レジスタダンプモニター用)と部位の一覧を持つ |
| `include/fitom/MidiManager.h` / `.cpp` | ✅ | MIDI バックエンド DLL 管理 |
| `include/fitom/Log.h` / `Log.cpp` | ✅ | Boost.Log ラッパー |
| `include/fitom/VolumeUtils.h` / `.cpp` | ✅ | CalcLinearLevel / Linear2dB / ROM テーブル |
| `include/fitom/FnumUtils.h` | ✅ | F-number テーブルキャッシュ |
| `include/fitom/AudioEngine.h` / `.cpp` | 🗑️ 廃止 | HW I/F プラグイン側へ移管 |

#### ボイスデータ
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/VoiceData.h` | ✅ | HwPatch / SwPatch / FmHwOp / FmSwOp |
| `include/fitom/VoiceProcessor.h` / `.cpp` | ✅ | ベロシティ感度・ソフト LFO 処理 |
| `include/fitom/PatchData.h` | ✅ | Patch / ToneLayer / HwBank / SwBank |
| `include/fitom/PatchManager.h` / `.cpp` | ✅ | バンク管理・JSON I/O |

#### 音源デバイス
設計は`chip-driver-architecture.md`、Y8960の拡張ブロックは同4.5.3節。

| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/ISoundDevice.h` / `SoundDevImpl.cpp` | ✅ | CSoundDevice 共通実装 |
| `OPN_new.cpp` | ✅ | COPN (YM2203, FXモード対応) |
| `OPM_new.cpp` | ✅ | OPM / OPP / OPZ (REV/EGS対応)。Y8960のOPMも`chip:"OPM"`としてそのまま扱う |
| `OPN2_new.cpp` | ✅ | COPNA/COPN2 (CSpanDevice、6ch) / COPNB (YM2610無印、ch0/ch3無効化した実効4ch) / COPNARhythm |
| `OPL_new.cpp` | ✅ | OPL/OPL2/COPL3(4OPモード)/COPL3_2(2OP、CSpanDevice)/COPLRhythm(内蔵リズム5パート)/COPL2EX(Y8960拡張OPL2部、chごとの定位つき)+COPL2EXRhythm |
| `OPLL_new.cpp` | ✅ | OPLL/OPLL2/OPLLP/OPLLX/VRC7/COPLLRhythm/COPLLEX(Y8960拡張OPLL部。chごとのROMプリセットバンク選択と定位)+COPLLEXRhythm。ROMプリセットの解決規則は`patch-structure-design.md` |
| `PSG_new.cpp` | ✅ | SSG/EPSG/DCSG/SCC/SAA1099と、SSG互換部を持つチップ: CSSGS(YMZ705=SSGS / YMZ732=SSGS2)、CSSGS3(YMZ771=SSGS3。AMM部は非対応)、CSSGEX(Y8960のSSG部)。CSCCはY8960のSCC部(SCCEX)も担当する |
| `DSG_new.cpp` | 🚧 | CDSG (YM2163楽音4ch) / CDSGRhythm (内蔵リズム5パート)。ROM固定の20音色をPatchManagerが暗黙のバンクとして生成する。`VOICE_PATCH_DSG`は`isPsgFamilyVoicePatchType()`に含めない(HwBankを持たないため、含めるとパッチピッカーのDSGカテゴリにSSGのバンクが並ぶ)。音とGUIは未確認(3節) |
| `OPL4.cpp` | ✅ | COPL4AWM (YMF278のAWM部) |
| `MultiDev_new.cpp` / `include/fitom/MultiDevice.h` | ✅ | CMultiDevice/CSpanDevice/CUnison/CLinearPanDevice |
| `ADPCM_new.cpp` | ✅ | CYmDelta(Y8950/OPNA/OPNB。OPL2EX内蔵ADPCM-BはCY8960Delta)/CAdPcm2610A/CAdPcmZ280/CSSGSAdPcm |
| └ `CSSGSAdPcm` | 🚧 | YMZ705/YMZ732内蔵ADPCM 8ch。書くのは6bitのボイス番号だけで、再生ピッチは変えられない(サンプリング周波数4択)。コーデック・音量4bitの解釈・サンプリング周波数のビット対応はデータシートに記載が無く、実機での確認が要る(3節) |
| `include/fitom/DeviceFactory.h` / `.cpp` | ✅ | IPort → ISoundDevice ファクトリ |

#### MIDI 処理
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/MidiCh.h` / `MidiCh.cpp` | ✅ | CInstCh / CRhythmCh (マルチレイヤー) |

#### 設定・コア
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/Config.h` / `Config.cpp` | ✅ | FITOMConfig (ISoundDevice 対応版)。プロファイルの読み込みと、GUIで変えた値の書き戻し |
| `include/fitom/CFITOM.h` / `CFITOM.cpp` | ✅ | コアシングルトン / MidiProcessor。物理チップの列挙(レジスタダンプ・レベルメーター・部位ゲイン用) |
| `Sf2BankRegistry.cpp` | ✅ | SF2直行パスのバンク解決とプリセット名(`phdr`)の読み出し |

### バックエンド DLL

| ファイル | 状態 | 内容 |
|---|---|---|
| `backends/midi_rtmidi/` | ✅ | RtMidi (Windows/Linux/macOS共通) |
| `backends/midi_pipe/` | ✅ | 内部用MIDIパイプ (`fitom_midi_pipe`、名前付きパイプ/UNIXソケット、パッチエディタ連携用、既定OFF)。`-DFITOM_BUILD_BACKEND_MIDI_PIPE=ON`でビルドするとプロファイルの設定と無関係に常時有効。最大16本まで同時接続でき、接続直後にチャンネル割り当てをプライベートSysEx(sub-cmd 0x03)で通知する。詳細は`plugin-midi-pipe.md` |
| `backends/hw_if/CMakeLists.txt` | ✅ | `HWIF_SOURCE_DIR`で外部のHW I/Fソースを取り込む口だけを持つ。実際の`fitom_hw.dll`はFitomHwIF(別リポジトリ)がビルドする(**確認済み**: FitomHwIFのCMakeLists.txtとREADMEを読んで確認) |

### GUI

| ファイル | 状態 | 内容 |
|---|---|---|
| `gui/bridge/FITOMBridge.h` / `.cpp` | ✅ | UIフレームワーク非依存のコアブリッジAPI |
| `apps/fitom_gui/` | 🚧 | Dear ImGui + GLFW + OpenGL3。下表のとおり |

`apps/fitom_gui`で実装済みの画面・機能:

- **MIDIモニターバンド**(ルート画面): CH毎のBank/Program/Volume/Note/Device/Fnumber、MPU切替、128ノートのキーボードビューと発光エフェクト。SF2直行パスのチャンネルは、解決したパッチ名と送出したMIDIバイト列を表示する
- **CH設定ダイアログ**(CH番号をクリック): Volume/Panpot/Expression(即時プレビュー、キャンセルで復元)、リズム⇔インストゥルメント、Poly⇔Mono、パッチ選択、SF2直行パスの窓のON/OFF。値の適用はGUIからsetterを叩かず、`FITOMBridge`のMIDI送信経由でコアのMIDI処理経路を通す
- **パッチピッカー**: CC#0→CC#32→Prog.chgの階層ブラウジング(直接デバイス選択モード、リズムチャンネルのドラムキット一覧を含む)。行を押している間だけC4を鳴らす試聴。確定は「選択」ボタンだけで行う
- **外部パッチエディタの起動**(Bank/Programをダブルクリック): `<profile.json> <kind> <bank-file> <prog>`(`kind`="device"/"layered"/"drum")でキオスクモード起動する
- **MIDIポート設定ダイアログ**(ポート名をクリック): MPU 4面分のMIDI入力ポートの割り当て。OKで即時反映し、プロファイルへ書き戻す
- **システム設定ダイアログ**(左上の歯車): マスターボリューム、マスターピッチ(430〜450Hz)、HWプラグインの部位ごとのL/Rゲイン。OKでプロファイルへ書き戻す
- **レジスタダンプモニター**(`RegisterDumpWindow`)と**チャンネルレベルメーター**(`LevelMeterPanel`、ステレオのL/R分割表示つき)
- **プロファイル**: 引数省略時の起動時選択画面(`fitom.conf.json`の`profiles.root`配下)、実行中の切替、読み込み失敗時のフォールバック(第2引数)、ウィンドウタイトルへのプロファイル名表示。実行中の切替は、自分自身を新しいプロセスとして起動し直す方式で、理由は`apps/fitom_gui/main.cpp`のコメントにある
- **未着手**: デバイス一覧・パッチ一覧(フラット表示)・音色エディタ等、他画面への導線(該当する描画関数は`[[maybe_unused]]`で温存)

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
| `docs/plugin-midi-pipe.md` | ✅ |
| `docs/midi-implementation-status.md` | ✅ |
| `docs/terminology.md` | ✅ |
| `docs/voice-parameter-reference.md` | ✅ |
| `docs/sf2-fluidsynth-integration.md` | ✅ |
| `docs/vgm-recording.md` | ✅ (設計検討のみ) |
| `docs/opm-opz-hw-lfo.md` | ✅ (設計検討のみ) |
| `docs/manuals/midi-message-reference.md` | ✅ |
| `docs/manuals/midi-implementation-chart.md` | ✅ |
| `docs/manuals/hwpatch-reference.md` | ✅ |
| `docs/manuals/swpatch-reference.md` | ✅ |
| `docs/manuals/layered-patch-reference.md` | ✅ |

---

## 2. 機能

| 機能 | 状態 | 関連ドキュメント |
|---|---|---|
| ベロシティ感度 (VTL + VAR〜VRR、全FMチップ + PSG) | ✅ | `voice-data-design.md` |
| ソフトウェアLFO (LfoControl) | ✅ | `voice-data-design.md` |
| CC#1 Modulation → LFR=0音色専用のCC駆動LFO | ✅ | `midi-implementation-status.md` |
| マスターピッチ可変 (430-450Hz) | ✅ | — |
| ダイナミックボイスアサイン (findBestCh 1パス) | ✅ | — |
| Sustain (CC#64、チップ依存実装) | ✅ | `midi-implementation-status.md` |
| Sostenuto (CC#66) | ✅ | `midi-implementation-status.md` |
| Portamento/Legato (モノフォニック専用) | ✅ | `midi-implementation-status.md` |
| Portamento Rate テーブル (GM2グラフ準拠、セント単位遷移) | ✅ | `midi-implementation-status.md` |
| CC#120 forceDamp (全チップ、ALGキャリア判定込み) | ✅ | `midi-implementation-status.md` |
| RPN#1 チャンネルファインチューニング | ✅ | `midi-implementation-status.md` |
| VoicePatchType システム (音色パッチ互換性分類)。HwBankの登録キーはチップ種別(VoicePatchType)単位 | ✅ | `patch-structure-design.md` |
| バンクセレクトLSB直接指定モード | ✅ | `patch-structure-design.md` |
| 無音バンク (CC#0=127、`VOICE_PATCH_SILENCE`) | ✅ | `patch-structure-design.md` |
| PSGソフトウェアエンベロープ (SoftEnvelope, FM実機準拠ADSR) | ✅ | `voice-data-design.md` |
| AY-3-8910 HW EG (ext.HWEP)、AY8930拡張モードのバンク切り替え | ✅ | `voice-data-design.md`, `chip-driver-architecture.md` |
| リズムモード汎用フィールド (`rhythm_mode`)、ビルトインリズムのプロファイル明示化(`auto_devices`経由を含む) | ✅ | `config-design.md` |
| HWデバイス レイテンシ同期 (GetLatencySamples/SetDelaySamples) | ✅ | `plugin-hwif.md` |
| HWPlugin_Shutdown (任意API、二重実行防止) | ✅ | `plugin-hwif.md` |
| HWプラグインへ渡すレジスタアドレスは常に実チップのもの(契約) | ✅ | `plugin-hwif.md` |
| HWプラグインの部位ごとのゲイン (任意の組、プロファイルの`part_gains`に保存) | 🚧 | `plugin-hwif.md`, `config-design.md` |
| Sub-device自動生成 (OPNA→FM+SSG+ADPCM-B+Rhythm 等) | ✅ | `chip-driver-architecture.md` |
| 同種デバイス自動束ね (CSpanDevice、VoicePatchType基準、チップ試行順はローテーション) | ✅ | `chip-driver-architecture.md` |
| リニアステレオ化 (CLinearPanDevice、`stereo_pair`)。左右の分離は2方式: `true`=プラグイン側の出力ルーティング(`pan:1/2`) / `"L"`・`"R"`=チップ自身のL/R出力ビット | ✅ | `chip-driver-architecture.md`, `config-design.md` |
| OPL3 4OPモード (COPL3) + 疑似デチューン(DT2転用) | ✅ | `chip-driver-architecture.md` |
| OPN FXモード (3rd channel special mode、疑似デチューン/非整数倍率/固定周波数) | ✅ | `chip-driver-architecture.md`, `voice-data-design.md` |
| 内蔵リズム音源 (COPNARhythm / COPLRhythm / COPLLRhythm、独立レジスタ体系) | ✅ | `chip-driver-architecture.md` |
| リリース中再トリガー対策 (wasReleasing、OPM/OPN/OPL/OPL3) | ✅ | `chip-driver-architecture.md` |
| ADPCM RegMap (Y8950/OPNA/OPNB個別マップ) | ✅ | `chip-driver-architecture.md` |
| OPLL EGT/RR静的変換 (`SR>0`ならEGT=0+RRレジスタ=SR。OPL系とは別方式。実機確認済みの確定仕様) | ✅ | `chip-driver-architecture.md`, `voice-parameter-reference.md`, `manuals/hwpatch-reference.md` |
| ドラムのチョークグループ (`DrumPatch::chokeGroups`) | ✅ | `patch-structure-design.md` |
| PCMバンクの複数併用、パッチピッカー用named patchの自動合成 | ✅ | `patch-structure-design.md`, `manuals/hwpatch-reference.md` |
| pcmbank/samplezonebankへのSwPatch紐づけ (AWMは非対応、4節) | ✅ | `patch-structure-design.md`, `voice-parameter-reference.md` |
| プロファイルの`banks`の外部ファイル分離、`bank_overrides` | ✅ | `config-design.md` |
| SF2直行パス (FITOM_X本体側: 設定・MIDIディスパッチ・プライベートSysEx・`phdr`パース・GUI表示) | ✅ | `sf2-fluidsynth-integration.md`, `manuals/midi-message-reference.md` |
| 内部用MIDIパイプ (多接続、チャンネル自動割り当て) | ✅ | `plugin-midi-pipe.md` |
| MIDIバックエンドのRtMidi単一実装 | ✅ | `plugin-midi.md` |
| Y8960の拡張ブロック (OPLLEX/OPL2EX/SSGEX/SCCEX、chごとの8bit定位。仕様書10月20日版に追従) | 🚧 | `chip-driver-architecture.md` 4.5.3節 |
| テストケース名はASCIIに限定 (`tests/test_test_names.cpp`が検査する) | ✅ | — |

---

## 3. 未確認事項

作ったが、実機・エミュレータの音、またはGUIの目視で確かめていないもの。
ビルドとテストが通ることまでは確かめてある。

**音(実機・エミュレータ)**
- Y8960の拡張ブロック(OPLLEX/OPL2EX[内蔵ADPCM-B・リズムを含む]/SSGEX/SCCEX)とOPM。OPLLEXのリズムへのフォールバック(`resolveBuiltinRhythm()`)はコードを読んだだけ。OPL2EX内蔵ADPCM-Bの分周比をY8950と同一としているのは**推測**(仕様書に記載が無い。レジスタ配置がY8950と同一であることから類推)
- SSGS/SSGS2/SSGS3のSSG部
- `CSSGSAdPcm`: エミュレータ(EPSGemuEngine)はコーデックをYMZ280Bと同じ4bit ADPCMと仮定している。FitomEmuIFはSSGS/SSGS2の`PCM`へカタログ種別`SSGS_ADPCM`のイメージを渡す(**確認済み**: FitomEmuIF `2d0e443`のソースを読んで確認)。鳴ったとしても、コーデックが仮定のため実機と同じ音かは判断できない
- DSG(YM2163)
- SCC・DCSGの音量レジスタ変換則、AY8930拡張モードのバンク切り替え
- HWプラグインの部位ごとのゲイン: 実物のFitomEmuIFと名前指定の部位ゲインに対応したエンジン(YMEngine/DSGemuEngine/DSAemuEngine)を組み合わせて鳴らしたことは無い。`CFITOM`/`FITOMBridge`層(物理チップの添字からポートへの対応とロック)はテストが通っておらず、`HWPlugin_Write`と並行しないことはロックを取る箇所を読んで判断しただけ

**GUIの目視**
- システム設定ダイアログの部位ゲインのスライダー
- DSGのパッチピッカー・内蔵リズムの一覧
- SF2直行パスのMIDIモニター表示と、CH設定ダイアログの窓の切替
- レジスタダンプモニターのスクロールバーの修正、チャンネルレベルメーターの束ねられたチップの表示の修正(いずれも修正後の再確認)

**外部との連携**
- 外部パッチエディタの4引数キオスク起動が、layered/device/drumそれぞれで正しい編集画面を開くこと。`kind="device"`は、プロファイルの`devices[]`に対応するHWプラグインDLLがロードできないと解決に失敗する(`Config::buildDevice()`がDeviceEntryを作らないため)が、その場合の挙動は未検証
- 内部用MIDIパイプと実際のパッチエディタとの接続(PowerShellの簡易クライアントでの疎通確認までは実施済み)
- SF2直行パスのFitomSf2IFとFITOM_Xを組み合わせての音出し(FitomSf2IFは実装済み。同リポジトリの`docs/STATUS.md`も実機検証は未実施としている)

---

## 4. 既知の未対応・将来課題

**MIDI**
- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal(FM音源に対応するパラメータが無いため意図的に非対応)
- RPN 0x0002 Coarse Tuning、RPN 0x7F7F Null
- CC#2/CC#4 の変数分離

**チップドライバ**
- SD1/MA3/MA5/MA7のドライバ(パッチ種別とVoiceGroupの対応表だけがある。**確認済み**: `DeviceFactory.cpp`に生成処理が無いことと`Config.cpp`の対応表を読んで確認)
- VoicePatchType 完全一致以外へのフォールバック(旧FITOMの互換リスト相当)。OPLL系はROMプリセットがOPLLEXへ落ちるものだけがあり、標準OPLL系どうしの相互フォールバックは無い
- OPM/OPZ ハードウェアLFO: 設計検討完了・実装未着手。`docs/opm-opz-hw-lfo.md`
- SCCの波形バンク番号が常に0: `CSCC::setWaveBankNo()`がどこからも呼ばれておらず、複数の`sccwave`バンクを音色データ側から使い分ける経路が無い。バンク0だけなら動作する
- AWM(YMF278)はSwPatchの参照を解決するが音に反映しない。`COPL4AWM`は音量計算・`onNoteOn()`の対象外で、実機のLFO/VIBビット(レジスタ`0x80-0x97`)と波形側の設定を整合させる設計が要る(`patch-structure-design.md`)
- CAdPcmZ280 (YMZ280B/PCMD8) の旧FITOM実装との詳細突き合わせ
- OPLLEX: hwbank.jsonの`builtin`エントリ(`patch_type="OPLLEX"`)経由の名指し参照は未配線(ALGフィールドからは0-7を直接指定できる)
- SSGS3のAMM(MPEG Audioコーデック)部

**HWプラグインの部位ごとのゲイン**
- `devices[]`側の記述を変えると`params`が一致しなくなり、保存済みの`part_gains`が掛からなくなる(読み込み時に警告は出る)。GUIから付け替える手段は無い
- `extra_slot`の2ポート目(`port2`)の部位は扱わない
- `fitom_cli`には値を変える手段が無い(保存済みの値は読み込み時に適用される)
- 同じparamsのデバイスを同時に2つ開けないことを前提にしている(FitomEmuIFは1チップ1ハンドル)。崩れるとparamsでは1台に決まらない

**Y8960**
- カートリッジ側のミキサー(ブロックごとの音量 0x7FF7-0x7FFE)はカートリッジのメモリマップドI/Oで、チップドライバの対象外。扱うならHW I/Fプラグイン側の部位ゲインとして実装する
- 仕様は変更が続いている。仕様書が変わったら`chip-driver-architecture.md`4.5.3節を見直す

**GUI**
- 他画面(デバイス一覧・パッチ一覧・音色エディタ等)への導線
- 内部用MIDIパイプ経由の音色試聴連携(GUIからのSysEx送出、パッチエディタ本体との結合)
- `sf2_channel_windows`を一覧編集する専用ダイアログ(チャンネル単位の切替はCH設定ダイアログにある。`sf2-fluidsynth-integration.md`5節)
- チャンネルレベルメーターのサブデバイスの表示順(現状はConfig側の生成順)

**記録・出力**
- VGMレコーディング: 設計検討完了・実装未着手。`docs/vgm-recording.md`

**例示ファイル・利用者向けマニュアル**
- `config/profiles/`のFITOM用プロファイル6本のうち5本(`emulator_only`/`preset_opl`/`preset_opm`/`preset_opn`/`studio`)が`profile.schema.json`に通らない。廃止済みの`channel_map`・`midi_input`を持ち、必須の`profile_name`が無いものがある(**確認済み**: jsonschemaで検証)。通るのは`emulator_opn_family.profile.json`だけ。`*_profile.example.json`の2本はHWプラグイン側の設定ファイルでこのスキーマの対象外
- `docs/manuals/midi-message-reference.md`の2.6節(SF2直行パス)に、FitomSf2IFが未実装という古い注記と日付が残っている

**プラグイン SDK**
- `plugin_sdk/include/fitom/IHWPlugin.h`のコメント2か所に旧名FitomIFTestが残っている(実機HWのI/FはFitomHwIF)。写しを持つリポジトリへの同期の手間を1回で済ませるため、次に契約を変えるときにまとめて直す


---

## 5. 周辺リポジトリの追従状況

2026年10月23日時点(**確認済み**: 各リポジトリをfetchし、origin側の最新を読んで確認)。

| リポジトリ | 状況 |
|---|---|
| FitomEmuIF | 部位ゲインに追従済み。外部メモリの対応表にOPL2EXの`ADPCM_B`、SSGS/SSGS2の`PCM`がある(`2d0e443`) |
| FitomHwIF | 部位ゲインに追従済み(部位を0個返す実装) |
| FitomSf2IF | `IHWPlugin.h`の写しに部位ゲインの節が無い。部位ゲインは任意の組なので、このままでもロードはできる |
| DSAemuEngine | Y8960のSSG部を`SSGEX`の名前で公開し、旧名`SSGS`を受け付けない(`46c2fbe`) |
| FITOM_staging | `bin/`の実行ファイル・プラグイン・エンジンがすべて8月のビルドで、10月の変更(部位ゲイン、Y8960の追従)はそのままでは鳴らせない。fmgenのプロファイルは`engines/FmGenEngine`を指すが、置かれているDLLは`FmGenEngineApi.dll`。`config_schema/`の`fitom.conf.schema.json`と`profile.schema.json`がFITOM_Xと異なる(`profile.schema.json`の写しには`part_gains`が無く、トップレベルが`additionalProperties: false`のため、部位ゲインを保存したプロファイルは写しを更新するまで検証に通らない)。`emu_msx2pp`プロファイルはOPLLEX・OPL2EXを各2個(`Y8960emuEngine`)で構成していて、SSGEX/SCCEX/OPMは使っていない。新仕様の1回路に合わせるかは未決 |
| FITOM_patch_editor | コードにOPLLEX/SSGEX/SCCEXが無い。SSGS_ADPCMとSILENCEは手元に写した`chip-capabilities.json`に出てくるだけ |
| EPSGemuEngine | 手元がoriginより1件遅れている(早送りで追いつける) |

