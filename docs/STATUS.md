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
| `IHWPlugin.h` | ✅ | HW I/F DLL C API。部位ごとのゲイン(任意の組)、オフライン描画(任意の組)を含む |
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
| `include/fitom/Fnum.h`、`src/Fnum.h` / `Fnum.cpp` | ✅ | F-number テーブルの種別(`FnumTableType`)と、テーブルの実体(`CFnumTable`) |
| `include/fitom/FITOMdefine.h` | ✅ | デバイス種別の定数(`DEVICE_*`) |

#### ボイスデータ
| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/VoiceData.h` | ✅ | HwPatch / SwPatch / FmHwOp / FmSwOp |
| `include/fitom/VoiceProcessor.h` / `.cpp` | ✅ | ベロシティ感度・ソフト LFO 処理 |
| `include/fitom/PatchData.h` | ✅ | Patch / ToneLayer / HwBank / SwBank |
| `include/fitom/DrumData.h` | ✅ | DrumPatch / DrumNote(ドラムキット) |
| `include/fitom/PcmBankData.h` | ✅ | PCMバンク・サンプルゾーンバンク |
| `include/fitom/SccWaveData.h` | ✅ | SCCの波形バンク |
| `include/fitom/PatchManager.h` / `.cpp` | ✅ | バンク管理・JSON I/O |

#### 音源デバイス
設計は`docs/design/chip-drivers.md`(共通の仕組み)と`docs/design/chip-notes.md`(チップ別)。

| ファイル | 状態 | 内容 |
|---|---|---|
| `include/fitom/ISoundDevice.h` / `SoundDevImpl.cpp` | ✅ | CSoundDevice 共通実装 |
| `OPN_new.cpp` | ✅ | COPN (YM2203, FXモード対応) |
| `OPM_new.cpp` | ✅ | OPM / OPP / OPZ (REV/EGS対応)。Y8960のOPMも`chip:"OPM"`としてそのまま扱う |
| `OPN2_new.cpp` | ✅ | COPNA/COPN2 (CSpanDevice、6ch) / COPNB (YM2610無印、ch0/ch3無効化した実効4ch) / COPNARhythm |
| `OPL_new.cpp` | ✅ | OPL/OPL2/COPL3(4OPモード)/COPL3_2(2OP、CSpanDevice)/COPLRhythm(内蔵リズム5パート)/COPL2EX(Y8960拡張OPL2部、chごとの定位つき)+COPL2EXRhythm |
| `OPLL_new.cpp` | ✅ | OPLL/OPLL2/OPLLP/OPLLX/VRC7/COPLLRhythm/COPLLEX(Y8960拡張OPLL部。chごとのROMプリセットバンク選択と定位)+COPLLEXRhythm。ROMプリセットの解決規則は`docs/design/patch-resolution.md` |
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
| `include/fitom/Sf2BankRegistry.h` / `Sf2BankRegistry.cpp` | ✅ | SF2直行パスのバンク解決とプリセット名(`phdr`)の読み出し |

### バックエンド DLL

| ファイル | 状態 | 内容 |
|---|---|---|
| `backends/midi_rtmidi/` | ✅ | RtMidi (Windows/Linux/macOS共通) |
| `backends/midi_pipe/` | ✅ | 内部用MIDIパイプ (`fitom_midi_pipe`、名前付きパイプ/UNIXソケット、パッチエディタ連携用、既定OFF)。`-DFITOM_BUILD_BACKEND_MIDI_PIPE=ON`でビルドするとプロファイルの設定と無関係に常時有効。最大16本まで同時接続でき、接続直後にチャンネル割り当てをプライベートSysEx(sub-cmd 0x03)で通知する。詳細は`docs/interfaces/plugin-midi-pipe.md` |
| `backends/hw_if/CMakeLists.txt` | ✅ | `HWIF_SOURCE_DIR`で外部のHW I/Fソースを取り込む口だけを持つ。実際の`fitom_hw.dll`はFitomHwIF(別リポジトリ)がビルドする(**確認済み**: FitomHwIFのCMakeLists.txtとREADMEを読んで確認) |

### SMF→WAV 変換器

| ファイル | 状態 | 内容 |
|---|---|---|
| `apps/fitom_render/` | 🚧 | `fitom_render`。プロファイルでSMFを演奏してWAVへ書き出す。全HWプラグインがオフライン描画の組(`HWPlugin_InitOffline`/`HWPlugin_Render`)を持つときだけ変換する。SMFの読み込み・WAVの書き出し・時間の進め方は静的ライブラリ`fitom_render_lib`に分けてテストしている。FitomEmuIF・FitomSf2IFは組を実装済みで、実際のプロファイルとの組み合わせは未確認(3節)。利用者向けは`docs/manuals/render-reference.md` |

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

設定ファイルとバンクファイルのスキーマは`config_schema/`にある。文書の一覧は`README.md`の「ドキュメント」。

---

## 2. 機能

| 機能 | 状態 | 関連ドキュメント |
|---|---|---|
| ベロシティ感度 (VTL + VAR〜VRR、全FMチップ + PSG) | ✅ | `docs/design/voice-processor.md` |
| ソフトウェアLFO (LfoControl) | ✅ | `docs/design/voice-processor.md` |
| CC#1 Modulation → LFR=0音色専用のCC駆動LFO | ✅ | `docs/design/midi-processing.md` |
| マスターピッチ可変 (430-450Hz) | ✅ | — |
| ダイナミックボイスアサイン (findBestCh 1パス) | ✅ | — |
| Sustain (CC#64、チップ依存実装) | ✅ | `docs/design/midi-processing.md` |
| Sostenuto (CC#66) | ✅ | `docs/design/midi-processing.md` |
| Portamento/Legato (モノフォニック専用) | ✅ | `docs/design/midi-processing.md` |
| Portamento Rate テーブル (GM2グラフ準拠、セント単位遷移) | ✅ | `docs/design/midi-processing.md` |
| CC#120 forceDamp (全チップ、ALGキャリア判定込み) | ✅ | `docs/design/midi-processing.md` |
| RPN#1 チャンネルファインチューニング | ✅ | `docs/design/midi-processing.md` |
| VoicePatchType システム (音色パッチ互換性分類)。HwBankの登録キーはチップ種別(VoicePatchType)単位 | ✅ | `docs/design/patch-resolution.md` |
| バンクセレクトLSB直接指定モード | ✅ | `docs/design/patch-resolution.md` |
| 無音バンク (CC#0=127、`VOICE_PATCH_SILENCE`) | ✅ | `docs/design/patch-resolution.md` |
| PSGソフトウェアエンベロープ (SoftEnvelope, FM実機準拠ADSR) | ✅ | `docs/design/voice-processor.md` |
| AY-3-8910 HW EG (ext.HWEP)、AY8930拡張モードのバンク切り替え | ✅ | `docs/design/chip-notes.md` |
| リズムモード汎用フィールド (`rhythm_mode`)、ビルトインリズムのプロファイル明示化(`auto_devices`経由を含む) | ✅ | `docs/manuals/profile-reference.md` |
| HWデバイス レイテンシ同期 (GetLatencySamples/SetDelaySamples) | ✅ | `docs/interfaces/plugin-hwif.md` |
| HWPlugin_Shutdown (任意API、二重実行防止) | ✅ | `docs/interfaces/plugin-hwif.md` |
| HWプラグインへ渡すレジスタアドレスは常に実チップのもの(契約) | ✅ | `docs/interfaces/plugin-hwif.md` |
| HWプラグインの部位ごとのゲイン (任意の組、プロファイルの`part_gains`に保存) | 🚧 | `docs/interfaces/plugin-hwif.md`, `docs/manuals/profile-reference.md` |
| マスターボリュームのHWプラグインへの受け渡し (任意の`HWPlugin_SetMasterVolume`、デバイスごと、0-127)。チップのレジスタには掛けない | 🚧 | `docs/interfaces/plugin-hwif.md` |
| SMF→WAVのオフライン変換 (`fitom_render`。HWプラグインの任意の組`HWPlugin_InitOffline`/`HWPlugin_Render`、描画したサンプル数で1msタイマーを進める。MIDIイベントは64フレームの格子に揃えて送る。SMFの`FF 21` MIDI Portでトラックごとに送り先のMPUを選び、無ければMPU0。4以上は警告して捨てる) | 🚧 | `docs/manuals/render-reference.md`, `docs/interfaces/plugin-hwif.md` |
| Sub-device自動生成 (OPNA→FM+SSG+ADPCM-B+Rhythm 等) | ✅ | `docs/design/chip-drivers.md` |
| 同種デバイス自動束ね (CSpanDevice、VoicePatchType基準、チップ試行順はローテーション) | ✅ | `docs/design/chip-drivers.md` |
| リニアステレオ化 (CLinearPanDevice、`stereo_pair`)。左右の分離は2方式: `true`=プラグイン側の出力ルーティング(`pan:1/2`) / `"L"`・`"R"`=チップ自身のL/R出力ビット | ✅ | `docs/design/chip-drivers.md`, `docs/manuals/profile-reference.md` |
| OPL3 4OPモード (COPL3) + 疑似デチューン(`PDT`) | ✅ | `docs/design/chip-notes.md` |
| OPN FXモード (3rd channel special mode、疑似デチューン/非整数倍率/固定周波数) | ✅ | `docs/design/chip-notes.md` |
| 内蔵リズム音源 (COPNARhythm / COPLRhythm / COPLLRhythm、独立レジスタ体系) | ✅ | `docs/design/chip-notes.md` |
| リリース中再トリガー対策 (wasReleasing、OPM/OPN/OPL/OPL3) | ✅ | `docs/design/chip-drivers.md` |
| ADPCM RegMap (Y8950/OPNA/OPNB個別マップ) | ✅ | `docs/design/chip-notes.md` |
| OPLL EGT/RR静的変換 (`SR>0`ならEGT=0+RRレジスタ=SR。OPL系とは別方式。動的制御が実チップで効かないことをFITOM_Xとは別の環境で確かめた確定仕様) | ✅ | `docs/design/chip-notes.md`, `docs/manuals/hwpatch-reference.md` |
| ドラムのチョークグループ (`DrumPatch::chokeGroups`) | ✅ | `docs/design/patch-resolution.md` |
| PCMバンクの複数併用、パッチピッカー用named patchの自動合成 | ✅ | `docs/design/patch-resolution.md`, `docs/manuals/hwpatch-reference.md` |
| pcmbank/samplezonebankへのSwPatch紐づけ (AWMは非対応、4節) | ✅ | `docs/design/patch-resolution.md`, `docs/manuals/hwpatch-reference.md` |
| プロファイルの`banks`の外部ファイル分離、`bank_overrides` | ✅ | `docs/manuals/profile-reference.md` |
| SF2直行パス (FITOM_X本体側: 設定・MIDIディスパッチ・プライベートSysEx・`phdr`パース・GUI表示) | ✅ | `docs/design/sf2-direct-path.md`, `docs/manuals/midi-message-reference.md` |
| 内部用MIDIパイプ (多接続、チャンネル自動割り当て) | ✅ | `docs/interfaces/plugin-midi-pipe.md` |
| MIDIバックエンドのRtMidi単一実装 | ✅ | `docs/interfaces/plugin-midi.md` |
| Y8960の拡張ブロック (OPLLEX/OPL2EX/SSGEX/SCCEX、chごとの8bit定位。仕様書10月20日版に追従) | 🚧 | `docs/design/chip-notes.md`「Y8960 の拡張ブロック」 |

---

## 3. 未確認事項

作ったが、エミュレータの音、またはGUIの目視で確かめていないもの。
ビルドとテストが通ることまでは確かめてある。音程・音量・発音の有無のように波形から数値で決まるものは、`fitom_render`で鳴らして`tools/render_measure/`で測る。音色の良し悪しとGUIの表示は人間が確かめる。
確かめたら行を消し、結果を`docs/worklog/`に書く(不具合が見つかったら4節へ移す)。

実機(FitomHwIF)では、まだ何も鳴らしていない。環境が整っていないため、実機での確認は項目ごとには載せない。

プロファイル名はFITOM_stagingの`config/profiles/`のもの(**確認済み**: FITOM_staging `acb733a`の各プロファイルの`devices[].chip`を数えた)。

**準備**
- [ ] FITOM_stagingの`bin/`を、今のFITOM_Xのビルドに入れ替える。OPM/OPZ(OPP含む)のA4を正しい音程(KC=0x4A)で書く修正は、それより前のビルドに入っていない(前のビルドでは半音高く鳴る)。`bin/`のFitomEmuIF.dllはオフライン描画の組をエクスポートしていない(**確認済み**: `fitom_render`のログ)ので、`fitom_render`で使うにはFitomEmuIF `3d16a0f`以降のビルドに入れ替える。`bin/`はgit管理外でマシンごとに中身が違う(FITOM_stagingの`docs/STATUS.md`の「`bin/`」節)

**`emu_ibmpc`(SAA×2 / OPL2 / OPL3 / OPM / DCSG)**
- [ ] SAA1099の音程: A4が以前と同じ音程で鳴る(プラグインのクロックを読むように変えた。プラグイン設定の`clock`は8000000なので変わらないはず)。ピッチベンドが半音単位で効く。ソフトウェアLFOのある音色でビブラートがかかる。システム設定ダイアログでマスターピッチを430/450Hzへ変えると音程が追従する。FitomEmuIFはプラグイン設定の`clock`をそのまま`GetClock()`で返し、`clock`の無い設定は開く時点で拒否する(**確認済み**: FitomEmuIF `87d0a6c`のソースを読んで確認)

**`emu_psg_stereo`(SAA / SSGS / SSGS2 / SSGS3 / DSG / EPSG / SCC / DCSG)**
- [ ] SSGS/SSGS2/SSGS3のSSG部が鳴る
- [ ] `CSSGSAdPcm`が鳴る。エミュレータ(EPSGemuEngine)はコーデックをYMZ280Bと同じ4bit ADPCMと仮定している。FitomEmuIFはSSGS/SSGS2の`PCM`へカタログ種別`SSGS_ADPCM`のイメージを渡す(**確認済み**: FitomEmuIF `2d0e443`のソースを読んで確認)。鳴ったとしても、コーデックが仮定のため実機と同じ音かは判断できない
- [ ] DSG(YM2163)の音。GUI: パッチピッカーのDSGカテゴリにビルトインの20音色が並ぶ。内蔵リズム(CC#0=0x70)の一覧にDSGが出て、5パートが鳴る
- [ ] SCC・DCSGの音量レジスタ変換則、AY8930拡張モードのバンク切り替え

**`emu_opl` / `emu_opl_stereo`(OPL3 / OPL4 / Y8950[リズム])**
- [ ] OPL4 AWMのピッチベンド・ファインチューン・ソフトウェアLFOのビブラートが効く(kfs→1/128半音の換算を直した。直す前はほとんど動かなかったはず)。テストはレジスタ値を見るだけ
- [ ] Y8950のチャンネル名がGUIで`FM1`〜と表示される。`fitom_cli`のデバイス一覧では`Y8950`。YM3526(`chip: "OPL"`)を使うプロファイルはstagingに無い

**`emu_opll` / `emu_opll_stereo`(OPLL[リズム] / OPLLX / OPLLP / VRC7 / OPLLEX)**
- [ ] OPLLの内蔵リズムのSD/TOMが、ノート番号に応じてメロディ側と同じ音程で上下する(F-numberを9bitへ落とすよう直した)。テストはメロディ側と同じレジスタ値になることを見るだけ
- [ ] パッチピッカーのOPLLEX: CH番号→パッチ選択→カテゴリ「OPLLEX」→バンク「ROM Preset」に、`OPLL: `/`OPLLX: `/`OPLLP: `/`VRC7: `の接頭辞つきで60音色が並ぶ。試聴でその音色が鳴る。確定後、MIDIモニターのパッチ名も同じ表記になる。`FITOMBridge`にはテストが無く、ビルドが通ることまで確かめた

**`emu_msx2pp`(Y8960: OPLLEX[リズム] / OPL2EX / SSGEX / SCCEX / OPM)**
- [ ] Y8960の拡張ブロックのうちOPLLEXとOPL2EX(内蔵ADPCM-B・リズムを含む)が鳴る。OPLLEXの内蔵リズムのSD/TOMの音程も上の項目と同じく確かめる。OPLLEXのリズムへのフォールバック(`resolveBuiltinRhythm()`)はコードを読んだだけ。OPL2EX内蔵ADPCM-Bの分周比をY8950と同一としているのは**推測**(仕様書に記載が無い。レジスタ配置がY8950と同一であることから類推)
- [ ] Y8960のSSGEX/SCCEX/OPMが鳴る。このプロファイルが5ブロックを1個ずつ載せていることは**確認済み**(FITOM_staging `ef55367`のプロファイルとサブプロファイルを読んだ)

**`fmall`(OPL3 / OPL4 / OPLLEX[リズム] / OPNBB / OPZ)**
- [ ] OPZの固定周波数モード(`ext.FIX`)。`COPM::updateVoice()`は`ext.FIX`が0でないとき実機のbit5を立てるが、固定周波数として働くかは確かめていない。利用者向けマニュアルは「未対応」としている

**プロファイルを問わないもの**
- [ ] モノ(CC#126=1)の発音。レガート(CC#68)のオン・オフそれぞれで: 最初のノートが鳴る(レガートオンでは鳴っていなかった)。前のノートを押さえたままプログラムチェンジすると次のノートが新しい音色で鳴り、鳴らすチップが変わる場合は前の音が残らない。ソフトウェアLFOのある音色(`LFS=0`)を弾き直してもビブラートが2秒後に止まらない。レガートオンで同じ音色のノートをつなぐと、エンベロープが再トリガーされない
- [ ] ポリで、リリース中(キーオフから2秒以内)のチャンネルを再利用したノートでも、`LFS=0`の音色のビブラートが止まらない
- [ ] システム設定ダイアログの部位ゲインのスライダー(GUI)と、その音。実物のFitomEmuIFと名前指定の部位ゲインに対応したエンジン(YMEngine/DSGemuEngine/DSAemuEngine)を組み合わせて鳴らしたことは無い。`CFITOM`/`FITOMBridge`層(物理チップの添字からポートへの対応とロック)はテストが通っておらず、`HWPlugin_Write`と並行しないことはロックを取る箇所を読んで判断しただけ
- [ ] SF2直行パスのMIDIモニター表示と、CH設定ダイアログの窓の切替(GUI)
- [ ] レジスタダンプモニターのスクロールバー、チャンネルレベルメーターの束ねられたチップの表示(GUI。いずれも修正後の再確認)
- [ ] YMZ280B(PCMD8)で、CC#7を上げるほど大きく鳴る(Total Levelの向きを直した。直す前は最大音量で無音になっていたはず)。PCMD8を使うプロファイルはstagingに無い

**外部との連携**
- [ ] 外部パッチエディタの4引数キオスク起動が、layered/device/drumそれぞれで正しい編集画面を開くこと。`kind="device"`は、プロファイルの`devices[]`に対応するHWプラグインDLLがロードできないと解決に失敗する(`Config::buildDevice()`がDeviceEntryを作らないため)が、その場合の挙動は未検証
- [ ] 内部用MIDIパイプと実際のパッチエディタとの接続(PowerShellの簡易クライアントでの疎通確認までは実施済み)
- [ ] 内部用MIDIパイプの10本目の接続(ch9、起動時はリズムチャンネル)で、`CC#0=0x79`によりメロディチャンネルへ切り替えてから直接デバイス選択で試聴できること。コードを読んで判断しただけ
- [ ] マスターボリューム: FitomEmuIF(`3d16a0f`で`HWPlugin_SetMasterVolume`を実装済み)と組み合わせて、システム設定ダイアログのスライダーとマスターボリュームのSysExで、エミュレータの出力の音量が変わる。FITOM_X側は、検証用プラグインが値を受け取ることまでテストで確かめた
- [ ] SF2直行パスのFitomSf2IFとFITOM_Xを組み合わせての音出し(FitomSf2IFは実装済み。同リポジトリの`docs/STATUS.md`も実機検証は未実施としている)
- [ ] `fitom_render`: FitomEmuIF(`3d16a0f`)・FitomSf2IF(`12803b9`)はオフライン描画の組を実装済み。stagingのプロファイルでSMFを変換し、WAVを聴いて実時間で鳴らしたときと同じ音・同じテンポになっていること。SF2とFM/PSGの発音の時刻が揃っていること(イベントを64フレームの格子に揃えて送るため)。SMFの`FF 21`でMPU1〜3へ送ったトラックが、そのMPUの設定(`sf2_channel_windows`等)で鳴ること。`s16`/`s24`/`f32`それぞれが一般的なプレイヤーで開けること。FITOM_X側は、検証用プラグインで組の扱い・時刻とタイマーの位置・WAVのヘッダとサンプル値をテストで確かめた。実物のFitomEmuIF(`3d16a0f`のビルド)とYMFMEngineだけを載せたプロファイルでは、変換が通り、OPL3/OPL4の音程とパンが測れることを**確認済み**。FitomSf2IFとの組み合わせと、聴いての確認はしていない

---

## 4. 既知の未対応・将来課題

**MIDI**
- SF2直行パスには、起動時にプロファイルの`master_volume`/`master_pitch`を送らない(値が変わったときだけSysExで送る)。HWプラグインへは起動時にも`HWPlugin_SetMasterVolume`で渡すので、SF2だけ扱いが違う。**確認済み**: `CFITOM::initDevices()`と`setMasterVolume()`/`setMasterPitch()`を読んで確認
- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal(FM音源に対応するパラメータが無いため意図的に非対応)
- CC#2/CC#4 の変数分離
- `CInstCh::getLastDeviceIndex()`(GUIのMIDIモニターと`fitom_cli`のDevice表示)は、ノートが実際に鳴っているデバイス(`NoteHist::dev`)ではなく、レイヤーが解決したデバイス(`deviceIndex`)を返す。空きが無くフォールバック先のデバイスへ渡ったノートでは、表示が実際と食い違うはず。**確認済み**: コードを読んで確認。表示では確かめていない
- 外部MIDIクロック(0xF8)への同期は無い。コアには受け口(`MidiProcessor`の`clockEnabled_`→`CInstCh::midiClockCallback()`)があるが処理は空で、RtMidiバックエンドはTiming Clockを受信時に捨てている(`ignoreTypes`)。**確認済み**: コードを読んで確認

**パッチの解決**
- `PatchManager::loadDrumBankJson()`は呼び出し元が無い(実際に使うのは`loadDrumKitJson()`)。**確認済み**: grep

**チップドライバ**
- OPK(YM7116)/OPK2(YM7129)のドライバ(旧実装は`legacy/src/OPK.cpp`にある。**確認済み**: `DeviceFactory.cpp`に生成処理が無く、`Config.cpp`に`chip`名の対応も無いことをgrepで確認)
- SD1/MA3/MA5/MA7のドライバ(パッチ種別とVoiceGroupの対応表だけがある。**確認済み**: `DeviceFactory.cpp`に生成処理が無いことと`Config.cpp`の対応表を読んで確認)
- チップファミリーをまたぐフォールバック(旧FITOMの互換リスト相当)。ファミリー内のフォールバックは`acceptsFallback`で実装済み(`docs/design/chip-drivers.md`「フォールバック」)。OPLL系のROMプリセットは、OPLLEXへ落ちるもの以外はチップ間でフォールバックしない(ROMの中身が実機ごとに違うため)
- OPM/OPZ ハードウェアLFO: 設計検討完了。着手前の土台(`$38+ch`のドライバ側保持、`reset()`のLFOレジスタ初期化)は済み。波形・SELECTのCCと第2LFOは未着手。`docs/proposals/opm-opz-hw-lfo.md`
- SCCの波形バンク番号が常に0: `CSCC::setWaveBankNo()`がどこからも呼ばれておらず、複数の`sccwave`バンクを音色データ側から使い分ける経路が無い。バンク0だけなら動作する
- AWM(YMF278)はSwPatchの参照を解決するが音に反映しない。`COPL4AWM`は音量計算・`onNoteOn()`の対象外で、実機のLFO/VIBビット(レジスタ`0x80-0x97`)と波形側の設定を整合させる設計が要る(`docs/design/patch-resolution.md`)
- CAdPcmZ280 (YMZ280B/PCMD8) の旧FITOM実装との詳細突き合わせ
- OPL系・OPLL系のF-Numberの精度が粗く、低めにずれる(**未確認の候補**、計算のみ)。F-Numberの表はA4を内部値580(OPL2、3.579545MHz)のように1024未満の値で持ち、`updateFreq()`がそれを切り捨てで縮めてレジスタへ書く(OPLは`>>1`、OPLLは`>>2`)。OPLLのレジスタ値は1オクターブで約85〜172しか使わず、低い側の音で最大十数セント低くなる計算。全ノートについて、レジスタ値から実チップの周波数を求めてセントの誤差を並べれば決まる(`tests/test_fm_clock.cpp`の換算式が使える)。直すなら、blockを1つ下げて上位ビットを使うか、丸めて縮める
- `DEVICE_OPN3_L3`(YMF297のOPL3部)のクロックの換算は、OPL3と同じ÷4にしている(**未確認の候補**: YMF297のクロックとFM部の分周を確かめていない)。stagingにこのチップを使うプロファイルは無い
- OPLLEX: hwbank.jsonの`builtin`エントリ(`patch_type="OPLLEX"`)経由の名指し参照は未配線(ALGフィールドからは0-7を直接指定できる)
- SSGS3のAMM(MPEG Audioコーデック)部
- OPL4 AWMはマスターピッチを読まない。`COPL4AWM::getFnumber()`はALSAと同じ基準点からの計算で、F-numberテーブルもマスターピッチも使わない。ROM波形の校正値(`pitch_offset`)が440Hz基準なら、マスターピッチを変えてもAWMだけ音程が変わらない。**確認済み**: コードを読んで確認。音では確かめていない
- ADPCM-BとYMZ280Bは、ラウドネス(GM2の`40·log10(L/127)`dBの目盛り)を線形のゲインのレジスタへそのまま写しているので、CC#7・ベロシティのdB上の効きがFM/PSGの半分になる。実機のレジスタが線形かは**未確認の候補**(YMZ280Bのエミュレータ[EPSGemuEngine、MAME由来]は線形。ADPCM-Bのエミュレータの扱いは見ていない。YMZ280Bのデータシートは記載が無い)。実機の出力を測るか、YM2608のアプリケーションマニュアル等でADPCM-Bのボリュームの定義を見れば決まる。直すなら、レジスタへ`(L/127)^2`に比例する値を書く

**HWプラグインの部位ごとのゲイン**
- `devices[]`側の記述を変えると`params`が一致しなくなり、保存済みの`part_gains`が掛からなくなる(読み込み時に警告は出る)。GUIから付け替える手段は無い
- `extra_slot`の2ポート目(`port2`)の部位は扱わない
- `fitom_cli`には値を変える手段が無い(保存済みの値は読み込み時に適用される)
- 同じparamsのデバイスを同時に2つ開けないことを前提にしている(FitomEmuIFは1チップ1ハンドル)。崩れるとparamsでは1台に決まらない

**Y8960**
- カートリッジ側のミキサー(ブロックごとの音量 0x7FF7-0x7FFE)はカートリッジのメモリマップドI/Oで、チップドライバの対象外。扱うならHW I/Fプラグイン側の部位ゲインとして実装する
- 仕様は変更が続いている。仕様書が変わったら`docs/design/chip-notes.md`「Y8960 の拡張ブロック」を見直す

**GUI**
- 他画面(デバイス一覧・パッチ一覧・音色エディタ等)への導線
- 内部用MIDIパイプ経由の音色試聴連携(GUIからのSysEx送出、パッチエディタ本体との結合)
- `sf2_channel_windows`を一覧編集する専用ダイアログ(チャンネル単位の切替はCH設定ダイアログにある。`docs/design/sf2-direct-path.md`「GUI」)
- チャンネルレベルメーターのサブデバイスの表示順(現状はConfig側の生成順)

**記録・出力**
- VGMレコーディング: 設計検討完了・実装未着手。`docs/proposals/vgm-recording.md`。SMFからのVGM出力は、サンプル単位の時計を持つ`fitom_render`に足す方針(`docs/plans/smf-to-wav.md`)

**ビルド**
- 呼び出し元の無いコード: `ISoundDevice::reset()`(複合デバイスが子へ転送するだけ)、`core/src/Fnum.h`/`Fnum.cpp`の`CFnumTable`(グローバル`theFnum`は生成される)、`CFITOM::getDeviceVoiceType()`、`calcEffectiveLevel`(宣言のみで定義が無い)と`linearToDb`(`VoiceProcessor.h`)、`CAdPcmBase::resolvePcmEntry()`、内部用MIDIパイプの`MidiInDevice::listenPipe`。**確認済み**: grepで参照を数えて確認
- `gui/mfc_shim/`はどの`CMakeLists.txt`からも`add_subdirectory`されておらず、ビルド対象外。中身は廃止済みの`FmEnginePort`等を参照していて古い。`CMakePresets.json`が設定する`FITOM_GUI_MFC`もどこからも読まれない。残すかは未決(**確認済み**: grep)
- `vcpkg.json`の依存に`boost-interprocess`が残っているが、コードはBoost.Interprocessを使っていない(**確認済み**: `core`/`apps`/`gui`/`backends`で`boost/interprocess`のインクルードが無いことをgrepで確認)。依存を外すかは未決

**設定スキーマ**
- `profile.schema.json`の`devices[].engine`の`examples`が`"YMEngine"`/`"AYEngine"`のまま。実際のプロファイルが書く値は`"engines/YMFMEngine"`等(**確認済み**: FITOM_stagingの全プロファイルの`engine`の値を数えた)。FITOM_stagingの写しも同じで、原本の修正を待っている

**プラグイン SDK**


---

## 5. 周辺リポジトリの追従状況

2026年10月23日時点。FITOM_stagingは10月24日、FitomEmuIF・FitomHwIF・FitomSf2IFは10月25日に見直した(**確認済み**: 各リポジトリをfetchし、origin側の最新を読んで確認)。
他のリポジトリへの作業の依頼文は`docs/plans/request-<リポジトリ名>.md`に置く。依頼先の作業が終わったら、この表を更新して依頼文を消す。

| リポジトリ | 状況 |
|---|---|
| FitomEmuIF | 部位ゲインに追従済み。外部メモリの対応表にOPL2EXの`ADPCM_B`、SSGS/SSGS2の`PCM`がある(`2d0e443`)。`HWPlugin_SetMasterVolume`(振幅は(値/127)²)とオフライン描画の組を実装済み(`3d16a0f`)。`IHWPlugin.h`の写しは`faaf183`の版と同一。その後FITOM_X側でオフライン描画の書き込みの反映を「64フレームの境界まで遅らせてよい」と緩めたが、コメントだけの変更でABIは変わらないので、同期しなくても動作は変わらない |
| FitomHwIF | 部位ゲインに追従済み(部位を0個返す実装)。`HWPlugin_SetMasterVolume`とオフライン描画の組は、実機なので実装しない。`IHWPlugin.h`の写しは`faaf183`の版と同一(`5a97edc`)。その後のFITOM_X側の変更はオフライン描画のコメントだけで、FitomHwIFには関係しない |
| FitomSf2IF | オフライン描画の組を実装済み(`12803b9`)。部位ゲインと`HWPlugin_SetMasterVolume`はエクスポートしない(SF2のデバイスにはマスターボリュームをSysExで送る)。`IHWPlugin.h`の写しは`faaf183`の版と同一。fluidsynthが64フレーム単位で合成するので、書き込みの反映は次の64フレームの境界まで遅れる(FitomSf2IFのテストで**確認済み**、`b64b307`)。FITOM_X側は契約をこれに合わせて緩め、`fitom_render`がMIDIイベントを64フレームの格子に揃えて送る。FitomSf2IFの`docs/STATUS.md`は同期の方法を「描画の頭に空フレームを置く」と書いていて、実際の方法と食い違う。FitomSf2IF側で直す |
| DSAemuEngine | Y8960のSSG部を`SSGEX`の名前で公開し、旧名`SSGS`を受け付けない(`46c2fbe`) |
| FITOM_staging | `config_schema/`はFITOM_Xの`d214ff9`に同期済み、`emu_ibmpc`の`_comment`のSAAのクロックの注記も直してある(`dbd2499`)。その後FITOM_X側で`profile.schema.json`の`devices[].clock`の`description`を変えたので、説明文だけ食い違う(構造は同一)。`bin/`のFitomEmuIF.dllはオフライン描画に対応していない(3節の「準備」)。FITOM_stagingからの依頼文`docs/plans/request-FITOM_X.md`は、FITOM_staging側で消す約束になっている。`emu_msx2pp`プロファイルは、Y8960の5ブロック(OPLLEX/OPL2EX/SSGEX/SCCEX/OPM)を1個ずつ載せている(10月25日にorigin `ef55367`まで読んで確認) |
| FmGenEngine | A4の音程が、出力のサンプルレートによって数セントずれる。`emu_fmgen_opn`のOPN系(7,159,090Hz)で、44.1kHzでは-3.6セント、48kHzでは+5.5セント、96kHzでは-7.6セント(**確認済み**: `tools/render_measure/fm_pitch.py`で測定)。同じドライバ・同じクロック・同じレートのYMFMEngineは±0.1セント以内なので、FITOM_Xの外の問題。**推測**: FmGenEngine(fmgen)のレート変換の丸め。コードは読んでいない |
| FITOM_patch_editor | コードにOPLLEX/SSGEX/SCCEXが無い。SSGS_ADPCMとSILENCEは手元に写した`chip-capabilities.json`に出てくるだけ |
| EPSGemuEngine | 手元がoriginより1件遅れている(早送りで追いつける) |

