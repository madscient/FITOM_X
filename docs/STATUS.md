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
| `OPL_new.cpp` | ✅ | OPL/OPL2/COPL3(4OPモード)/COPL3_2(2OP、CSpanDevice)/COPLRhythm(内蔵リズム5パート)/COPL2EX(Y8960拡張OPL2部のFMコア、DEVICE_OPL2EX。COPL2にchごとの定位[0xF6-0xFE]の書き込みを足したもの。内蔵ADPCM-BはADPCM_new.cppのCY8960Delta[DEVICE_ADPCMB_OPL2EX、Y8950と同一レジスタ配置+定位0xFF]、内蔵リズムはCOPL2EXRhythm[DEVICE_OPL2EX_RHY]。`chip-driver-architecture.md`4.5.3節参照。実機・エミュレータでの音出し確認は未実施) |
| `OPLL_new.cpp` | ✅ | OPLL/OPLL2/OPLLP/OPLLX/VRC7/COPLLRhythm/COPLLEX(Y8960拡張OPLL部、DEVICE_OPLLEX。COPLLを継承し、チャンネル別ROMプリセットバンク選択レジスタ[0x40+ch、ext.ALG_EXTのbit2-1]と定位レジスタ[0x50+ch]の書き込みを追加。内蔵リズムはCOPLLEXRhythm[DEVICE_OPLLEX_RHY、定位を書く点以外は標準OPLLと同一]。`chip-driver-architecture.md`4.5.3節参照。hwbank.jsonのbuiltinエントリ(patch_type="OPLLEX")経由の名指し参照は未配線[*.hwbank.jsonのALGフィールドからは0-7まで直接指定可]。PatchManager::initOpllRomPatches()が生成する標準OPLL系(OPLL/OPLLX/OPLLP/VRC7)の暗黙ROMプリセットは、生成時点でext.ALG_EXTのbit2-1に対応するBANK値を焼き込み済みのため、要求チップが未接続ならresolveOpllRomVoice()がOPLLEXへフォールバックする[標準OPLL系どうしの相互フォールバックは引き続き非対応]。2026年8月、CC#0=OPLLEX・バンク0を直接選んだ場合も同じ暗黙バンクで解決できるよう拡張(resolveOpllRomVoice()にrequestedVoicePatchType引数を追加し、OPLLEX指定時はhwProg上位3bitを「フォールバック元チップ種別」ではなく「OPLLEX自身のBANK選択」として扱う設計に変更。他のOPLLファミリーと同様progだけで暗黙バンク内の最大60音色[4バンク×15]を一意に解決できる)。実機・エミュレータでの音出し確認は未実施) |
| `PSG_new.cpp` | ✅ | SSG/DCSG/SCC (CPSGBaseはSW-EG/SW-LFO共通化のみ)。CSSGはレジスタ空間を`unitBase(ch)`/`unitCh(ch)`(0x20刻み)経由で索引し、YM2149相当を複数内蔵するチップへ同じ実装を展開できる。CSSGS (SSG互換部、3ch×2ユニット=6ch) はこれを継承し、chごとの4bitパンポット($10-$12/$30-$32)を追加するのみ。CSSGSはYMZ705(SSGS)とその上位互換のYMZ732(SSGS2)を共用で担当する(レジスタマップは完全に同一。deviceTypeを分けているのはマスタークロックの分周比の違い[SSGS=4.096MHz÷2または6.144MHz÷3 / SSGS2=12.288MHz÷6固定]だけで、判定はDeviceFactory::ssgsSsgBlockClock()が行う。SSGS2のシンプルアクセスモードはFITOMが使わないハードウェアモードのため対応不要)。さらにCSSGSを継承するCSSGS3がYMZ771(SSGS3)を担当する。SSG音源としての機能は同一で、違いは①レジスタが機能ごとにまとめ直され$10-$32へ並ぶ、②パンポットが5bit(0-31、16=中央)、③SSGトータルボリューム($32、128で100%、**リセット値0は完全無音のためinit()で必ず書く**)が追加、の3点のみ。CSSGのレジスタアドレスを仮想の索引メソッド(toneReg/noiseReg/mixReg/volReg/envPeriodReg/envShapeReg)へ集約したため、CSSGS3はそれらを差し替えるだけでレジスタ書き込みの本体コードには一切手を入れていない。クロックは16.384MHz÷8。SSGS3のAMM(MPEG Audioコーデック)部は当面非対応でcomposite展開もしない。VoicePatchTypeは3品種ともVOICE_PATCH_SSGなので混在構成でもCSpanDeviceで束ねられる。実機・エミュレータでの音出し確認は未実施(SSGS/SSGS2/SSGS3とも)。CSSGSを継承するCSSGEXがY8960のSSG部(DEVICE_SSGEX、SSG-2が0x10から・chごとの音量0x20-・8bit定位0x28-)を、CSCCがY8960のSCC部(DEVICE_SCCEX、無印SCCの配置+定位0x100-0x104)を担当する(`chip-driver-architecture.md`4.5.3節)。いずれも実機・エミュレータでの音出し確認は未実施 |
| `DSG_new.cpp` | 🚧 | CDSG (YM2163楽音4ch) / CDSGRhythm (内蔵リズム5パート: BD/HC/SDN/HHO/HHD)。ROM固定音色のみでユーザー音色は持たず、「エンベロープ4種×波形5種」の20音色をPatchManagerが暗黙のバンクとして機械生成する(prog = 波形*4 + エンベロープ、音色名は`<波形名>.<エンベロープ名>`)。サスティンペダルをハードウェアのSUSビットへ直結。CPSGBaseは継承せず(内蔵EGを持つためソフトウェアADSR不要)、周波数も共有の周期テーブル(SN76489の1/32スケール)では精度が足りないためHzから直接DV/octへ分解する。ビルトイン音色へのSwPatch紐づけは、OPLL ROM音色用のメタバンク(`role=="builtin_swpatch_meta"`)をチップ非依存化して共有する(`opllBuiltinMetaBank_`→`builtinMetaBank_`へ改名、`BuiltinRef::patchType`に`4=DSG`を追加、`isValid()`を`patchNo>=0`へ緩和[DSGはprog 0が正規の音色のため])。GUI導線も接続済み(パッチピッカーのカテゴリに`0x44 "DSG"`[単一の"Builtin"バンク+20音色]、内蔵リズム`0x70`配下のチップ一覧に`DSG`[5パート]を追加。MIDIモニターの名前解決も両方に対応)。なお`VOICE_PATCH_DSG`は`isPsgFamilyVoicePatchType()`に**含めない** — ユーザー音色を持たずHwBankが存在しないため、共有バンク入口(`VOICE_PATCH_SSG`)へ寄せるとパッチピッカーのDSGカテゴリにSSGのバンクが並んでしまう。実機・エミュレータでの音出し確認、およびGUIの目視確認は未実施 |
| `MultiDev_new.cpp` / `include/fitom/MultiDevice.h` | ✅ | CMultiDevice/CSpanDevice/CUnison (ヘッダー化済み) |
| `ADPCM_new.cpp` | ✅ | CYmDelta(Y8950/OPNA/OPNB/OPL2EX[DEVICE_ADPCMB_OPL2EX、CY8960Delta。Y8960拡張OPL2部の内蔵ADPCM-B。レジスタ配置がY8950と同一のためkY8950_DeltaTを流用し、定位レジスタ0xFFを足す。メモリ容量256KBは仕様書のSerialSRAMの割り当てで確認済み、分周比はY8950と同一という推測で仕様書に記載なし])/CAdPcm2610A/CAdPcmZ280/CSSGSAdPcm |
| └ `CSSGSAdPcm` | 🚧 | YMZ705(SSGS)/YMZ732(SSGS2)内蔵ADPCM 8ch(両チップで制御が完全に同一のためdeviceType `DEVICE_SSGS_ADPCM` を共有し、混在構成でも束ね対象・PCMイメージも共通)。他のADPCM系と違い、①波形の開始/終了アドレスレジスタを持たず(チップが外部メモリ先頭のボイステーブルを自力で引くため、書くのは6bitのボイス番号=`SampleZone::waveIndex`だけ)、②再生ピッチを変えられない(サンプリング周波数32k/16k/8k/4kの4択のみ、`pcmbank.json`の`sample_rate`から決まる)。専用の`VOICE_PATCH_SSGS_ADPCM`(0x55)、PCMメモリイメージのカタログ種別は`SSGS_ADPCM`。**ADPCMのコーデックはデータシートに記載が無く不明。エミュレーション側は`EPSGemuEngine`(別リポジトリ)がYMZ280Bと同じ4bit ADPCMと仮定して実装している**。`FitomEmuIF`の外部メモリ対応表(`pcm_mappings_for_chip()`)はSSGS/SSGS2の`PCM`へカタログ種別`SSGS_ADPCM`のイメージを渡す(確認済み: FitomEmuIF 2d0e443のソースを読んで確認。音出しは未確認)。鳴ったとしても、コーデックが仮定である以上、実機と同じ音かは判断できない。レジスタ書き込みの検証(`tests/test_ssgs.cpp`)のみ実施済み。音量4bitを同一チップのSSG部と同じ対数DAC(3dB/step)と解釈している点、およびサンプリング周波数のビット値対応(00=4k〜11=32k)はデータシートに記載が無く、実機での確認が必要 |
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
| `apps/fitom_gui/` | 🚧 | Dear ImGui + GLFW + OpenGL3 導入済み。ルート画面のMIDIモニターバンド(CH毎のBank/Program/Volume/Note/Device/Fnumber表示、MPU切替、128ノートキーボードビュー+発光エフェクト)を実装済み。Bank/Program表示のダブルクリックで外部パッチエディタ(別リポジトリ`FITOM_patch_editor`、実行ファイルは`fitom_gui`と同じディレクトリに配置想定)をキオスクモード(`<profile.json> <kind> <bank-file> <prog>`、`kind`="device"/"layered"/"drum"、D-039/D-040)で子プロセス起動する機能を実装済み(パッチエディタ未検出・起動失敗時はImGuiモーダルでエラー表示。実機での起動確認は未実施、下記STATUS.md注記参照)。CH番号のシングルクリックでCH設定ダイアログ(`ChSettingsDialog`)を開き、Volume(CC#7)/Expression(CC#11)/リズム⇔インストゥルメント切替(CC#0特殊値)/Poly⇔Mono切替(CC#126/127)/パッチ選択を変更できる機能を実装済み。パッチ選択はCC#0→CC#32→Prog.chgの階層ブラウジングを行う`PatchPickerDialog`(直接デバイス選択モードも対応)、リズムチャンネルはドラムキットのフラット一覧を別途実装済み。適用ロジックはGUI側から`IMidiCh`のsetterを個別に叩かず、`FITOMBridge::sendControlChange`/`sendProgramChange`経由でコアの既存MIDI処理経路(`MidiProcessor::processControl`/`IMidiCh::progChange`)を再利用する方式(2026年7月)。パッチピッカーのProg.chg階層は、行をマウスボタンダウンした瞬間にCC#0/CC#32/Prog.chg/Note On(C4、`FITOMBridge::sendNoteOn`/`sendNoteOff`新設)を送り、ボタンアップでNote Offを送る「押している間だけ鳴る」試聴動作を実装済み(`ImGui::IsItemActivated`/`IsItemDeactivated`でボタンダウン/アップを直接検出。同じ行への連打も含め、押すたびに必ず送り直す)。確定は「選択」ボタンのみで行う(以前はダブルクリックでも確定していたが、連打時に試聴のNote Onと確定側のNote Offが競合し音が鳴らなくなる不具合があり、2026年7月に確定操作を「選択」ボタンへ一本化して解消)。確定時は試聴中の値のままNote Offのみ送り(通常は既にボタンアップで止まっている)、ピッカー/CH設定いずれのキャンセルでも試聴前(open()時点)のCC#0/CC#32/Prog.chgへ復元するメッセージを送り直す。CH設定ダイアログのVolume/Panpot/Expressionスライダーも操作するたびにCC#7/#10/#11を即時送信してプレビューでき、キャンセル時は開いた時点の値へ復元する(2026年7月)。MIDIモニターのバンク名・パッチ名解決は、直接デバイス選択モード(HwBankRegistry参照)にも対応済み(2026年7月修正。以前は通常モード[PatchBank]とリズムチャンネルのみ対応で、直接モードのチャンネルは常に数値フォールバック表示になっていた)。MIDIモニターのMIDIポート名部分のクリックでMIDIポート設定ダイアログ(`MidiPortSettingsDialog`)を開き、MPU(`CFITOM::getMpuCount()`、現状4)分のMIDI入力ポート割り当てをそれぞれドロップダウン(システムが現在列挙するポート名一覧、`FITOMBridge::getAvailableMidiInputPorts()`)から変更できる機能を実装済み(2026年7月新設)。バリデーションは重複設定(複数ポートへの同一MIDI IN割り当て)チェックのみで、違反時はエラーメッセージボックスを表示してOKを継続させない。OKで閉じると`FITOMBridge::setMidiInputPorts()`が既存の全MIDI入力ポートを閉じてから選択内容で開き直し(即時反映)、`FITOMConfig`側の設定(現在のプロファイル状態)を更新したうえで、`FITOMBridge::saveCurrentProfile()`経由で現在ロード中のプロファイルファイルへ書き戻す。この変更に伴い、コア側(`CFITOM::init()`)もMIDI入力ポート数に関わらず常にMAX_MPUS(4)分のMidiProcessor/チャンネルを生成するよう修正した(以前は設定済みポート数分しか生成せず、未設定のMPUには実行中に新規ポートを割り当てられなかった)。MIDIモニター左上の歯車アイコンボタン(`gearIconButton()`、日本語フォントのグリフ収録範囲に依存しないようImDrawListへの直接描画で実装)からシステム設定ダイアログ(`SystemSettingsDialog`)を開き、マスターボリューム・マスターピッチ(430〜450Hz)を変更できる機能を実装済み(2026年7月新設。ChSettingsDialogのVolume/Panpot/Expressionスライダーと同じくドラッグ中は即時プレビュー、キャンセルで開いた時点の値へ復元)。こちらもOKで`FITOMBridge::saveCurrentProfile()`によりプロファイルファイルへ書き戻す。プロファイルスキーマに`master_volume`/`master_pitch`フィールドを新設し(`config_schema/profile.schema.json`)、`FITOMConfig::buildFromProfile()`で読み込み・`FITOMConfig::saveProfile()`で書き戻す(ロード時のJSON全体を`profileJson_`に保持しておき、GUIから変更されるフィールドのみ上書きして書き出すことで、devices/hw_plugins/banks等の他のフィールドはそのまま維持される)。あわせて、`FITOMBridge::setMasterPitch()`が従来`FnumRegistry`を直接叩いておりConfig側の値が更新されず発音中チャンネルへのF-number即時反映も行われていなかったバグを、`CFITOM::setMasterPitch()`経由に修正した。デバイス一覧・パッチ一覧(フラット表示)等、他画面への導線は未着手(該当描画関数は`[[maybe_unused]]`で温存)。起動時プロファイル選択画面(2026年8月新設): コマンドライン引数省略で起動した場合、`fitom.conf.json`の`profiles.root`(省略時はカレントワーキングディレクトリ)配下の`*.profile.json`を一覧し、クリックで選んで起動できる(`FITOMBridge::listAvailableProfiles()`→`FITOMConfig::listProfiles()`。表示名はプロファイルの`profile_name`フィールド、無ければファイル名)。コマンドライン引数で明示指定した場合の挙動(失敗時に固定エラー表示のまま)は変更していない。`fitom_cli`は対象外で、引数省略時は従来通りエラー終了する。実行中のプロファイル切替(2026年8月新設): MIDIモニターバンド上部の「プロファイル切替」ボタンから、同じ一覧を再度モーダル表示できる。`CFITOM::exit()`は二重呼び出し防止の冪等ガードを持ち一度exit()すると以後何もしなくなる設計のため(アプリ終了時の一度きりの呼び出しを前提)、実行中のコア再初期化(exit→init)は行わず、選択したプロファイルを引数に自分自身(`fitom_gui.exe`)を新しいプロセスとしてfire-and-forget起動し(`launchProcess()`、外部パッチエディタ起動と同じ仕組み)、現在のウィンドウを閉じて通常のシャットダウン処理へ流す方式で実現している(実質的な切替に見えればよいという方針、コアの状態は一切引き継がない)。**新プロセスの起動は、必ずこのプロセスの`bridge.exit()`(MIDI入力ポート・HWプラグイン[実機ならCOMポート/USB、エミュレータならオーディオストリーム]・内部用MIDIパイプの解放を含む)を先に完了させてから行う**(2026年8月、レビュー指摘により順序を修正。以前は新プロセスの起動が先で、旧プロセスの解放は`main()`末尾のシャットダウン処理まで遅延しており、新旧プロセスが同じMIDIポート/HWデバイスを同時に掴もうとする競合状態になりうる不具合があった)。新プロセスの起動自体(`CreateProcess`等)に失敗した場合、`CFITOM::exit()`のexited_ガードが一度trueになると以後リセットされない設計であるため、このプロセス内での再init()(元プロファイルへの復帰)は行わずコア無し状態にとどめ、ユーザーに手動再起動を促す(exit→initの繰り返しに依存すると、その後の本当の終了時にHWPluginRegistry::closeAll()等の後始末がexited_の再チェックで無条件にスキップされてしまうため)。プロファイル起動失敗時のフォールバック(2026年8月新設): `fitom_gui`はコマンドライン第2引数(フォールバックプロファイル)を受け付けるようになった(`fitom_gui <profile.json> [<fallback_profile.json>]`)。プロファイル切替は、切替前の現在のプロファイル(`bridge.currentProfilePath()`)を第2引数として新プロセスへ渡す。新プロセス側で第1引数のプロファイル読み込みに失敗した場合、新プロセス自身が(exit()を経由せず、まだ一度もinit()していない状態のまま)第2引数で再度`bridge.init()`を試み、成功すればそちらで起動する(エラーポップアップで復帰した旨を通知)。この機能のために`FITOMBridge::init()`の`config->loadProfile()`戻り値チェック漏れも修正した(以前はプロファイル読み込みが失敗しても`CFITOM::init()`が常に0を返す実装のため`bridge.init()`が常にtrueを返しており、不正プロファイルでも「読み込み成功したが0デバイス」という無自覚な状態になっていた。`apps/fitom_cli`は元々このチェックを行っていたが`gui/bridge`側は漏れていた) |ウィンドウタイトル(2026年8月新設): タイトルバーに`FITOM_X [<プロファイル表示名>]`の形式で現在ロード中のプロファイルを表示する(`FITOMBridge::profileDisplayName()`→`FITOMConfig::peekProfileDisplayName()`。表示名の解決規則は`listProfiles()`と共通で、`profile_name`フィールド優先・無ければファイル名)。プロファイル未指定起動時はウィンドウ作成時の既定タイトル("FITOM_X"のみ)のまま |

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
| リニアステレオ化 (CLinearPanDevice、`stereo_pair`の明示指定でL/R2台を等パワーパンニングのステレオ1台に束ねる。deviceType基準・composite展開チップ対応)。左右の分離方法は2方式: `true`=プラグイン側の出力ルーティング(`pan:1/2`、モノラルチップでも可) / `"L"`・`"R"`=チップ自身のL/R出力ビット(OPM/OPZ/OPL3/OPL4[FM部]/OPN2/OPNA/OPNB/OPNBB系、プラグイン側は`pan:0`のままでよくOPL4のAWM部が自前のパンポットをフル活用できる) | ✅ | `chip-driver-architecture.md`, `config-design.md` |
| OPL3 4OPモード (COPL3) + 疑似デチューン(DT2転用) | ✅ | `chip-driver-architecture.md` |
| OPN FXモード (3rd channel special mode、疑似デチューン/非整数倍率/固定周波数) | ✅ | `chip-driver-architecture.md`, `voice-data-design.md` |
| COPNARhythm / COPLLRhythm (内蔵リズム音源、独立レジスタ体系) | ✅ | `chip-driver-architecture.md` |
| CPSGBase 責務整理 (SW-EG/SW-LFO共通化のみに純化、SSG固有コードをCSSGへ移動) | ✅ | `chip-driver-architecture.md` |
| リリース中再トリガー対策 (wasReleasing、OPM/OPN/OPL/OPL3) | ✅ | `chip-driver-architecture.md` |
| ADPCM RegMap 全面修正 (Y8950/OPNA/OPNB個別マップ、memory/panmaskフィールド追加) | ✅ | `chip-driver-architecture.md` |
| OPLL Fnumberビットシフト修正 | ✅ | `chip-driver-architecture.md` |
| OPLL EGT/RR静的変換 (`SR>0`ならEGT=0+RRレジスタ=SR。キーオン/キーオフ時の動的書き換えは廃止しOPL系とは別方式。実機OPLLのEG挙動がOPL系と異なることを2026年8月に実機確認済みの**確定仕様**) | ✅ | `chip-driver-architecture.md`, `voice-parameter-reference.md`, `manuals/hwpatch-reference.md` |
| HWPlugin_Shutdown (未エクスポート時は何もしないオプショナルAPI、二重実行防止) | ✅ | `plugin-hwif.md` |
| HWプラグインの部位ごとのゲイン(2026年10月新設、任意の組: `HWPlugin_GetPartCount`/`GetPartName`/`SetPartGain`/`GetPartGain`)。チップが別々の端子から出す出力(OPNAのFMとSSG等)のバランスを、FMエンジン内蔵hwifに対してFITOMから調整する。先行実装はFitomEmuIFだが、契約の正本は`plugin_sdk/include/fitom/IHWPlugin.h`。**構成**: (1) `HWPluginInstance::load()`が組の有無を`HWPlugin_GetPartCount`だけで判定する(先頭があるのに残りが欠けるプラグインはロード失敗)。(2) `HWPort`がOpen直後に部位の一覧と既定値を控える(`getParts()`)。(3) `FITOMConfig`がプロファイルのトップレベル`part_gains`を読み、デバイスを開いた直後に適用し、`saveProfile()`で書き戻す。デバイスはラベルではなく「プラグイン名+`HWPlugin_Open`に渡すparams」で特定する(ステージングのプロファイルにラベルの重複が実在するため)。保存するのは既定値から変えた部位だけで、該当デバイスの無いエントリも捨てずに書き戻す。(4) `CFITOM::getPhysicalChipPartGains()`/`setPhysicalChipPartGain()`が物理チップ単位で公開し、`processMutex_`を取る(`HWPlugin_Write`との並行可否を契約が定めていないため、MIDI処理・タイマーと直列化して判断を不要にした)。(5) `FITOMBridge::getHwChipPartGains()`/`setHwChipPartGain()`と、システム設定ダイアログ(`SystemSettingsDialog`)の部位ごとのL/Rスライダー(即時プレビュー、キャンセルで復元、OKで書き戻し。L/R連動チェックと既定値ボタン付き。スライダーの上限は2.0=約+6dB)。**確認済み**(検証用プラグイン`tests/stub_hwplugin`を組のエクスポート状態だけ変えて3通りにビルド[差はdumpbinで確認]し、`tests/test_hwport.cpp`・`tests/test_part_gain.cpp`から実行): 部位名の列挙と既定値(1.0以外を含む)、L/R独立の設定と読み戻し、持たない名前の拒否、組を持たないプラグインで部位0個、先頭だけのプラグインでロード失敗、`part_gains`の読み込み時適用(ラベルが重複していてもparamsで1台に決まる・`auto_devices`のデバイスにも掛かる)、変えた部位だけが書き戻される・既定値に戻すと消える・全部戻すとキーごと消える、該当デバイスの無いエントリの保持、書き戻した値が0.37のような短い10進になり読み直すと同じfloatになること、他のフィールドがロード時のまま残ること。ローダーの判定・未エクスポート時のガード・読み込み時適用・paramsでの特定・既定値の除外・10進表記の6点は、該当箇所を壊すとテストが落ちることも確認した。宣言19本はFitomEmuIF側の写し(`3258d50`)とコメント・並び順を除いて一致し(FitomEmuIFは`ec6e471`以降、写しを正本とバイト単位で同一にしている。diffで確認済み)、同リポジトリでビルド済みの`FitomEmuIF.dll`が4シンボルをエクスポートしていることもdumpbinで確認。`fitom_cli`を検証用プラグインのプロファイルで起動し、CFITOMの初期化が最後まで通って動き続けること、保存済みの値が該当デバイスに適用された旨のログ(`part_gains: SSG = [0.5, 0.75] ...`)が出ることも確認した。**前提**: 同じparamsのデバイスを同時に2つ開けないこと(FitomEmuIFは1チップ1ハンドル)。これが崩れると、paramsでは1台に決まらない。**見送った案**: (i) 手書き用のキーを`devices[].part_gain`と`hw_plugins[].auto_devices_part_gain`に置く案。理由: 利用者が「GUIで変えて書き戻す実行時設定」を選んだ。(ii) 値をdBで書く案。理由: 契約と同じ線形なら無音(0)をそのまま書け、既定値が0の部位も同じ表記で扱える。(iii) デバイスを`label`で特定する案。理由: ラベルの重複が実在する。(iv) 変えていない部位も含めて全部保存する案。理由: プラグイン側の既定値が変わっても、古い値で上書きし続けることになる。**未検証**: (a) GUI(スライダーの見た目と操作、OK/キャンセルの挙動)。コンパイルが通ることしか見ていない。(b) `CFITOM`/`FITOMBridge`層(物理チップの添字からポートへの対応付けとロック)。テストが通しているのは`FITOMConfig`と`HWPort`までである。(c) 実物のFitomEmuIFをFITOM_Xからロードして4関数を呼ぶこと、ゲインが出力の音量に効くこと。名前指定の部位ゲインはYMEngine(`df2c92b`)・DSGemuEngine(`f7834f6`、origin側のみで手元の作業ツリーは未取得)・DSAemuEngine(`b90bcc6`)が実装済みだが、手元でビルド済みの`YMFMEngine.dll`は`df2c92b`より前のビルドで、これらを組み合わせて鳴らしたことはない。(d) 「MIDI処理・タイマーの`HWPlugin_Write`と並行しない」は、ロックを取る箇所(`receiveMpuByte`/`receiveInternalPipeByte`/`sendChannel*`/`timerCallback`/`pollingCallback`/`midiClockCallback`)を読んで判断したもので、並行実行で確かめてはいない | 🚧 | `plugin-hwif.md`, `config-design.md` |
| GUI MIDIモニターバンド (CH毎表示 + 128ノートキーボードビュー + 発光エフェクト) | ✅ | — |
| GUI CH設定ダイアログ + パッチピッカーダイアログ (Volume/Expression/リズム⇔インストゥルメント切替/Poly⇔Mono切替/CC#0→CC#32→Prog.chg階層ブラウジング + Prog.chg選択時の試聴(Note On C4)・キャンセル時の復元。GUIからは`FITOMBridge`のMIDI送信メソッド経由でコアの既存MIDI処理経路を再利用) | ✅ | — |
| 内部用MIDIパイプ (`fitom_midi_pipe`、パッチエディタ試聴連携) | ✅ | `plugin-midi-pipe.md` |
| 内部用MIDIパイプの多接続化(最大16本+接続直後のチャンネル自動割り当てSysEx) | ✅ | `plugin-midi-pipe.md` |
| MIDIバックエンドDLLをRtMidi単一実装へ統合 (旧midi_wms/midi_winmm/midi_alsaの3実装廃止、SysEx未対応だった既存欠陥を解消、macOS対応を新規追加) | ✅ | `plugin-midi.md` |
| OPNB(YM2610無印)の誤分類修正 (VOICE_PATCH_OPNからOPN2側へ、COPNB新設でSSG/ADPCM-Aサブデバイス自動生成・実効4ch化に対応。ステージング環境からの指摘で発覚) | ✅ | `chip-driver-architecture.md` |
| `CSoundDevice::chState_`等、チャンネル数を固定長配列で持っていた箇所のvector化 (OPL4 AWM=24chがMAX_CHS=16固定配列を超えて範囲外アクセスするクラッシュを修正。`chState_`本体に加え`CPSGBase::lfoTL_`/`envelopes_`、`CLinearPanDevice::masterVolume_`/`masterPan_`も同種のためvector化。ステージング環境での「無言で強制終了」報告から発覚) | ✅ | `chip-driver-architecture.md` |
| GUI MIDIポート設定ダイアログ (`MidiPortSettingsDialog`、MIDIモニターのポート名クリックで開く。MPU 4面分のMIDI入力ポート割り当てをドロップダウンで変更、重複設定のみバリデーション、OKで即時反映+プロファイル書き戻し。`CFITOM::init()`をMIDI入力ポート数に関わらず常に4MPU分生成するよう修正し、実行中の未使用MPUへのポート割り当てに対応) | ✅ | `config-design.md` |
| GUI システム設定ダイアログ (`SystemSettingsDialog`、MIDIモニター左上の歯車アイコンボタンで開く。マスターボリューム/マスターピッチ(430〜450Hz)をスライダーで即時プレビュー、OKでプロファイル書き戻し。プロファイルスキーマに`master_volume`/`master_pitch`を新設し、`FITOMConfig::saveProfile()`(新設、ロード時JSONへの差分上書き書き戻し)経由で永続化。`FITOMBridge::setMasterPitch()`がConfig/発音中チャンネルへの反映を素通りしていたバグも合わせて修正) | ✅ | `config-design.md` |

変更・修正・調査の経緯は`docs/worklog/`に月ごとに置く。

---

## 既知の未対応・将来課題

- SF2直行パス(fluidsynth統合)は、このリポジトリのスコープであるFITOM_X本体側(設定読み込み・MIDIディスパッチ・プライベートSysEx・プリセット名解決用`phdr`パース・MIDIモニターバンドへの表示配線・CH設定ダイアログでのチャンネル単位の窓切替)は実装済みだが、実際に音を鳴らすhwifプラグイン本体(仮称`FitomSf2IF`、別リポジトリ、`fluid_synth_t`をリンクする側)は未実装。GUI側(MIDIモニターバンドの表示・CH設定ダイアログの新規UI)は実機/実プラグイン接続環境での目視確認が未実施。また`sf2_channel_windows`(複数チャンネル分)を一覧編集する専用ダイアログも未着手(チャンネル単位の切替はCH設定ダイアログで対応済みのため優先度は低い、`docs/sf2-fluidsynth-integration.md`5節参照)
- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal（FM音源に対応するパラメータがないため意図的に非対応）
- RPN 0x0002 Coarse Tuning、RPN 0x7F7F Null
- CC#2/CC#4 の変数分離
- VoicePatchType 未実装チップ (MA3系列, SAA1099, AWM) のドライバ実装
- VoicePatchType 完全一致以外へのフォールバック（旧FITOMの互換リスト相当、将来実装予定）
- GUI (Dear ImGui) 実装の残り(`apps/fitom_gui`。MIDIモニターバンドは実装済み。デバイス一覧・パッチ一覧・音色エディタ等、他画面への導線が未着手)
- 外部パッチエディタ起動機能(ダブルクリック→キオスクモード起動)は実機でのダブルクリック確認済み(2026年7月、プロファイルパス解決バグ修正、`docs/worklog/2026-07.md`参照)。ただし`kind="device"`(直接モード)の解決経路(`pm.resolveDirect()`)は`PatchManager::resolveTriple()`がVoiceGroup照合のために`devices[]`から一致するデバイスを線形探索する都合上、プロファイルの`devices[]`に対応するHWプラグインDLLが実際にロードできていないと(`Config::buildDevice()`がHWプラグイン未登録時に早期returnし`DeviceEntry`自体を作らないため)解決に失敗する点は未検証のまま残っている(`kind="layered"`側は`pm.findPatchBank()`のみで完結するためこの制約を受けない、2026年7月のkind対応時に判明)。4引数キオスク起動(`<profile> <kind> <bank-file> <prog>`、パッチエディタ側D-039/D-040)自体の実機動作確認(実際にlayered/deviceそれぞれで適切な編集画面が開くこと)は未実施
- 外部パッチエディタとの内部用MIDIパイプ(`fitom_midi_pipe`)連携: 実機MIDIキーボード(rtmidi)との同時併用不可という設計上の制約は解消済み(2026年7月、プロファイル設定から独立して常時有効化するよう変更、`docs/worklog/2026-07.md`参照。`-DFITOM_BUILD_BACKEND_MIDI_PIPE=ON`でビルドすれば追加のプロファイル設定なしに常時有効)。PowerShellの簡易クライアントによる疎通確認(チャンネル割り当て通知・Program Change・Note On/Offの送受信)は実施済みだが、実際の外部パッチエディタプロジェクト(`FITOM_patch_editor`)との実接続確認は引き続き未実施
- GUI MIDIパイプ経由の音色試聴連携(`fitom_midi_pipe`側は実装済みだが、GUI側からのSysEx送出・パッチエディタ本体との結合は未着手)
- **OPM/OPZ ハードウェアLFO対応: 設計検討完了・実装未着手(2026年8月)**。`docs/opm-opz-hw-lfo.md`参照
- SCCの波形バンク番号が常に0固定: `CSCC::setWaveBankNo()`がどこからも呼ばれておらず、複数の`sccwave`バンクを音色データ側から使い分ける経路が未接続。バンク0だけなら現状で動作する(2026年8月、SCCの発音確認作業中に発見)
- CAdPcmZ280 (YMZ280B/PCMD8) の旧FITOM実装との詳細突き合わせ未完了
- レジスタダンプモニター(`RegisterDumpWindow`)の基本動作(発光エフェクト・オルタネート表示・物理チップ名表示・高位ポート表示含む)はユーザーによる実機/実プラグイン接続環境での目視確認とフィードバックを経て、単一物理ポートで高位アドレス(0x100以降)を使うチップ(OPNA/OPN2/OPL3等)の高位側が表示されない不具合(`PhysicalChipInfo::dumpSize`新設で修正)、チップごとのテーブルに常にスクロールバーが出る不具合(テーブル高さ固定+ScrollYをやめ自動サイズ化で修正)の2件を修正済み。後者の修正自体は、この開発環境にHWプラグインDLL・物理チップが無くビルド確認までしかできていないため、ユーザーによる目視再確認が未実施
- AWM(YMF278)のSwPatch(パフォーマンスパッチ)非対応: `zones[].sw_bank/sw_prog`・`pcmbank.swpatches[]`の参照自体は解決されるが、`COPL4AWM`は音量計算・`onNoteOn()`呼び出しの対象外のままのため音には反映されない。実機がトレモロ・ビブラートをレジスタ`0x80-0x97`のLFO/VIBビットとしてデバイス機能に持ち、`samplezone`が参照する波形バイナリ側の設定と整合させる設計が必要なため(2026年7月、`patch-structure-design.md`参照)
- VGMレコーディング機能(GUIから任意開始・停止、最初のレジスタ書き込みから記録、停止時にファイナライズ)は技術検討完了・実装未着手(2026年7月、`docs/vgm-recording.md`参照)。`HWPort::write()`/`writeBurst()`(既存のシャドウレジスタ更新と同じチョークポイント)へのフック、`CFITOM::PhysicalChipInfo`(レジスタダンプモニター用の物理チップ列挙)を流用したヘッダ構築、壁時計ベースのwaitサンプル変換、`PcmBankRegistry`の`binPath`を使ったROMイメージdata block埋め込みという設計方針は確定しているが、コード実装(`VgmRecorder`新設、`FITOMBridge`のAPI追加等)には未着手
- チャンネルレベルメーター(`LevelMeterPanel`)は実機確認により、同種デバイス自動束ね(spanGroups、例: OPN2とOPNAのFM部が同一VoicePatchTypeのため自動的に束ねられる構成)が絡む物理チップで表示が大きく欠落する不具合が発見され修正した(2026年7月)。原因は`buildPhysicalChipList()`がspanGroupsで他デバイスに吸収された側・吸収した側の両方をサブデバイス内訳の対象外にしていたため。`CFITOM::initDevices()`のデバイス構築ループに`pendingSubDevices_`(span/stereo展開“前”の生ISoundDevice記録)を新設し、`buildPhysicalChipList()`側でport単位に突き合わせて`PhysicalChipInfo::subDevices`を復元するよう修正した(stereoPairPortで束ねられるデバイスも2026年8月に対応済み。`docs/worklog/2026-08.md`のL/R分割表示の項参照)。この開発環境にはHWプラグインDLL・物理チップが無くビルド確認までしかできていないため、この修正自体のユーザーによる目視再確認が未実施。またサブデバイスの表示順(現状Config側の生成順)をユーザー例示の並び(例: OPNBならFM→PA→PB→SSGの順)に揃えるための並べ替えは未対応のまま(表示は生成順)。**恒常的無効チャンネルの非活性表示(2026年7月追加)**: OPNBのch0/ch3(実機に存在しない、`COPNB`が`enableCh(false)`で無効化)やOPL/OPLL系リズムモード時のch6-8(リズム専用に転用、`chState_[i].disable()`)は、既存の`ChState::isEnabled()`(`status != Status::Disabled`)をそのまま`PhysicalChipChannelState::enabled`/`FITOMLevelChannel::enabled`として伝播し、`LevelMeterPanel`側でバーを薄暗いブランクプレースホルダ(枠線・塗りつぶし無し、ラベルも減光)として表示するよう対応。プレースホルダを出すのは物理チップ単位表示のみで(右ペインのレジスタダンプと物理ch番号で突き合わせるため歯抜けの位置に意味がある)、論理チップ単位表示では詰めて省く。判定はチップ種別ごとの特別扱いをせず、既存の`ChState`の無効化状態をそのまま使うだけで済んだ
- Y8960の仕様変更への追従(2026年10月)の残り: (1) 別リポジトリ側の追従は済んでいる。DSAemuEngineはY8960のSSG部を`SSGEX`の名前で公開し、旧名`SSGS`を受け付けない(46c2fbe)。FitomEmuIFの外部メモリ対応表はOPL2EXの`ADPCM_B`へY8950と同じカタログ種別`ADPCM-B`のイメージを渡す(2d0e443)。いずれもソースを読んで確認済みで、FITOM_Xと結合しての音出しは未確認。(2) Y8960のOPM(YM2151、10月20日追加)は`chip:"OPM"`として既存の`COPM`で扱う。仕様書のOPMシートにはI/Oポートしか無く拡張レジスタは無い。クロックはMSXスロットのシステムクロック3.579545MHzをそのまま使う(仕様書には記載が無く、利用者からの情報)。`COPM`は3.579545MHz基準のKC表で音程を出す(`OPM_new.cpp`の`kMasterTuneBase`)ため、変更は不要。実機・エミュレータでの音出しは未確認。(3) カートリッジ側のミキサー(ブロックごとの音量 0x7FF7-0x7FFE)はチップのレジスタではなくカートリッジのメモリマップドI/Oのため、FITOM_Xのチップドライバの対象外。扱うならHW I/Fプラグイン側の部位ゲインとして実装する。(4) 仕様書のI/OはOPLL 1組(7Ch/7Dh)だけで、カートリッジの作業履歴(`history.md`、10月16日)にも2回路から1回路への変更が記録されている。ステージングの`emu_msx2pp`プロファイル(8月27日更新)は、OPLLEX・OPL2EXを各2個(エンジン`Y8960emuEngine`、pan 1/2のペア)で構成しており、SSGEX/SCCEX/OPMは使っていない(確認済み: ステージングのorigin/mainのプロファイルを読んで確認)。このプロファイルは「MSX2++の音源構成を模した」もので、新仕様の1回路へ合わせるべきかは未決
- HWプラグインの部位ごとのゲイン(2026年10月)の残り: (1) GUIの目視確認と、実物のFitomEmuIF+追従済みエンジンでの音出し確認が未実施(上の実装状況の表を参照)。(2) `devices[]`側の記述を変えると`params`が一致しなくなり、保存済みの`part_gains`が掛からなくなる(読み込み時に警告は出る)。GUIから付け替える手段は無い。(3) `extra_slot`の2ポート目(`port2`)の部位は扱わない(1ポート目のハンドルの部位だけが対象)。(4) `fitom_cli`には値を変える手段が無い(プロファイルに保存済みの値は読み込み時に適用される)。(5) ステージングリポジトリが持つ`config_schema/profile.schema.json`の写しには`part_gains`が無い。トップレベルが`additionalProperties: false`なので、GUIで部位ゲインを保存したプロファイルは、写しを更新するまでそのスキーマの検証に通らない

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
