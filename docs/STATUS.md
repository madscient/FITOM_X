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
| `OPN2_new.cpp` | ✅ | COPNA/COPN2 (CSpanDevice、6ch) / COPNB (YM2610無印、ch0/ch3無効化した実効4ch) / COPNARhythm |
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
| `backends/midi_rtmidi/` | ✅ | RtMidi (Windows/Linux/macOS共通、2026年7月に旧midi_wms/midi_winmm/midi_alsaの3実装から統合) |
| `backends/midi_pipe/` | ✅ | 内部用MIDIパイプ (`fitom_midi_pipe`、名前付きパイプ/UNIXソケット、パッチエディタ連携用、既定OFF)。最大16本まで同時接続対応(2026年7月〜、接続ごとに専用スレッド)。接続確立直後、チャンネル割り当てをプライベートSysEx(sub-cmd 0x03)で通知し、クライアント側は自分でチャンネルを選ばない設計に変更(複数パッチエディタインスタンス同時起動時の衝突回避)。詳細は`plugin-midi-pipe.md`参照 |
| `backends/hw_if/CMakeLists.txt` | ✅ | FitomIFTest submodule 統合 |

### GUI

| ファイル | 状態 | 内容 |
|---|---|---|
| `gui/bridge/FITOMBridge.h` | ✅ | UIフレームワーク非依存のコアブリッジAPI |
| `gui/bridge/FITOMBridge.cpp` | ✅ | ブリッジ実装 |
| `apps/fitom_gui/` | 🚧 | Dear ImGui + GLFW + OpenGL3 導入済み。ルート画面のMIDIモニターバンド(CH毎のBank/Program/Volume/Note/Device/Fnumber表示、MPU切替、128ノートキーボードビュー+発光エフェクト)を実装済み。Bank/Program表示のダブルクリックで外部パッチエディタ(別リポジトリ`FITOM_patch_editor`、実行ファイルは`fitom_gui`と同じディレクトリに配置想定)をキオスクモード(`<profile.json> <hwbank-file> <prog>`)で子プロセス起動する機能を実装済み(パッチエディタ未検出・起動失敗時はImGuiモーダルでエラー表示。実機での起動確認は未実施、下記STATUS.md注記参照)。CH番号のシングルクリックでCH設定ダイアログ(`ChSettingsDialog`)を開き、Volume(CC#7)/Expression(CC#11)/リズム⇔インストゥルメント切替(CC#0特殊値)/Poly⇔Mono切替(CC#126/127)/パッチ選択を変更できる機能を実装済み。パッチ選択はCC#0→CC#32→Prog.chgの階層ブラウジングを行う`PatchPickerDialog`(直接デバイス選択モードも対応)、リズムチャンネルはドラムキットのフラット一覧を別途実装済み。適用ロジックはGUI側から`IMidiCh`のsetterを個別に叩かず、`FITOMBridge::sendControlChange`/`sendProgramChange`経由でコアの既存MIDI処理経路(`MidiProcessor::processControl`/`IMidiCh::progChange`)を再利用する方式(2026年7月)。パッチピッカーのProg.chg階層は、行をマウスボタンダウンした瞬間にCC#0/CC#32/Prog.chg/Note On(C4、`FITOMBridge::sendNoteOn`/`sendNoteOff`新設)を送り、ボタンアップでNote Offを送る「押している間だけ鳴る」試聴動作を実装済み(`ImGui::IsItemActivated`/`IsItemDeactivated`でボタンダウン/アップを直接検出。同じ行への連打も含め、押すたびに必ず送り直す)。確定は「選択」ボタンのみで行う(以前はダブルクリックでも確定していたが、連打時に試聴のNote Onと確定側のNote Offが競合し音が鳴らなくなる不具合があり、2026年7月に確定操作を「選択」ボタンへ一本化して解消)。確定時は試聴中の値のままNote Offのみ送り(通常は既にボタンアップで止まっている)、ピッカー/CH設定いずれのキャンセルでも試聴前(open()時点)のCC#0/CC#32/Prog.chgへ復元するメッセージを送り直す。CH設定ダイアログのVolume/Panpot/Expressionスライダーも操作するたびにCC#7/#10/#11を即時送信してプレビューでき、キャンセル時は開いた時点の値へ復元する(2026年7月)。MIDIモニターのバンク名・パッチ名解決は、直接デバイス選択モード(HwBankRegistry参照)にも対応済み(2026年7月修正。以前は通常モード[PatchBank]とリズムチャンネルのみ対応で、直接モードのチャンネルは常に数値フォールバック表示になっていた)。MIDIモニターのMIDIポート名部分のクリックでMIDIポート設定ダイアログ(`MidiPortSettingsDialog`)を開き、MPU(`CFITOM::getMpuCount()`、現状4)分のMIDI入力ポート割り当てをそれぞれドロップダウン(システムが現在列挙するポート名一覧、`FITOMBridge::getAvailableMidiInputPorts()`)から変更できる機能を実装済み(2026年7月新設)。バリデーションは重複設定(複数ポートへの同一MIDI IN割り当て)チェックのみで、違反時はエラーメッセージボックスを表示してOKを継続させない。OKで閉じると`FITOMBridge::setMidiInputPorts()`が既存の全MIDI入力ポートを閉じてから選択内容で開き直し(即時反映)、`FITOMConfig`側の設定(現在のプロファイル状態)を更新したうえで、`FITOMBridge::saveCurrentProfile()`経由で現在ロード中のプロファイルファイルへ書き戻す。この変更に伴い、コア側(`CFITOM::init()`)もMIDI入力ポート数に関わらず常にMAX_MPUS(4)分のMidiProcessor/チャンネルを生成するよう修正した(以前は設定済みポート数分しか生成せず、未設定のMPUには実行中に新規ポートを割り当てられなかった)。MIDIモニター左上の歯車アイコンボタン(`gearIconButton()`、日本語フォントのグリフ収録範囲に依存しないようImDrawListへの直接描画で実装)からシステム設定ダイアログ(`SystemSettingsDialog`)を開き、マスターボリューム・マスターピッチ(430〜450Hz)を変更できる機能を実装済み(2026年7月新設。ChSettingsDialogのVolume/Panpot/Expressionスライダーと同じくドラッグ中は即時プレビュー、キャンセルで開いた時点の値へ復元)。こちらもOKで`FITOMBridge::saveCurrentProfile()`によりプロファイルファイルへ書き戻す。プロファイルスキーマに`master_volume`/`master_pitch`フィールドを新設し(`config_schema/profile.schema.json`)、`FITOMConfig::buildFromProfile()`で読み込み・`FITOMConfig::saveProfile()`で書き戻す(ロード時のJSON全体を`profileJson_`に保持しておき、GUIから変更されるフィールドのみ上書きして書き出すことで、devices/hw_plugins/banks等の他のフィールドはそのまま維持される)。あわせて、`FITOMBridge::setMasterPitch()`が従来`FnumRegistry`を直接叩いておりConfig側の値が更新されず発音中チャンネルへのF-number即時反映も行われていなかったバグを、`CFITOM::setMasterPitch()`経由に修正した。デバイス一覧・パッチ一覧(フラット表示)等、他画面への導線は未着手(該当描画関数は`[[maybe_unused]]`で温存) |

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
| `docs/manuals/midi-message-reference.md` | ✅ |
| `docs/manuals/midi-implementation-chart.md` | ✅ |
| `docs/manuals/hwpatch-reference.md` | ✅ |
| `docs/manuals/swpatch-reference.md` | ✅ |
| `docs/manuals/native-patch-reference.md` | ✅ |

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
| HWPlugin_Shutdown (未エクスポート時は何もしないオプショナルAPI、二重実行防止) | ✅ | `plugin-hwif.md` |
| GUI MIDIモニターバンド (CH毎表示 + 128ノートキーボードビュー + 発光エフェクト) | ✅ | — |
| GUI CH設定ダイアログ + パッチピッカーダイアログ (Volume/Expression/リズム⇔インストゥルメント切替/Poly⇔Mono切替/CC#0→CC#32→Prog.chg階層ブラウジング + Prog.chg選択時の試聴(Note On C4)・キャンセル時の復元。GUIからは`FITOMBridge`のMIDI送信メソッド経由でコアの既存MIDI処理経路を再利用) | ✅ | — |
| 内部用MIDIパイプ (`fitom_midi_pipe`、パッチエディタ試聴連携) | ✅ | `plugin-midi-pipe.md` |
| 内部用MIDIパイプの多接続化(最大16本+接続直後のチャンネル自動割り当てSysEx) | ✅ | `plugin-midi-pipe.md` |
| MIDIバックエンドDLLをRtMidi単一実装へ統合 (旧midi_wms/midi_winmm/midi_alsaの3実装廃止、SysEx未対応だった既存欠陥を解消、macOS対応を新規追加) | ✅ | `plugin-midi.md` |
| OPNB(YM2610無印)の誤分類修正 (VOICE_PATCH_OPNからOPN2側へ、COPNB新設でSSG/ADPCM-Aサブデバイス自動生成・実効4ch化に対応。ステージング環境からの指摘で発覚) | ✅ | `chip-driver-architecture.md` |
| `CSoundDevice::chState_`等、チャンネル数を固定長配列で持っていた箇所のvector化 (OPL4 AWM=24chがMAX_CHS=16固定配列を超えて範囲外アクセスするクラッシュを修正。`chState_`本体に加え`CPSGBase::lfoTL_`/`envelopes_`、`CLinearPanDevice::masterVolume_`/`masterPan_`も同種のためvector化。ステージング環境での「無言で強制終了」報告から発覚) | ✅ | `chip-driver-architecture.md` |
| GUI MIDIポート設定ダイアログ (`MidiPortSettingsDialog`、MIDIモニターのポート名クリックで開く。MPU 4面分のMIDI入力ポート割り当てをドロップダウンで変更、重複設定のみバリデーション、OKで即時反映+プロファイル書き戻し。`CFITOM::init()`をMIDI入力ポート数に関わらず常に4MPU分生成するよう修正し、実行中の未使用MPUへのポート割り当てに対応) | ✅ | `config-design.md` |
| GUI システム設定ダイアログ (`SystemSettingsDialog`、MIDIモニター左上の歯車アイコンボタンで開く。マスターボリューム/マスターピッチ(430〜450Hz)をスライダーで即時プレビュー、OKでプロファイル書き戻し。プロファイルスキーマに`master_volume`/`master_pitch`を新設し、`FITOMConfig::saveProfile()`(新設、ロード時JSONへの差分上書き書き戻し)経由で永続化。`FITOMBridge::setMasterPitch()`がConfig/発音中チャンネルへの反映を素通りしていたバグも合わせて修正) | ✅ | `config-design.md` |
| レジスタダンプモニターのコア層基盤(2026年7月新設)。`HWPort::write()`/`writeBurst()`が実際にHWPlugin_Write/WriteBlockへ渡した値をそのまま`shadowRegs_`(mutex保護、addr=0x0000-0xFFFF全域)にミラーする「最後に書き込んだ値」のキャッシュを新設(`HWPort::getShadowReg()`/`getShadowRegRange()`)。実チップにはレジスタ読み出しAPIが無いため、あくまでFITOM_Xが最後に書き込んだ値を返す点に注意。`CFITOM`に物理チップ単位の列挙(`PhysicalChipInfo`、`getPhysicalChipCount()`/`getPhysicalChipInfo()`/`getPhysicalChipRegisterDump()`)を新設し、サブデバイス自動生成(OPNA→FM+SSG+ADPCM-B等、同一`HWPort`を共有)や同種デバイス自動束ね(spanGroups)・リニアステレオ化(stereoPairPort)で生成される複数の論理`ISoundDevice`を、物理チップ単位(`buildPhysicalChipList()`が`initDevices()`末尾でHWPortポインタの同一性から判定)にまとめて1エントリとして扱う。2ポートチップ(OPN2/OPL3等)は`getPhysicalChipRegisterDump()`がport1を0x000-0x0FF、port2を0x100-0x1FFにpackして返す。`HWPort`は`HWPlugin_Open()`に渡したparams_json(type/serial/port/slot、またはFMHWIFのengine/chip等)も保持し、`getPhysicalChipName()`でhwif接続情報由来の物理チップ名(例:"SPFM_TOWER COM3 slot0"、"YMEngine/OPNA")を組み立てて返す(`PhysicalChipInfo::physicalName`)。**Fix(2026年7月)**: 高位ポート(0x100以降)を使うチップが実際に接続されている物理HWポートは1つだけ(SPFM 2スロット等の`extra_slot`を使わない一般的な構成)の場合に、高位アドレス側が表示されない不具合を修正。OPNA/OPN2系(`OPN2Port2`/`OffsetPort`経由で内部的に同一`HWPort`へ+0x100して書く)やOPL3(addr>=0x100を直接同一`HWPort`へ書く)は、`config_`側に別々の`port`/`port2`が存在しなくても0x100-0x1FFへの書き込みは同じ`HWPort`のシャドウレジスタに記録されているため、`port2`の有無だけでダンプ範囲を判定していたのが原因。`PhysicalChipInfo::dumpSize`を新設し、`port2`が無い場合も`CFITOM::getDeviceRegSize(deviceType)`(kDevMapの既知のレジスタ空間サイズ)が0x100を超えていれば、その分だけ同一`HWPort`から一括で読み出すよう修正した | ✅ | — |
| レジスタダンプモニターのBridge API・GUI(2026年7月新設、同月中に表示方式を変更)。`FITOMBridge::getHwChips()`/`getHwChipRegisterDump()`でコア層の物理チップ列挙・レジスタダンプを公開(`FITOMChipInfo::physicalName`はhwif接続情報由来の物理チップ名。当初`descriptor`としてFITOM_X内部のチップドライバ分類=論理チップ名を返していたが、GUIでの目視確認で違和感が指摘され物理チップ名に差し替えた)。`apps/fitom_gui`に`RegisterDumpWindow`を新設し、MIDIモニター左上の歯車アイコン隣の「REG」ボタンでMIDIモニター本体とルート画面上でオルタネート表示(排他的に切り替え)する(当初は独立した別ウィンドウとして重ねて表示していたが、実際にGUIを使ったユーザーからの指摘によりMIDIモニターとの排他表示に変更。ボタンは表示中「MIDI」に切り替わり元に戻せる)。物理チップごとに16進数生値のグリッド(`ImGui::Table`、1ポートは16行、2ポートは32行)を表示し、直前フレームとの差分検出で値が変化したセルは`ImGui::TableSetBgColor`でセル背景を発光させ、`renderKeyboardView()`の発音グロー(main.cpp、ベロシティ連動の発光エフェクト)と同じ「発光開始時刻からの経過時間でフェード」方式(0.6秒)で徐々に消える。表示専用(値の編集不可)。**Fix(2026年7月)**: 各チップのテーブルに`ImGuiTableFlags_ScrollY`+行数からの高さ見積もりで固定高さを与えていたが、見積もりがセルパディング分だけ実際の内容より小さく、バンド(チップ)ごとに常にスクロールバーが出てしまっていた不具合をユーザー指摘で修正。テーブルの高さ指定・ScrollYを廃止し、行数分へ自然にフィットさせる(自動サイズ)方式に変更。ウィンドウ全体の内容が窓の高さを超えた場合はMIDIモニターバンドと同じくルートウィンドウ側のスクロールに任せる | ✅ | — |
| チャンネルレベルメーターのコア層基盤(2026年7月新設。Bridge API・GUIは未着手、下記「既知の未対応」参照)。レジスタダンプモニターを左右2ペイン化し、左に「チップごとの物理チャンネル1本ずつのバー(ベロシティ/TL連動の疑似メーター。FITOM_Xは音声合成を行わないため実音量信号は存在せず、`ChState::isActive()`+`velocity`による疑似表示である点に注意)」、右に既存のレジスタダンプを表示する計画の土台。`PhysicalChipSubDevice`(`device`/`deviceType`/`chCount`)を新設し、`PhysicalChipInfo::subDevices`として物理チップ1個を構成する論理`ISoundDevice`群(例: OPNBならFM+SSG+ADPCM-A+ADPCM-B)の内訳を`buildPhysicalChipList()`のメインループ(`config_->getDeviceCount()`)で記録する(stereoPairPort/spanGroups経由で束ねられる物理チップは、`devices_[i]`が`CLinearPanDevice`/`CSpanDevice`にラップされ`getChCount()`が複数物理チップ分を合算してしまうため対象外、既知の制限)。`CFITOM::getPhysicalChipChannelStates()`で全チャンネル分の現在の発音状態(`PhysicalChipChannelState{sounding,velocity}`)を`subDevices`の並び順で取得できる。チャンネル名は`CFITOM::getSubDeviceChannelPrefix(deviceType)`(新設の`kChannelPrefixMap`、OPN系→"FM"、OPL3→"4OP"、OPL3_2→"2OP"、ADPCM-A→"PA"、ADPCM-B→"PB"、SSG→"SSG"、OPL4AWM→"AWM"等)を「接頭辞+(ch+1)」で組み立てる想定(呼び出し側で組み立てる、コア側は接頭辞のみ提供)。表示順(サブデバイスの並び)は現状Config側の生成順のままで、ユーザー例示の並び(FM→PA→PB→SSG)とは一致しない場合があり、GUI実装時に表示用の並べ替えが必要になる見込み。**物理/論理表示切替(2026年7月追加)**: チャンネルレベルメーターのみ対象(レジスタダンプは実バイト値表示のため常に物理ポート単位のまま)。`CFITOM::getLogicalDeviceChannelStates(deviceIndex)`を新設し、`getPhysicalChipChannelStates()`(同一物理ポートを共有する全サブデバイスをまとめた状態)とは別に、`devices[]`の1エントリ(=1論理デバイス)単独のチャンネル状態を返せるようにした。論理チップ側の一覧・ラベルは既存の`getDevices()`(`FITOMDeviceInfo`)とチャンネル名は共通の`getSubDeviceChannelPrefix()`をそのまま流用でき、新規データモデルの追加は不要だった | ✅(コア層のみ) | — |
| PCMバンク複数併用対応・パッチピッカー用named patch自動合成(2026年7月新設、ステージング環境で「ADPCM-Aのパッチピッカーが常に空、試聴すると発音解決失敗」と報告され発覚)。`banks.pcm_banks[]`に任意の`group`(ADPCMB/ADPCMA/PCMD8)を追加。従来`CFITOM::initDevices()`が全PCMデバイスへ`setPcmRegistry(reg, 0)`とバンク番号0を決め打ちしていたため、2つ目以降の`pcm_banks`エントリ(コーデックの異なるADPCM-A用バンク等)が常に無視される不具合があった。`group`指定時はそのVoicePatchTypeに一致するPCMデバイスへ対応バンク番号を自動的に割り当てる(`PcmBankRegistry::findBankNoForVoicePatchType()`、未指定時はbank0固定の旧動作のまま後方互換)。また`group`指定時、`PatchManager::loadPcmBankJson()`が`entries[]`(adpcm_packer出力由来、各エントリの`root_note`も反映)の各サンプルからnamed patchを自動合成するため、`*.samplezonebank.json`を別途手書きしなくても、そのままパッチピッカー(CC#0=ADPCM-B/ADPCM-A/PCMD8)から個々のサンプルを選択・試聴できる。**ADPCM-B/ADPCM-A/PCMD8はHwPatchではなくAWMと同じ`SampleZonePatch`スキーマ(`isSampleBasedVoicePatchType()`)を使う設計のため、自動合成先は`HwBankRegistry`ではなく`SampleZoneBankRegistry`である点に注意**(実装時に一度`HwBankRegistry`側へ誤って合成し、パッチピッカーの表示は直るが実際のNoteOn解決が失敗する`SampleZonePatch not found`という不具合を作り込みかけた)。この修正に合わせ、GUI側(`FITOMBridge::getHwBankList()`/`getHwBankPatches()`、MIDIモニターのバンク/パッチ名解決)も`isSampleBasedVoicePatchType()`で分岐し`sampleRegistry()`を参照するよう修正(以前はサンプルベース音源系のバンク一覧が常に空になっていた、AWMも同様の恩恵を受ける) | ✅ | `patch-structure-design.md`, `hwpatch-reference.md` |
| ADPCM-A/ADPCM-B/PCMD8の波形メモリ配置の責務誤り修正、および`CMultiDevice`(CSpanDevice/CUnison)のPCM/波形レジストリ未伝播バグ修正(2026年7月、「ADPCM-A/Bともに正常に発音していない。キーオンフラグは操作されているが波形アドレスが正しく設定されていない」との報告で発覚)。調査は2段階だった:(1) `CAdPcmBase::loadVoice()`(`CYmDelta`/`CAdPcm2610A`/`CAdPcmZ280`共通のパターン)が波形バイナリ(adpcm_packer出力の.bin)をチップのPCM RAM相当レジスタへFITOM_X側から逐次書き込む実装になっていたこと自体が設計上の誤りだったため、`loadVoice()`を`registerVoice()`(voices_[WS番号]テーブルへオフセット/サイズを登録するだけ、レジスタ書き込みなし)に置き換え、実チップへのPCMバイナリ転送処理を全廃した(波形データの配置はhwif側の責務、FITOM_X本体はStart/Endアドレスレジスタを設定するだけでよいという設計に統一)。(2) しかしこれでも症状が解消せず、ユーザーが`CAdPcm2610A::updateVoice()`にデバッグログを仕込んで調査した結果、`voices_[]`テーブル自体が一度も登録されていないことが判明。**真因は`ISoundDevice::setPcmRegistry()`/`initPcmData()`がデフォルトno-op実装であるのに対し、複数チップを束ねる`CMultiDevice`(`CSpanDevice`/`CUnison`)がこれらをオーバーライドしておらず、束ねられた実チップ(サブチップ)へ一切伝播していなかったこと**。ADPCM-Aのように2枚の物理OPNBチップに跨って「同種デバイス自動束ね」(spanGroups、VoicePatchType基準)される構成では、`CFITOM::initDevices()`が`dev->setPcmRegistry(...)`/`dev->initPcmData()`を呼ぶ`dev`は束ね役の`CSpanDevice`自身であり、それが基底のno-opで止まってしまうため、実際に発音する`CAdPcm2610A`インスタンス側の`voices_`が空のまま(Start/Endアドレス=0)だった。`CMultiDevice`に`setWaveRegistry()`/`setPcmRegistry()`/`initPcmData()`を追加し、`chips_`の全サブチップへブロードキャストするよう修正(SCCの`setWaveRegistry()`も同じ未伝播バグを抱えていたため合わせて修正)。3チップドライバ共通のロジックのため、ADPCM-B/ADPCM-A/PCMD8すべてに同じ修正を適用。`core/include/fitom/PcmBankData.h`冒頭のワークフロー説明コメントも合わせて修正 | ✅ | — |
| ADPCM-A(YM2610/2610B)・ADPCM-B(YM2608=OPNA)のポートアドレス誤り修正(2026年7月、上記の一連のADPCM修正を経てもなお発音せず、ユーザーが「OPNA/OPNB/OPNBBのADPCMA/ADPCMBポートアドレスが間違っている、本来port2側[アドレス0x100以降]に書くべき値がport1側になっている」と指摘し発覚)。`Config::resolveCompositeSpec()`が生成する`DEVICE_ADPCMB_OPNA`/`DEVICE_ADPCMA`のサブデバイスは`usesExtraPort=false`のままだったため、`DeviceFactory::create()`に渡る`port`は常にSSG/FM共通と同じport1(低位、アドレス0x000-0x0FF)だった。だが実チップ上、ADPCM-A(YM2610/2610B、レジスタ範囲がSSGの0x00-0x0Dや`kOPNB_DeltaT`の0x10-0x1cと衝突する)・ADPCM-B(YM2608=OPNAのみ、一部のBRDY割り込み等の制御ビットはport1側の`COPNA::init()`が個別に設定するが、Start/End/Delta-N/Volume等の主要レジスタ群はport2側)は、実際にはport2(高位、アドレス0x100以降)に配置されるレジスタ体系である(ADPCM-B[YM2610/2610B側、`kOPNB_DeltaT`]は逆にport1のままで正しく対象外)。`Config::resolveCompositeSpec()`の該当`SubDeviceSpec`を`usesExtraPort=true`に変更し、`CFITOM`に`resolveAdpcmHighPort()`(`offsetPorts_`で寿命管理する`OffsetPort(port,0x100)`を、プロファイルで明示的な2スロットHW構成[`extra_port`]が無い場合に自前生成する)を新設して`initDevices()`のメインループ・spanGroupサブチップ生成ループ双方に適用した。`OPN2_new.cpp`の`COPNB`クラスコメント(「YM2610無印はADPCM-B用メモリ空間を持たないため生成しない」)も、上の`resolveCompositeSpec()`修正時に追従できていなかった古い記述だったため合わせて訂正 | ✅ | `chip-driver-architecture.md` |

---

## 既知の未対応・将来課題

- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal（FM音源に対応するパラメータがないため意図的に非対応）
- RPN 0x0002 Coarse Tuning、RPN 0x7F7F Null
- CC#2/CC#4 の変数分離
- VoicePatchType 未実装チップ (MA3系列, SAA1099, AWM) のドライバ実装
- OPL/OPL2/OPL3自体のリズムモード対応（現状OPLL系のみ対応。COPL_new.cppにリズム関連コードなし）
- VoicePatchType 完全一致以外へのフォールバック（旧FITOMの互換リスト相当、将来実装予定）
- GUI (Dear ImGui) 実装の残り(`apps/fitom_gui`。MIDIモニターバンドは実装済み。デバイス一覧・パッチ一覧・音色エディタ等、他画面への導線が未着手)
- 外部パッチエディタ起動機能(ダブルクリック→キオスクモード起動)は実装済みだが、実機での動作確認(実際にダブルクリックしてパッチエディタが開くこと)は未実施。発音履歴(ChState/ノートオン)には依存せず、`CInstCh::progChange()`と同じ`PatchManager::resolve()`/`resolveDirect()`をチャンネルの現在のCC#0/#32/プログラムチェンジ値でその場で再実行する設計にしたため、ノートオン自体は不要になったが、それでも`PatchManager::resolveTriple()`がVoiceGroup照合のために`devices[]`から一致するデバイスを線形探索するため、プロファイルの`devices[]`に対応するHWプラグインDLLが実際にロードできていないと(`Config::buildDevice()`がHWプラグイン未登録時に早期returnし`DeviceEntry`自体を作らないため)解決に失敗する。HWプラグインDLL(FitomEmuIF等)が用意できない開発環境ではこの経路を検証できない(コード上の妥当性は`PatchManager::jsonToHwPatch()`/`resolve()`/`resolveDirect()`等のソース確認で担保)
- GUI MIDIパイプ経由の音色試聴連携(`fitom_midi_pipe`側は実装済みだが、GUI側からのSysEx送出・パッチエディタ本体との結合は未着手)
- OPZ の2系統LFOリソース対応（旧FITOMも未完成のため現状維持）
- CAdPcmZ280 (YMZ280B/PCMD8) の旧FITOM実装との詳細突き合わせ未完了
- レジスタダンプモニター(`RegisterDumpWindow`)の基本動作(発光エフェクト・オルタネート表示・物理チップ名表示・高位ポート表示含む)はユーザーによる実機/実プラグイン接続環境での目視確認とフィードバックを経て、単一物理ポートで高位アドレス(0x100以降)を使うチップ(OPNA/OPN2/OPL3等)の高位側が表示されない不具合(`PhysicalChipInfo::dumpSize`新設で修正)、チップごとのテーブルに常にスクロールバーが出る不具合(テーブル高さ固定+ScrollYをやめ自動サイズ化で修正)の2件を修正済み。後者の修正自体は、この開発環境にHWプラグインDLL・物理チップが無くビルド確認までしかできていないため、ユーザーによる目視再確認が未実施
- チャンネルレベルメーターのBridge API・GUI新規実装(コア層の`PhysicalChipSubDevice`/`CFITOM::getPhysicalChipChannelStates()`/`getLogicalDeviceChannelStates()`/`getSubDeviceChannelPrefix()`は実装済み。`FITOMBridge`側の公開APIと、`apps/fitom_gui`側の2ペイン化[左:チップごとのチャンネルバー、右:既存のレジスタダンプ]・バー描画・12本ごとの折り返しレイアウト・物理/論理表示切替UIは未着手)。またサブデバイスの表示順(現状Config側の生成順)をユーザー例示の並び(例: OPNBならFM→PA→PB→SSGの順)に揃えるための並べ替えもGUI実装時に必要

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
