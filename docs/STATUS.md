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
| `docs/manuals/layered-patch-reference.md` | ✅ |

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
| チャンネルレベルメーターのコア層基盤(2026年7月新設。Bridge API・GUIは未着手、下記「既知の未対応」参照)。レジスタダンプモニターを左右2ペイン化し、左に「チップごとの物理チャンネル1本ずつのバー(ベロシティ/TL連動の疑似メーター。FITOM_Xは音声合成を行わないため実音量信号は存在せず、`ChState::isActive()`+`velocity`による疑似表示である点に注意)」、右に既存のレジスタダンプを表示する計画の土台。`PhysicalChipSubDevice`(`device`/`deviceType`/`chCount`)を新設し、`PhysicalChipInfo::subDevices`として物理チップ1個を構成する論理`ISoundDevice`群(例: OPNBならFM+SSG+ADPCM-A+ADPCM-B)の内訳を`buildPhysicalChipList()`のメインループ(`config_->getDeviceCount()`)で記録する(stereoPairPort/spanGroups経由で束ねられる物理チップは、`devices_[i]`が`CLinearPanDevice`/`CSpanDevice`にラップされ`getChCount()`が複数物理チップ分を合算してしまうため対象外、既知の制限)。`CFITOM::getPhysicalChipChannelStates()`で全チャンネル分の現在の発音状態(`PhysicalChipChannelState{sounding,velocity}`)を`subDevices`の並び順で取得できる。チャンネル名は`CFITOM::getSubDeviceChannelPrefix(deviceType)`(新設の`kChannelPrefixMap`、OPN系→"FM"、OPL3→"4OP"、OPL3_2→"2OP"、ADPCM-A→"PA"、ADPCM-B→"PB"、SSG→"SSG"、OPL4AWM→"AWM"等)を「接頭辞+(ch+1)」で組み立てる想定(呼び出し側で組み立てる、コア側は接頭辞のみ提供)。表示順(サブデバイスの並び)は現状Config側の生成順のままで、ユーザー例示の並び(FM→PA→PB→SSG)とは一致しない場合があり、GUI実装時に表示用の並べ替えが必要になる見込み。**物理/論理表示切替(2026年7月追加)**: チャンネルレベルメーターのみ対象(レジスタダンプは実バイト値表示のため常に物理ポート単位のまま)。`CFITOM::getLogicalDeviceChannelStates(deviceIndex)`を新設し、`getPhysicalChipChannelStates()`(同一物理ポートを共有する全サブデバイスをまとめた状態)とは別に、`devices[]`の1エントリ(=1論理デバイス)単独のチャンネル状態を返せるようにした。論理チップ側の一覧・ラベルは既存の`getDevices()`(`FITOMDeviceInfo`)とチャンネル名は共通の`getSubDeviceChannelPrefix()`をそのまま流用でき、新規データモデルの追加は不要だった。**Bridge API・GUI(2026年7月追加)**: `FITOMBridge`に`FITOMLevelChannel{name,sounding,velocity}`/`FITOMLevelBand{label,channels}`と`getPhysicalLevelBands()`/`getLogicalLevelBands()`を新設(チャンネル名はBridge側で「接頭辞+(ch+1)」を組み立て済みの状態で返す)。`apps/fitom_gui`に`LevelMeterPanel`を新設し、`RegisterDumpWindow`(旧・単独ウィンドウ)と共に左右2ペイン化(`ImGui::BeginChild`2枠、左42%/右残り、各々独立スクロール)。左ペインの`LevelMeterPanel`は「物理」「論理」ボタンで`getPhysicalLevelBands()`/`getLogicalLevelBands()`を切り替え、バンドごとに`ImGui::SeparatorText`+チャンネル数分の縦バー(`ImDrawList`直接描画、発音中はベロシティに応じた緑塗り、バー下部にチャンネル名)を12本折り返しで表示する | ✅ | — |
| チャンネルレベルメーターのカラーLED風表示化・ノートオン取りこぼし修正(2026年7月、ソリッドバーからの見た目改善要望+実機確認で「ノートオンをかなり取りこぼしているように見える」と報告され発覚)。**表示**: `LevelMeterPanel`のバー描画をソリッド塗りつぶしから14分割セグメント(下から緑8/黄3/赤3ゾーン、消灯セグメントも各ゾーン色の暗いバリエーションで塗る)のLED風表示に変更。バーのレベルはノートオン/オフに連動する疑似減衰エンベロープ(GUI側`ChannelEnvelope`、チャンネルごとに`unordered_map`で保持)で駆動し、ノートオンでベロシティ由来のピークへ即座に立ち上がり1.5秒で全減衰、ノートオフ時点でその時のレベルから250msで全減衰する。ノートオン時はピークホールド(ピーク位置のセグメントを白側にブレンドして際立たせる)を重ね、1.5秒で全減衰・ノートオフで即時消滅させる(`main.cpp`の鍵盤ビュー発光エフェクトと同じ「経過時間からのフェード計算」方式)。**ノートオン取りこぼし修正**: 当初はノートオンのエッジ検出を`FITOMLevelChannel::sounding`のfalse→true遷移または`velocity`変化で行っていたが、`sounding`/`velocity`はブリッジが返す現在値のスナップショットに過ぎないため、ボイススチールによる同一物理チャンネル上の連続ノートオン(同一ベロシティでの再トリガー、`sounding`はtrueのまま・`velocity`も不変)を検出できず取りこぼしていた。真因の解決には現在値スナップショットの比較では原理的に不十分なため、`ChState`にノートオンのたびに単調増加する`noteOnSeq`(`uint32_t`)を新設し、`CSoundDevice::noteOn()`でインクリメント、`PhysicalChipChannelState`→`FITOMLevelChannel`まで伝播させ、GUI側のノートオン検出を`noteOnSeq`の変化(フレーム間差分)で行うよう変更した(ノートオフの検出は従来通り`sounding`のtrue→false遷移のまま)。この修正に合わせ、従来`enabled`の伝播が抜けていた`CFITOM::getLogicalDeviceChannelStates()`(物理側の`getPhysicalChipChannelStates()`には既にあった)も揃えて修正した | ✅ | — |
| PCMバンク複数併用対応・パッチピッカー用named patch自動合成(2026年7月新設、ステージング環境で「ADPCM-Aのパッチピッカーが常に空、試聴すると発音解決失敗」と報告され発覚)。`banks.pcm_banks[]`に任意の`group`(ADPCMB/ADPCMA/PCMD8)を追加。従来`CFITOM::initDevices()`が全PCMデバイスへ`setPcmRegistry(reg, 0)`とバンク番号0を決め打ちしていたため、2つ目以降の`pcm_banks`エントリ(コーデックの異なるADPCM-A用バンク等)が常に無視される不具合があった。`group`指定時はそのVoicePatchTypeに一致するPCMデバイスへ対応バンク番号を自動的に割り当てる(`PcmBankRegistry::findBankNoForVoicePatchType()`、未指定時はbank0固定の旧動作のまま後方互換)。また`group`指定時、`PatchManager::loadPcmBankJson()`が`entries[]`(adpcm_packer出力由来、各エントリの`root_note`も反映)の各サンプルからnamed patchを自動合成するため、`*.samplezonebank.json`を別途手書きしなくても、そのままパッチピッカー(CC#0=ADPCM-B/ADPCM-A/PCMD8)から個々のサンプルを選択・試聴できる。**ADPCM-B/ADPCM-A/PCMD8はHwPatchではなくAWMと同じ`SampleZonePatch`スキーマ(`isSampleBasedVoicePatchType()`)を使う設計のため、自動合成先は`HwBankRegistry`ではなく`SampleZoneBankRegistry`である点に注意**(実装時に一度`HwBankRegistry`側へ誤って合成し、パッチピッカーの表示は直るが実際のNoteOn解決が失敗する`SampleZonePatch not found`という不具合を作り込みかけた)。この修正に合わせ、GUI側(`FITOMBridge::getHwBankList()`/`getHwBankPatches()`、MIDIモニターのバンク/パッチ名解決)も`isSampleBasedVoicePatchType()`で分岐し`sampleRegistry()`を参照するよう修正(以前はサンプルベース音源系のバンク一覧が常に空になっていた、AWMも同様の恩恵を受ける) | ✅ | `patch-structure-design.md`, `hwpatch-reference.md` |
| ADPCM-A/ADPCM-B/PCMD8の波形メモリ配置の責務誤り修正、および`CMultiDevice`(CSpanDevice/CUnison)のPCM/波形レジストリ未伝播バグ修正(2026年7月、「ADPCM-A/Bともに正常に発音していない。キーオンフラグは操作されているが波形アドレスが正しく設定されていない」との報告で発覚)。調査は2段階だった:(1) `CAdPcmBase::loadVoice()`(`CYmDelta`/`CAdPcm2610A`/`CAdPcmZ280`共通のパターン)が波形バイナリ(adpcm_packer出力の.bin)をチップのPCM RAM相当レジスタへFITOM_X側から逐次書き込む実装になっていたこと自体が設計上の誤りだったため、`loadVoice()`を`registerVoice()`(voices_[WS番号]テーブルへオフセット/サイズを登録するだけ、レジスタ書き込みなし)に置き換え、実チップへのPCMバイナリ転送処理を全廃した(波形データの配置はhwif側の責務、FITOM_X本体はStart/Endアドレスレジスタを設定するだけでよいという設計に統一)。(2) しかしこれでも症状が解消せず、ユーザーが`CAdPcm2610A::updateVoice()`にデバッグログを仕込んで調査した結果、`voices_[]`テーブル自体が一度も登録されていないことが判明。**真因は`ISoundDevice::setPcmRegistry()`/`initPcmData()`がデフォルトno-op実装であるのに対し、複数チップを束ねる`CMultiDevice`(`CSpanDevice`/`CUnison`)がこれらをオーバーライドしておらず、束ねられた実チップ(サブチップ)へ一切伝播していなかったこと**。ADPCM-Aのように2枚の物理OPNBチップに跨って「同種デバイス自動束ね」(spanGroups、VoicePatchType基準)される構成では、`CFITOM::initDevices()`が`dev->setPcmRegistry(...)`/`dev->initPcmData()`を呼ぶ`dev`は束ね役の`CSpanDevice`自身であり、それが基底のno-opで止まってしまうため、実際に発音する`CAdPcm2610A`インスタンス側の`voices_`が空のまま(Start/Endアドレス=0)だった。`CMultiDevice`に`setWaveRegistry()`/`setPcmRegistry()`/`initPcmData()`を追加し、`chips_`の全サブチップへブロードキャストするよう修正(SCCの`setWaveRegistry()`も同じ未伝播バグを抱えていたため合わせて修正)。3チップドライバ共通のロジックのため、ADPCM-B/ADPCM-A/PCMD8すべてに同じ修正を適用。`core/include/fitom/PcmBankData.h`冒頭のワークフロー説明コメントも合わせて修正 | ✅ | — |
| ADPCM-A(YM2610/2610B)・ADPCM-B(YM2608=OPNA)のポートアドレス誤り修正(2026年7月、上記の一連のADPCM修正を経てもなお発音せず、ユーザーが「OPNA/OPNB/OPNBBのADPCMA/ADPCMBポートアドレスが間違っている、本来port2側[アドレス0x100以降]に書くべき値がport1側になっている」と指摘し発覚)。`Config::resolveCompositeSpec()`が生成する`DEVICE_ADPCMB_OPNA`/`DEVICE_ADPCMA`のサブデバイスは`usesExtraPort=false`のままだったため、`DeviceFactory::create()`に渡る`port`は常にSSG/FM共通と同じport1(低位、アドレス0x000-0x0FF)だった。だが実チップ上、ADPCM-A(YM2610/2610B、レジスタ範囲がSSGの0x00-0x0Dや`kOPNB_DeltaT`の0x10-0x1cと衝突する)・ADPCM-B(YM2608=OPNAのみ、一部のBRDY割り込み等の制御ビットはport1側の`COPNA::init()`が個別に設定するが、Start/End/Delta-N/Volume等の主要レジスタ群はport2側)は、実際にはport2(高位、アドレス0x100以降)に配置されるレジスタ体系である(ADPCM-B[YM2610/2610B側、`kOPNB_DeltaT`]は逆にport1のままで正しく対象外)。`Config::resolveCompositeSpec()`の該当`SubDeviceSpec`を`usesExtraPort=true`に変更し、`CFITOM`に`resolveAdpcmHighPort()`(`offsetPorts_`で寿命管理する`OffsetPort(port,0x100)`を、プロファイルで明示的な2スロットHW構成[`extra_port`]が無い場合に自前生成する)を新設して`initDevices()`のメインループ・spanGroupサブチップ生成ループ双方に適用した。`OPN2_new.cpp`の`COPNB`クラスコメント(「YM2610無印はADPCM-B用メモリ空間を持たないため生成しない」)も、上の`resolveCompositeSpec()`修正時に追従できていなかった古い記述だったため合わせて訂正 | ✅ | `chip-driver-architecture.md` |
| ADPCM-B(Delta-T)再生レート・アドレス境界・センターパン無音・マスタークロックの4件を修正(2026年7月、上記のポートアドレス修正後もADPCM-Bが正常に再生されず、実機ログでの調査により発覚)。(1) **Delta-N算出式**: `FnumUtils.h`の`FnumTableType::DeltaN`ケースが実チップの式`delta_n=round(2^16*freq*divide/master)`のうち`divide`(OPNA/OPNB=144)の乗算を丸ごと欠落させていた上、基準周波数に他の型と同じ`masterPitch_`(A440チューニング、既定440Hz)を使っていたが、DeltaNは`Fnum.cpp::CFnumTable::GetDeltaN`(旧FITOM、現在未使用)以来の固定16000Hz基準でなければならず、`CYmDelta`のkNoteOffset/kPitchOrigin定数もその16000Hz基準にキャリブレーションされたものだった。両者の欠落・不一致によりC4(MIDIノート60)のDeltaNが本来約25085相当のところ690程度にしかならず、再生速度が実際の約1/36というかなり遅い速度になっていた。`divide`の乗算を復元し、`freq*(16000/440)`で16000Hz基準に変換してから計算するよう修正。(2) **Start/Endアドレス境界**: `RegMap`に`addrShift`(バイトオフセットをレジスタ値へ変換する際の右シフト量、ymfmの`adpcm_b_channel::address_shift()`相当)を新設。旧実装は全チップに4byte境界(shift=2)を固定で使っていたが、OPNB/OPNBB(YM2610/2610B)は実チップの回路仕様上256byte境界(shift=8)固定であり(OPNA/Y8950は4byte境界で正しい)、OPNB/OPNBBの再生アドレスが実際の配置位置から64倍ズレていた。`AdpcmVoice::startAddr`/`length`の内部表現もビット単位からバイト単位に変更し、`registerVoice()`/`updateVoice()`双方で`addrShift`に基づき変換するよう統一。**(2026-07-24追記)** OPNA ADPCM-Bと同一パッチのはずのOPNB ADPCM-Bが同じ音にならず(ノイズになる)との報告を受け、原因切り分けのため`kOPNB_DeltaT`の`addrShift`値を8→2へ一時的に差し戻して検証したが改善しなかった。`YMEngine/extern/ymfm/src/ymfm_opn.cpp`のchip別コンストラクタ(`ym2608::ym2608()`は`m_adpcm_b(intf)`でaddrshift省略=0、`ym2610::ym2610()`は`m_adpcm_b(intf, 8)`で明示的に8)と`adpcm_b_channel::address_shift()`(`ymfm_adpcm.cpp`、コンストラクタ引数が非0なら無条件にその値を返す実装)を確認し、YM2610/2610Bは256byte境界(shift=8)固定で確定と裏取りできたため8に戻した(2の方が誤り)。8でも症状が残る場合、真因はaddrShift値ではなくadpcm_packer側の実際のデータ整列(pcmbank.jsonの`boundary`フィールドはFITOM_X側で検証に使われない情報欄[`config_schema/pcmbank.schema.json`]のため、実データが本当に256byte境界に整列されているとは限らない)を疑う必要がある。下記「既知の未対応」参照。(3) **センターパン無音**: ADPCM-Bのcontrol2レジスタのbit7/6(pan_left/pan_right)は、実チップ上ステレオ定位だけでなく「出力有効化ビット」を兼ねる(両ビット0だと演算結果が出力に一切加算されない)。`CSoundDevice::noteOn()`はpanDirty(前回値との差分)が立った場合のみ`updatePanpot()`を呼ぶ設計だが、panpotの既定値は0(center)であり、MIDI側が明示的にパンCCを送らない限り「0→0で変化なし」と判定され`updatePanpot()`が一度も呼ばれず、control2がリセット値(両ビットOFF)のまま放置されてセンターパンでは恒久的に無音になっていた。`CYmDelta`のNoteOn処理に、上位のdirtyフラグに関係なく`updatePanpot(ch)`を無条件呼び出しするよう追加(このチップのみ「毎ノートオンで確実に書く」必要があるための個別対応)。(4) **マスタークロック**: `createCAdPcm()`がFM系チップドライバ(`createCOPNA`/`createCOPNB`/`createCOPN2`等)と異なり、Delta-N算出のmasterに`port->getClock()`(実クロック)ではなくサンプルレート`sr`をそのまま渡していた。`port->getClock()`(取得不可時のみ`sr`にフォールバック)を使うよう統一。(1)の式は独立して修正が必要で、クロック取得の是非のみを先に直した際は逆にDeltaNが3〜4という壊滅的な値になり一度差し戻した経緯がある | ✅ | — |
| ADPCM-B(Delta-T)でOPNBが誤ってOPNA用PCMイメージのオフセットテーブルを参照していたバグを修正(2026年7月、上記(153行目)のaddrShift=8修正後もOPNBがノイズになり続けたため、実機のレジスタダンプ比較で発覚)。真因はaddrShiftではなく`PcmBankRegistry`側にあった。`CFITOM::initDevices()`は各PCMデバイスへ割り当てるバンク番号を`FITOMConfig::deviceTypeToVoicePatchType(deviceType)`経由のVoicePatchType一致のみで解決していたが、OPNA用ADPCM-B(`DEVICE_ADPCMB_OPNA`)とOPNB/OPNBB用ADPCM-B(`DEVICE_ADPCMB`)は音色パラメータ形式共有のため同一VoicePatchType(`VOICE_PATCH_ADPCMB`)を意図的に共有しており、`PcmBankRegistry::findBankNoForVoicePatchType()`は「最初に見つかった一致」しか返せないため、プロファイルに`group:"ADPCMB"`のpcm_banksエントリが1つ(OPNA用、32byte境界のオフセットテーブル)しか無い場合、OPNBのADPCM-Bデバイスにも同じOPNA用オフセットテーブルが誤って割り当てられていた(FitomEmuIF/YMEngine側の`pcm_image_catalog.json`はOPNB用に別キー`OPNB_ADPCM-B`で正しいバイナリ[256byte境界]を既にロードしていたため、実際のPCMメモリ内容とFITOM_X側が計算するStart/Endオフセットが完全に食い違っていた)。`PcmBank`に`deviceType`フィールド(`DEVICE_ADPCMB_OPNA`/`DEVICE_ADPCMB`/`DEVICE_ADPCMB_Y8950`等)、`PcmBankRegistry::findBankNoForDeviceType()`(deviceType完全一致、見つからなければ従来のVoicePatchType一致へフォールバック)を新設し、`CFITOM::initDevices()`はこちらを先に試すよう変更。profile.jsonの`pcm_banks[].chip`(任意、"OPNA"/"OPNB"/"OPNBB"/"Y8950")で物理チップ単位にバンクを指定できる(`Config.cpp`の`resolvePcmBankChipDeviceType()`が変換、`config_schema/profile.schema.json`にスキーマ追加)。entries[]のエントリ番号(WS/waveIndex)とサンプル名の対応は物理チップに依らず共通のため、HwBank/SampleZoneBank側の音色パッチ定義自体は引き続きOPNA/OPNBどちらのデバイスにも共通で使い回せる(chipを指定しても「束ね」=パッチ互換性は失われない、offsetテーブルという実装内部の詳細のみがチップ単位に分離される)。ただし`chip`指定時、entries[]からのnamed patch自動合成(`PatchManager::loadPcmBankJson()`)はバンクごとに独立して走るため、同一の名前集合が複数バンク番号の下でパッチピッカーに重複表示される点は既知のトレードオフ(実運用ではdrum_banks[]等の固定prog経由の参照が主でこの重複は表面化しにくい)。**(2026-07-24追記)** 上記修正後、実機ログ(`mergeSpannableDevices`)でOPNA/OPNB/OPNBBのADPCM-Bが`VoicePatchType=0x51`一致で1つの`CSpanDevice`(代表名"OPNA#2-ADPCMB"、3物理チップ・3ch)へ正しく束ねられていることは確認できたが、ユーザーから「束ねられていない(OPNB/OPNBB側が反映されない)」との報告があり再調査。原因は`CMultiDevice::setPcmRegistry()`(`MultiDevice.h`)が、代表デバイス(束ねの先頭、この場合OPNA)のdeviceTypeから解決された1つの`bankNo`を、束ねられた全サブチップ(OPNA/OPNB/OPNBBそれぞれの実`CYmDelta`インスタンス)へ無条件にブロードキャストしていたため、OPNB/OPNBBのサブチップにもOPNA用バンク(bank0)がそのまま伝播し、上記のdeviceType別バンク解決が束ね構成では無効化されていたことだった(単一チップ種のみの束ね[例: SSGを3枚束ねる等]では全サブチップが同じdeviceTypeのため問題化しなかった)。`CMultiDevice::setPcmRegistry()`を、各サブチップについてまず`c->getDeviceType()`(サブチップ自身の実際のdeviceType)で`findBankNoForDeviceType()`を個別に引き直し、見つかった場合はそちらを優先するよう修正(見つからない場合のみ代表デバイス基準の`bankNo`にフォールバック、単一チップ種の束ねとの後方互換を維持)。**(2026-07-24さらに追記)** ポリフォニック発音・束ねは正常に確認できたが、「同一の名前集合が複数バンク番号の下でパッチピッカーに重複表示される」という上記のトレードオフについて質問があったため、これも解消した。`PatchManager::loadPcmBankJson()`/`Config.cpp`のpcm_banksローダーに`offsetsOnly`引数/`pcm_banks[].offsets_only`(任意、既定false)を新設し、trueを指定したバンクはStart/Endオフセットテーブルとしてのみ登録され、named patch自動合成(sampleRegistry()への公開)をスキップするようにした。同一group内でchip違いのバンクを複数併用する場合、代表(通常はchip省略または最初のバンク)以外に`offsets_only: true`を指定すればパッチピッカーの重複表示を避けられる(`config_schema/profile.schema.json`にスキーマ追加) | ✅ | `chip-driver-architecture.md` |
| レジスタダンプモニターでOPNB(YM2610無印)/2610B(YM2610B)の高位ポート(0x100以降、ADPCM-A/ADPCM-B等が配置される)が表示されない不具合修正(2026年7月)。上記(147行目)の「port2が無くてもkDevMapの既知のレジスタ空間サイズが0x100超なら一括読み出し」という修正時に、`CFITOM.cpp`の`kDevMap`へ`DEVICE_OPNB`/`DEVICE_2610B`のエントリを追加し忘れていたため、`getDeviceRegSize()`が0を返し`PhysicalChipInfo::dumpSize`が既定の0x100のまま(0x000-0x0FFのみ)になっていたのが原因。両デバイスともCOPNA同様port2/OffsetPort経由で0x100-0x1FFを使うため、OPNA/OPN2と同じ`{VOICE_TYPE_FM4, VOICE_GROUP_OPNA, 0x200}`を追加した | ✅ | — |
| 外部パッチエディタ起動機能、実機での初回動作確認により発覚したプロファイルパス解決バグを修正(2026年7月)。`apps/fitom_gui/main.cpp`の`launchPatchEditorForChannel()`がパッチエディタ子プロセスへ渡す`profilePath`/`hwBankFile`は、プロファイルを相対パスで指定していた場合(`FITOMConfig::loadProfile()`のbaseDir=`path.parent_path()`も相対のまま伝播するため)fitom_gui自身のCWD基準の相対パス文字列のままだった。一方`launchProcess()`は子プロセスの作業ディレクトリをパッチエディタの実行ファイル自身のディレクトリに固定するため、fitom_gui起動時のCWDと異なる場所を基準に解決されてしまい「cannot open file」でプロファイル読み込みに失敗していた(パッチエディタ側は正しく動作しており、原因はFITOM_X側にあった)。子プロセスへ渡す直前に`fs::absolute()`で絶対パス化するよう修正 | ✅ | — |
| 内部用MIDIパイプ(`fitom_midi_pipe`)を、プロファイル設定(`midi_backend`/`midi_inputs`)から完全に独立させ、`-DFITOM_BUILD_BACKEND_MIDI_PIPE=ON`でビルドすれば常時・無条件で有効化されるよう変更(2026年7月、外部パッチエディタとの併用確認中に「実機MIDIキーボードとの同時併用ができず不便」との指摘で着手)。従来`FITOMBridge`/`fitom_cli`はMIDIバックエンドDLLをアプリ全体で1つしか同時保持できず、実機MIDIキーボード用のrtmidiバックエンドとfitom_midi_pipeが同一プロファイル内で併用できない制約があった(`IMidiPlugin.h`契約自体ではなく、アプリ層が「バックエンドDLLは1つだけ」と決め打ちしていたことに起因)。`CFITOM`に既存の`MAX_MPUS`(4)本のMPUとは完全に独立した内部パイプ専用の`MidiProcessor`+16ch(`internalPipeChannels_`/`internalPipeProcessor_`、`getInternalPipeProcessor()`)を新設し、`init()`で常に構築、`timerCallback`/`pollingCallback`/`midiClockCallback`/`allNoteOff`/`resetAllCtrl`/`setScaleTuning`の既存`MAX_MPUS`ループの直後にも組み込んだ。`FITOMBridge::initInternalMidiPipe()`(および`apps/fitom_cli`側の同等ロジック)が、プロファイルを一切参照せず`fitom_midi_pipe`のDLLファイル存在有無のみで無条件にロード・オープンする(存在しない場合は非致命的にスキップ)。`MAX_MPUS`/`getMpuCount()`自体は変更していないため、GUIのMIDIモニター・MIDIポート設定ダイアログ等、既存のMPU前提UIは無改修のまま(内部パイプは意図的にこれらに表示されない)。PowerShellの`NamedPipeClientStream`による疎通確認(チャンネル割り当て通知の受信→Program Change/Note On/Offの送信→ログでの反映確認)で、rtmidiバックエンドのロード失敗とは無関係に内部パイプが独立して機能することを確認済み | ✅ | `plugin-midi-pipe.md` |
| 内部用MIDIパイプを常時有効化した直後、実機で「パッチエディタを閉じた後FITOM_Xの終了操作がハングする」不具合が発覚し修正(2026年7月)。真因は`backends/midi_pipe/src/MidiPipe.cpp`の`~MidiInDevice()`(パイプの後始末処理、以前からの既存コード)が、`ConnectNamedPipe()`でブロック中の`acceptThread`を起こす手段として別スレッドからの`CloseHandle()`のみに頼っていたこと。Windowsではブロッキングモードのハンドルに対し、同期I/O呼び出し中のスレッドとは別のスレッドが`CloseHandle()`しても、その呼び出しが確実に解除される保証がない(既知の制約)。パイプが常時有効化されたことで、パッチエディタ未接続時でも起動直後からacceptThreadが常時この待機状態になり、終了時に高確率でハングするようになって初めて表面化した(以前はプロファイル側で明示的に有効化しない限りこの経路自体が実行されなかったため潜在化していた)。`CancelSynchronousIo()`(スレッド単位で保留中の同期I/Oをキャンセルできる、Vista以降のAPI)を`CloseHandle()`の前に追加し、acceptThread・各ワーカースレッド(パッチエディタ接続が生きたまま終了処理に入った場合の`ReadFile()`待ち)双方に適用するよう修正。`tests/test_midi_pipe.cpp`(`FITOM_BUILD_BACKEND_MIDI_PIPE=ON`時のみビルド、それ以外は既存テストへの影響ゼロ)を新設し、パッチエディタ未接続のままポートを閉じてもハングしないことを別スレッド+タイムアウト付きfutureで検証する回帰テストとして追加(修正前のコードに対して意図的に実行し、5秒でタイムアウト・テスト失敗することを確認したうえで、修正適用後にPASSすることも確認済み) | ✅ | `plugin-midi-pipe.md` |
| 外部パッチエディタ(`FITOM_patch_editor`)側の起動引数仕様変更(D-039/D-040、`../FITOM_patch_editor/docs/DESIGN.md`参照)に追従(2026年7月)。パッチエディタがhwpatch(デバイス)編集画面専用の3引数(`<profile> <hwbank-file> <prog>`)から、種別を挟んだ4引数(`<profile> <kind> <bank-file> <prog>`、`kind`="device"/"layered"等)へ変更されたため、FITOM_X側もチャンネルの現在のパッチ種別に応じて適切な`kind`で起動するよう追従した。`FITOMBridge::resolveChannelHwPatch()`のシグネチャに`outKind`を追加し、通常モード(bankSelMSB==0、レイヤードパッチ/PatchBank選択)では`kind="layered"`+`*.patchbank.json`+レイヤードPatchのprog番号を、直接モード(bankSelMSB!=0)では従来通り`kind="device"`+`*.hwbank.json`+HW prog番号を返す。以前は通常モードでも`pm.resolve()`で先頭ToneLayerのHwPatchまで解決してしまい「レイヤードパッチ経由だった」という情報が呼び出し元に渡る前に失われ、常にデバイスパッチ編集画面が開いてしまう不具合があったが(パッチエディタ側の調査で発覚、D-039の`FITOM_X本体側`部分)、通常モードでは`pm.resolve()`を呼ばず`pm.findPatchBank(bankNoLsb)`のfilenameとprogNoをそのまま返すよう変更したことで解消した。`apps/fitom_gui/main.cpp`の`launchPatchEditorForChannel()`も4引数の起動コマンドラインを組み立てるよう追従。performance/drum種別は「チャンネルの現在のパッチ」になることが無いため(パフォーマンスパッチは常にDevice/Layeredから参照される側、ドラムチャンネルは`isRhythm()`で従来通り除外)、今回のFITOM_X側の対応範囲には含まれない | ✅ | — |
| `VoiceProcessor::onVolumeChange()`未使用バグの修正(2026年7月)。発音中のCC7(Volume)/CC11(Expression)変更が`effectiveTL()`経由の音量計算へ反映されない問題(下記「既知の未対応・将来課題」に記載していたもの)を修正。原因は`onVolumeChange()`自体がどこからも呼ばれていなかったこと。`CSoundDevice::setVolume()`/`setExpression()`が値変更を検出した時点で`ChState::proc.onVolumeChange()`を呼ぶよう変更した。`onVolumeChange()`はTL計算に`voice.swOp[].VTL`(ベロシティ/エクスプレッション感度)を必要とするが、従来`assignCh()`時にのみ一時的な`FmVoice`を組み立てて`onNoteOn()`へ渡すだけで、SwPatch自体はどこにも保持していなかった。そのため`ChState`に`swPatch`(値コピー)を新設して`assignCh()`でキャッシュし、`CSoundDevice::buildVoiceForCh()`(新設ヘルパー、hwPatch/samplePatch/swPatchキャッシュから`FmVoice`を再構築)経由で`assignCh()`/`setVolume()`/`setExpression()`が共通利用するよう整理した。副次効果として、ボイススチールでチャンネルが再利用された際、`assignCh()`内の`onNoteOn()`が新ノートの前に残っていた旧CC7/CC11値で最初のTLを計算してしまい、直後の`setVolume()`/`setExpression()`(旧実装ではdirtyフラグを立てるだけ)がそれを訂正できていなかった、新規ノート発音直後の初期音量が誤る不具合も併せて修正された(CC変更が無い通常の連続ノートオンでは元々問題なし) | ✅ | — |
| pcmbank/samplezonebankへのパフォーマンスパッチ(SwPatch)紐づけ(2026年7月新設)。`SampleZone`に`swBank`/`swProg`(`HwPatch`と同じ規約)を追加し、`*.samplezonebank.json`の`zones[].sw_bank/sw_prog`、`*.pcmbank.json`の新設`swpatches[]`(`entries[]`とentry_noで対応)の2経路で指定できる。`DrumNote::swBank/swProg`による上書きも既存のHwPatchと同じ優先順位で効く。解決はNoteOn時点で`SampleZonePatch::resolveZone(note, vel)`(COPL4AWM/CAdPcmBaseに重複していたゾーン線形探索をここへ統一)により実際に一致するゾーンを求めてから行う。**実装過程で2件の既存バグを発見・修正**: (1) ゾーン探索(`updateVoice()`)が`assignCh()`内部で`s.lastNote`が正しいノートに更新される前(`setNoteFine()`は`assignCh()`より後に呼ばれる)に走ってしまい、常にチャンネル再利用時の1つ前のノート(初回は`0xFF`)でゾーンが決まっていた潜在バグ(`setNoteFine()`が`update`かつ`s.samplePatch`ありの場合`updateVoice()`を呼び直すよう修正、4チップ共通)。(2) `CSoundDevice::assignCh()`の`samplePatch`分岐が`VoiceProcessor::onNoteOn()`を一切呼んでいなかったため、音量計算が`effectiveTL(0)`のみに依存する設計のCYmDelta(ADPCM-B)/CAdPcmZ280(PCMD8)は、MIDI Volume/Expression/Velocityを無視して常に最大音量で再生されていた(本機能とは無関係の既存バグ)。`CSoundDevice`に`usesVoiceProcessorForSamplePatch()`/`sampleVoiceProcessorVolume()`の2仮想関数を新設し、該当チップのみ`onNoteOn()`を呼ぶよう修正、`updateTL()`(従来no-op)も`updateVolExp()`を呼ぶよう変更してトレモロLFOの継続反映にも対応した。CAdPcm2610A(ADPCM-A)はピッチ制御が無くビブラート/fine_transposeが原理的に不可能なため対象外としつつ、実機のVol/Exp-Vel分離設計(vol=総合音量レジスタ、exp×vel=チャンネルレジスタ)を活かして`onNoteOn()`へvol=127(中立値)を渡すことで、ベロシティ感度(VTL)/トレモロのみ安全に追加した(総合音量レジスタ・複数MIDI CH間の競合という既存の制約には触れていない)。COPL4AWM(AWM)は実機がLFO/VIBレジスタをデバイス機能として持ち波形バイナリ側の設定と整合させる設計が別途必要なため、意図的に対象外のまま(`onNoteOn`を呼ばない)とした | ✅ | `patch-structure-design.md`, `voice-parameter-reference.md` |
| SF2直行パス(fluidsynth統合、`docs/sf2-fluidsynth-integration.md`参照)のFITOM_X本体側実装(2026年7月新設)。新設`Sf2BankRegistry`(`core/include(src)/fitom/Sf2BankRegistry.{h,cpp}`)が`banks.sf2_banks[]`を1回のパースからCC#32解決用マップと`devices[]`へ渡す`soundfonts`一覧(重複除去・初出順)の両方を導出する。`FITOMConfig::buildFromProfile()`はdevices[]構築より前にこれを構築し、`chip:"SF2"`の`devices[]`エントリのparams_jsonへ`soundfonts`を自動注入する(`resolveChipDeviceId("SF2")`は`DEVICE_NONE`へ明示的に解決し、既存の「未知チップ」警告を出さない)。トップレベル`sf2_channel_windows[]`も読み込み・検証(fluidsynth_chan/(mpu,ch)の重複、範囲外値、16エントリ超過を起動時エラーに)する。プロファイル記述レベルで`chip=="SF2"`が複数ある場合・`sf2_banks`/`sf2_channel_windows`が設定されているのに対応する`devices[]`が無い場合も起動時エラーとする。`CFITOM::initDevices()`は`chip:"SF2"`の`DEVICE_NONE`スキップを他の「未知チップ」skipと区別し(警告を出さず)、そのIPortを`sf2Port_`として記録する。`MidiProcessor`に`mpuIndex_`を持たせ、`processMessage()`が窓(MPU×ch)に含まれるメッセージを`CInstCh`/`CRhythmCh`へのディスパッチより前に`CFITOM::routeSf2ChannelMessage()`へ振り分ける(CC#0破棄、CC#32→`sf2_banks`解決のキャッシュ、有効な解決が無い間はプログラムチェンジも読み捨て、解決済みならプログラムチェンジ受信時にプライベートSysEx sub-cmd 0x05(`F0 00 48 01 05 <chan> <soundfont_index> <bank_msb> <bank_lsb> <prog> F7`)を`HWPlugin_WriteBlock`経由で送出、それ以外の通常メッセージはchanを付け替えてそのまま転送)。プライベートSysEx sub-cmd 0x04(`F0 00 48 01 04 <mpu> <ch> <fluidsynth_chan\|0x7F> F7`)による窓の動的割り当て・解除(`CFITOM::setSf2ChannelWindow()`、fluidsynth_chanの重複割り当ては要求ごと拒否)も実装。マスターボリューム/マスターピッチの変更(`CFITOM::setMasterVolume()`/`setMasterPitch()`)は、新規プロトコルを設計せず既存のGM2 Universal Realtime SysEx(マスターボリューム`04 01`・マスターファインチューニング`04 03`)をそのままSF2デバイスへ転送する。`tests/test_config.cpp`に`Sf2BankRegistry`の解決・重複除去・不正エントリ、`sf2_banks`/`sf2_channel_windows`の各種バリデーション(起動時エラーになるケース含む)のユニットテストを追加。**このリポジトリのスコープ外(未着手)**: `fluid_synth_t`を実際にリンクするhwifプラグイン本体(仮称`FitomSf2IF`、別リポジトリ)、`sf2_channel_windows`編集用のGUI専用ダイアログ、MIDIモニターのDevice/Fnumber列でのSF2直行パス表示。`FitomSf2IF`が存在しない現状は`sf2Port_`が常に`nullptr`のため、窓に含まれるメッセージは単純に読み捨てられる(実害はないが実際の発音確認は`FitomSf2IF`の実装後になる) | ✅ (FITOM_X本体側) | `sf2-fluidsynth-integration.md`, `manuals/midi-message-reference.md` |
| ADPCM-A/ADPCM-BでExpression/Velocityの極性が反転していたバグを修正(2026年7月、上記pcmbank/samplezonebankへのSwPatch紐づけをユーザーが実機確認した結果、報告され発覚)。`VoiceProcessor::effectiveTL()`はラウドネス空間(0=無音,127=最大音量、`OPN_new.cpp`の`effTLToReg()`コメント参照)を返す設計だが、`CAdPcm2610A`(ADPCM-A)/`CYmDelta`(ADPCM-B)の`updateVolExp()`が、この値を`127-effectiveTL(0)`で不要に反転させてから使っていたため、Expression/Velocityの効きが逆転していた。ADPCM-Aは元々`calcVolExpVel()`の戻り値(同じラウドネス空間の規約)をそのまま渡していたところへ、上記コミットで`effectiveTL()`に置き換えた際に反転を誤って追加してしまっていた。ADPCM-Bは同コミットでは一切変更していない既存コードだが、同種の反転が元から入っており、`onNoteOn()`を呼ぶよう修正したことで`effectiveTL()`が実際に機能するようになり、初めて症状として顕在化した。両者とも反転を除去し、`effectiveTL()`を`calcVolExpVel()`と同じラウドネス空間の値としてそのまま使うよう修正 | ✅ | — |
| CC#1(Modulation)ソフトウェアLFOが、それを一度も送っていない別のMIDIチャンネルの発音に誤って掛かることがある不具合を修正(2026年7月、「CC#1でソフトウェアピッチLFOを使用した後、まったく別のCHで発音した音にLFOがかかることがある」との報告で発覚)。原因はデバイスチャンネル(物理ボイススロット)の使い回し。`CInstCh::setModulation()`は発音中の`notes_[]`(自チャンネルが現在所有するdevChのみ)にしか`ISoundDevice::setCC1Modulation()`をpushせず、`CInstCh::noteOn()`側も新規ノート確保時にCC#1値を対象devChへ再送していなかった(該当箇所に「CC#1(pmDepth_)はここでは一切参照しない」という明示コメントあり)。一方`VoiceProcessor::reset()`(`ChState::free()`/`init()`から呼ばれる)は`chLfo_`はリセットするが`cc1Value_`/`cc1LfoMode_`はクリアしない設計のため、あるMIDIチャンネルでCC#1>0を送った後そのdevChがボイススチール等で別のMIDIチャンネルへ再割り当てされると、`VoiceProcessor::onNoteOn()`が残留した`cc1Value_>0`を見て(新ノートの音色が`sw.LFR=0`の場合)ソフトLFOを誤って起動していた。同種の問題は既にCC#77(ソフトウェアLFO Depth上書き)で認識・対策済みで、`CInstCh::noteOn()`が`ISoundDevice::setLfoDepthOverride()`を毎ノートオン必ずpushする実装になっていたため、これと同じパターンで`dev->setCC1Modulation(devCh, pmDepth_, modDepthRange_)`を毎ノートオンで必ずpushするよう追加して解消した | ✅ | `midi-implementation-status.md` |
| プロファイルの`banks`セクションを外部ファイルへ分離可能に(2026年7月新設)。パッチバンク構成(bank/prog番号の割り当て)はデバイス構成(`devices`/`hw_plugins`)に一切依存しないため、`banks`に文字列を指定すると外部JSONファイルへのパスとみなし、参照先オブジェクト(`hw_banks`/`sw_banks`/`patch_banks`/`drum_banks`/`scc_wave_banks`/`pcm_banks`/`sf2_banks`を持つオブジェクト、内容は従来の`banks`オブジェクト直書きと同一スキーマ)をそのままその位置に埋め込んだものとして読み込む(パス解決はプロファイル自身のディレクトリが基点、既存の`banks.*[].file`と同じ規則)。これにより、デバイス構成が異なる複数プロファイル間で同じプリセットバンク構成ファイルを使い回せる(デバイス構成に含まれないバンクエントリは単に発音しないだけであり、デバイス構成の一部変更がbank/prog番号の一貫性を崩すことはない)。`FITOMConfig::buildFromProfile()`が`resolveBanksSection()`(新設の静的ヘルパー、`Config.cpp`)で`banks`の実体(オブジェクトそのもの、または文字列なら参照先ファイルをパースした結果)を1回だけ解決し、`loadDrumBanks()`/`loadSf2Banks()`双方へ渡すよう変更(両関数はプロファイル全体ではなく解決済みの`banks`実体を直接受け取るようシグネチャ変更)。`profileJson_`(ロード時の生JSON、`saveProfile()`の書き戻しベース)は無加工のまま保持されるため、外部参照形式で書かれたプロファイルは書き戻し時も文字列参照のまま維持される(展開後のオブジェクトで上書きされない)。`config_schema/profile.schema.json`の`banks`プロパティを`oneOf`(文字列/オブジェクト)化、`banks/README.md`に使用例を追記 | ✅ | — |

---

## 既知の未対応・将来課題

- SF2直行パス(fluidsynth統合)は、このリポジトリのスコープであるFITOM_X本体側(設定読み込み・MIDIディスパッチ・プライベートSysEx)は実装済みだが、実際に音を鳴らすhwifプラグイン本体(仮称`FitomSf2IF`、別リポジトリ、`fluid_synth_t`をリンクする側)は未実装。また`sf2_channel_windows`編集用のGUI専用ダイアログ、MIDIモニターでのSF2直行パスチャンネルの表示も未着手(`docs/sf2-fluidsynth-integration.md`5節参照)
- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal（FM音源に対応するパラメータがないため意図的に非対応）
- RPN 0x0002 Coarse Tuning、RPN 0x7F7F Null
- CC#2/CC#4 の変数分離
- VoicePatchType 未実装チップ (MA3系列, SAA1099, AWM) のドライバ実装
- OPL/OPL2/OPL3自体のリズムモード対応（現状OPLL系のみ対応。COPL_new.cppにリズム関連コードなし）
- VoicePatchType 完全一致以外へのフォールバック（旧FITOMの互換リスト相当、将来実装予定）
- GUI (Dear ImGui) 実装の残り(`apps/fitom_gui`。MIDIモニターバンドは実装済み。デバイス一覧・パッチ一覧・音色エディタ等、他画面への導線が未着手)
- 外部パッチエディタ起動機能(ダブルクリック→キオスクモード起動)は実機でのダブルクリック確認済み(2026年7月、プロファイルパス解決バグ修正、上記表参照)。ただし`kind="device"`(直接モード)の解決経路(`pm.resolveDirect()`)は`PatchManager::resolveTriple()`がVoiceGroup照合のために`devices[]`から一致するデバイスを線形探索する都合上、プロファイルの`devices[]`に対応するHWプラグインDLLが実際にロードできていないと(`Config::buildDevice()`がHWプラグイン未登録時に早期returnし`DeviceEntry`自体を作らないため)解決に失敗する点は未検証のまま残っている(`kind="layered"`側は`pm.findPatchBank()`のみで完結するためこの制約を受けない、2026年7月のkind対応時に判明)。4引数キオスク起動(`<profile> <kind> <bank-file> <prog>`、パッチエディタ側D-039/D-040)自体の実機動作確認(実際にlayered/deviceそれぞれで適切な編集画面が開くこと)は未実施
- 外部パッチエディタとの内部用MIDIパイプ(`fitom_midi_pipe`)連携: 実機MIDIキーボード(rtmidi)との同時併用不可という設計上の制約は解消済み(2026年7月、プロファイル設定から独立して常時有効化するよう変更、上記変更履歴参照。`-DFITOM_BUILD_BACKEND_MIDI_PIPE=ON`でビルドすれば追加のプロファイル設定なしに常時有効)。PowerShellの簡易クライアントによる疎通確認(チャンネル割り当て通知・Program Change・Note On/Offの送受信)は実施済みだが、実際の外部パッチエディタプロジェクト(`FITOM_patch_editor`)との実接続確認は引き続き未実施
- GUI MIDIパイプ経由の音色試聴連携(`fitom_midi_pipe`側は実装済みだが、GUI側からのSysEx送出・パッチエディタ本体との結合は未着手)
- OPZ の2系統LFOリソース対応（旧FITOMも未完成のため現状維持）
- CAdPcmZ280 (YMZ280B/PCMD8) の旧FITOM実装との詳細突き合わせ未完了
- レジスタダンプモニター(`RegisterDumpWindow`)の基本動作(発光エフェクト・オルタネート表示・物理チップ名表示・高位ポート表示含む)はユーザーによる実機/実プラグイン接続環境での目視確認とフィードバックを経て、単一物理ポートで高位アドレス(0x100以降)を使うチップ(OPNA/OPN2/OPL3等)の高位側が表示されない不具合(`PhysicalChipInfo::dumpSize`新設で修正)、チップごとのテーブルに常にスクロールバーが出る不具合(テーブル高さ固定+ScrollYをやめ自動サイズ化で修正)の2件を修正済み。後者の修正自体は、この開発環境にHWプラグインDLL・物理チップが無くビルド確認までしかできていないため、ユーザーによる目視再確認が未実施
- AWM(YMF278)のSwPatch(パフォーマンスパッチ)非対応: `zones[].sw_bank/sw_prog`・`pcmbank.swpatches[]`の参照自体は解決されるが、`COPL4AWM`は音量計算・`onNoteOn()`呼び出しの対象外のままのため音には反映されない。実機がトレモロ・ビブラートをレジスタ`0x80-0x97`のLFO/VIBビットとしてデバイス機能に持ち、`samplezone`が参照する波形バイナリ側の設定と整合させる設計が必要なため(2026年7月、`patch-structure-design.md`参照)
- VGMレコーディング機能(GUIから任意開始・停止、最初のレジスタ書き込みから記録、停止時にファイナライズ)は技術検討完了・実装未着手(2026年7月、`docs/vgm-recording.md`参照)。`HWPort::write()`/`writeBurst()`(既存のシャドウレジスタ更新と同じチョークポイント)へのフック、`CFITOM::PhysicalChipInfo`(レジスタダンプモニター用の物理チップ列挙)を流用したヘッダ構築、壁時計ベースのwaitサンプル変換、`PcmBankRegistry`の`binPath`を使ったROMイメージdata block埋め込みという設計方針は確定しているが、コード実装(`VgmRecorder`新設、`FITOMBridge`のAPI追加等)には未着手
- チャンネルレベルメーター(`LevelMeterPanel`)は実機確認により、同種デバイス自動束ね(spanGroups、例: OPN2とOPNAのFM部が同一VoicePatchTypeのため自動的に束ねられる構成)が絡む物理チップで表示が大きく欠落する不具合が発見され修正した(2026年7月)。原因は`buildPhysicalChipList()`がspanGroupsで他デバイスに吸収された側・吸収した側の両方をサブデバイス内訳の対象外にしていたため。`CFITOM::initDevices()`のデバイス構築ループに`pendingSubDevices_`(span/stereo展開“前”の生ISoundDevice記録)を新設し、`buildPhysicalChipList()`側でport単位に突き合わせて`PhysicalChipInfo::subDevices`を復元するよう修正した(stereoPairPortで束ねられるデバイス自身は、L/R個別のch構成を安定して取り出せないため引き続き対象外、既知の制限)。この開発環境にはHWプラグインDLL・物理チップが無くビルド確認までしかできていないため、この修正自体のユーザーによる目視再確認が未実施。またサブデバイスの表示順(現状Config側の生成順)をユーザー例示の並び(例: OPNBならFM→PA→PB→SSGの順)に揃えるための並べ替えは未対応のまま(表示は生成順)。**恒常的無効チャンネルの非活性表示(2026年7月追加)**: OPNBのch0/ch3(実機に存在しない、`COPNB`が`enableCh(false)`で無効化)やOPL/OPLL系リズムモード時のch6-8(リズム専用に転用、`chState_[i].disable()`)は、既存の`ChState::isEnabled()`(`status != Status::Disabled`)をそのまま`PhysicalChipChannelState::enabled`/`FITOMLevelChannel::enabled`として伝播し、`LevelMeterPanel`側でバーを薄暗いブランクプレースホルダ(枠線・塗りつぶし無し、ラベルも減光)として表示するよう対応。判定はチップ種別ごとの特別扱いをせず、既存の`ChState`の無効化状態をそのまま使うだけで済んだ

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
