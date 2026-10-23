# MIDI処理の内部設計

**対象読者**: FITOM_X本体の開発者・AI  
**関連文書**:
- 利用者向けの仕様: `docs/manuals/midi-message-reference.md`、対応状況の一覧: `docs/manuals/midi-implementation-chart.md`
- SF2直行パス: `docs/design/sf2-direct-path.md`
- パッチの解決: `docs/design/patch-resolution.md`

この文書は、受信したMIDIメッセージがコードのどこでどう処理されるかと、その設計判断を書く。メッセージごとの利用者向けの意味は、上のマニュアルを正とする。

---

## 受信の流れ

```
MIDIバックエンドDLLのコールバック(DLLのスレッド)
  → CFITOM::receiveMpuByte(mpu, ...)        processMutex_ を取る
      → MidiProcessor::receiveByte()        ランニングステータス・SysExの組み立て
          → MidiProcessor::processMessage()
              ├ SF2直行パスの窓に含まれる(mpu,ch) → CFITOM::routeSf2ChannelMessage()
              ├ コントロールチェンジ → MidiProcessor::processControl()
              └ それ以外 → channels_[ch](IMidiCh: CInstCh / CRhythmCh)
```

- MPU(実MIDI入力ポートの単位、最大4面)ごとに`MidiProcessor`と16ch分の`IMidiCh`がある。内部用MIDIパイプは、MPUとは別の専用の`MidiProcessor`と16chを持つ(`internalPipeProcessor_`/`internalPipeChannels_`。MIDIクロックは扱わない)
- MIDI入力・タイマー・GUIからの送出(`MidiProcessor::sendControlChange()`等)は、すべて`CFITOM`の`processMutex_`で直列化される。GUIからの送出も、実MIDI入力と同じくSF2直行パスの窓を確かめてから振り分ける
- SF2直行パスの窓はMPUに対してだけ設定する。内部用MIDIパイプのメッセージは窓の判定を通らない
- 起動時、各`MidiProcessor`のch9(10チャンネル目)はリズムチャンネル(`CRhythmCh`)、それ以外はメロディチャンネル(`CInstCh`)として作られる

## コントロールチェンジの配線

`MidiProcessor::processControl()`が受け、多くは`IMidiCh`の対応するメソッドへそのまま渡す。

| CC# | 処理 | 要点 |
|---|---|---|
| 0 | `bankSelMSB()` | 0x78/0x79はここで消費してチャンネルロールを切り替え、`bankSelMSB()`には渡さない(「Bank Select 方式」節) |
| 1 | `setModulation()` | ソフトウェアLFO用。`sw.LFR=0`の音色だけに効く(「ソフトウェアLFO」節) |
| 2, 4 | `setBreathCtrl()` / `setFootCtrl()` | 空実装。操作するパラメータが無い |
| 5 | `setPortTime()` | 「レガートとポルタメント」節 |
| 6, 38, 96, 97 | Data Entry MSB/LSB、Data Increment/Decrement | 選択中のRPN/NRPNへ。Increment/Decrementの刻みはレジスタごとに違う(MSBだけ有効なパラメータは128刻み、LSBだけ有効なパラメータは1刻み等。`dataIncrement()`) |
| 7, 10, 11 | `setVolume()` / `setPanpot()` / `setExpression()` | パンはパンを持つデバイスでだけ効き、持たないデバイスでは無視される |
| 14, 15 | `setHwLfoDepth()` / `setHwLfoRate()` | 非標準。チップ内蔵LFO(「ハードウェアLFO」節) |
| 32 | `bankSelLSB()` | 意味はCC#0の値で変わる。リズムチャンネルでは無視 |
| 64 | `setSustain()` | チップごとの実装(「サステイン」節) |
| 65, 68, 84 | `setPortamento()` / `setLegato()` / `setPortamentoSource()` | モノフォニックのチャンネルでだけ効く |
| 66 | `setSostenuto()` | 「ソステヌート」節 |
| 76, 77, 78 | `setSoftLfoRate()` / `setSoftLfoDepth()` / `setSoftLfoDelay()` | GM2のSound Controller 7/8/9。ソフトウェアLFOへのオフセット |
| 98–101 | NRPN/RPNの番号の選択 | |
| 120 | `allSoundOff()` | 「CC#120とCC#123」節 |
| 121 | `resetAllCtrl()` | ボリューム・エクスプレッション・パン・ピッチベンドとそのレンジ・RPN#1・ペダル類・ポルタメントとソース指定・ハードウェアLFO・ソフトウェアLFOのオフセットを初期値に戻す。HwPatch/SwPatch/ToneLayerのオーバーライドは消さない(プログラムチェンジで消える) |
| 123 | `allNoteOff()` | 通常のnoteOff。サステイン中なら保持される |
| 126, 127 | `setMonoMode()` / `setPolyMode()` | 「モノモードとボイス数上限」節 |

これ以外のCC(CC#67 Soft Pedal、CC#122 Local Control、CC#124/125 Omni Off/On等)は無視する。

## RPN と NRPN

| 番号 | 名称 | 実装の要点 |
|---|---|---|
| RPN 0x0000 | Pitch Bend Range | |
| RPN 0x0001 | Fine Tuning | 14bitを使い、中央8192で±100セント。実効分解能はkfs(約1.5625セント)で四捨五入 |
| RPN 0x0002 | Coarse Tuning | MSBだけ有効、±64半音 |
| RPN 0x0005 | Modulation Depth Range | CC#1のLFOの最大デプス |
| RPN 0x7F7F | RPN Null | Data Entryを無効にする |
| NRPN 96,1 (0x3001) | 物理チャンネル固定 | `CInstCh::applyPhyChOverride()`。Data EntryのMSBを使う。モノフォニックかつ直接モードで有効なデバイスを選んでいるときだけ効き、指定チャンネルがそのデバイスの`getChCount()`(スパン時は合算値)の範囲内かを確かめる。新たなNRPN・プログラムチェンジ・バンクセレクトのいずれかで解除 |
| NRPN 96,2 (0x3002) | パフォーマンスバンクセレクト | `CInstCh::pendingSwBankOverride_`に置くだけ。96,3とセットで確定する |
| NRPN 96,3 (0x3003) | パフォーマンスプログラムセレクト | `swBankOverride_`/`swProgOverride_`を確定させる。`noteOn()`は`rl->swPatch`の代わりに`patchMgr_->resolveSwPatch(swBankOverride_, swProgOverride_)`を基点にする(解決できなければ`rl->swPatch`へ戻す。DrumNoteの`swBank`/`swProg`と同じ規約)。CC#76/78のオフセットはこの基点の上に掛かる。プログラムチェンジで3つとも-1に戻る |
| NRPN 24/26/28,nn (0x1800/0x1A00/0x1C00+nn) | ドラムのピッチ/レベル/パン | リズムチャンネル専用(GM2準拠)。`CRhythmCh::setNRPNRegister()`。Data EntryのMSBを使う |
| NRPN 97,0–31 (0x3080–0x309F) | ToneLayerオーバーライド | `CInstCh::applyToneLayerOverride()`。下記 |

**ToneLayerオーバーライド(NRPN 97)**: `reg-0x3080`を8で割った商がレイヤー番号(0-3)、余りがパラメータ番号(0-7: voice_patch_type/hwbank/hwprog/transpose/note_range_lo/note_range_hi/volume_offset/pan_offset)。直接モード(`bankSelM_!=0`)のチャンネルでは何もしない。パラメータ0/1は`pendingVoicePatchType`/`pendingHwBank`に置くだけで、パラメータ2(hwprog)を受けたときに、この2つと合わせて`patchMgr_->resolveDirect()`を呼び、`ToneLayerOverride::resolved`(単層の`ResolvedPatch`)を確定させる。`noteOn()`のレイヤーのループは`resolver_.layerCount()`ではなく`MAX_TONE_LAYERS`まで回し、レイヤードパッチが定義していないレイヤー番号にもオーバーライドを置けるようにしている。パラメータ3-7はそれぞれ独立したactiveフラグを持ち、有効ならToneLayer本来の値の代わりに使う。プログラムチェンジで全部消える(`clearToneLayerOverrides()`)。発音中のノートには反映せず、次のノートオンから効く。

## システムエクスクルーシブ

`MidiProcessor::processSysEx()`が受ける。

### Universal Real Time (F0 7F)

Device IDは見ず、常に自分宛として処理する。Universal Non-Realtime(0x7E)は扱わない。

| Sub-ID1 | Sub-ID2 | 名称 | 実装の要点 |
|---|---|---|---|
| 0x04 | 0x01 | Master Volume | MSBだけ使う(7bit)。`CFITOM::setMasterVolume()`は値をプロファイル設定に保存し、SF2直行パスのデバイスへ転送するだけで、FM/PSGのチップの音量には反映しない(`CSoundDevice::setMasterVolume()`は呼ばれず、`masterVolume_`も読まれていない) |
| 0x04 | 0x03 | Master Fine Tuning | 14bitを使う、±100セント |
| 0x04 | 0x04 | Master Coarse Tuning | MSBだけ有効、±64半音 |
| 0x08 | 0x08 | Scale/Octave Tuning(1byte形式) | 簡略実装(「ピッチの計算」節) |

### プライベート (F0 00 48 01 ...)

`MidiProcessor::processPrivateSysEx()`が受ける。マニュファクチャラIDの次の1バイトがsub-cmd。

| sub-cmd | 用途 | 方向 | 詳細 |
|---|---|---|---|
| 0x01 | HwPatchパラメータオーバーライド | 受信 | 下記 |
| 0x02 | SwPatchパラメータオーバーライド | 受信 | 下記 |
| 0x03 | チャンネル割り当て通知 | 送信のみ(内部用MIDIパイプ) | `docs/interfaces/plugin-midi-pipe.md` |
| 0x04 | SF2直行パスの窓の割り当て・解除 | 受信 | `docs/design/sf2-direct-path.md`「窓」 |
| 0x05 | SF2のバンク・プログラム選択 | 送信のみ(SoundFontプラグイン) | `docs/interfaces/plugin-hwif.md` |
| 0x06 | レイヤードパッチ(`Patch`)の直接編集 | 受信 | 下記 |
| 0x07 | ドラムキット(`DrumPatch`)の直接編集 | 受信 | 下記 |

**sub-cmd 0x01/0x02(HwPatch/SwPatchのオーバーライド)**

```
[0..2] 00 48 01 (呼び出し元processSysEx()で確認済み)
[3]    sub-cmd (0x01=HwPatch / 0x02=SwPatch)
[4]    target-type (0x00=チャンネル / 0x01=プリセットバンク)
[5..]  target-addr (sub-cmd・target-typeにより1/2/3byte、下記)
[layerOffset]  layer
[jsonOffset..] JSONペイロード(ASCII)
```

- `target-addr`(target-type=0x00、sub-cmd共通): `[5]`=MIDIチャンネル(0-15)
- `target-addr`(target-type=0x01、sub-cmd=0x01/HwPatch): `[5]`=VoicePatchType `[6]`=HwBankインデックス `[7]`=HwProg (3byte)
- `target-addr`(target-type=0x01、sub-cmd=0x02/SwPatch): `[5]`=SwBankインデックス `[6]`=SwProg (2byte。SwBankRegistryはVoiceGroupを持たない単一の番号空間のため、HwPatchと異なりVoicePatchTypeが不要)

JSONのフィールドマージは`PatchManager::mergeHwPatchFromJsonText()`/`mergeSwPatchFromJsonText()`(内部的にはそれぞれ匿名名前空間の`mergeHwPatchFromJson()`/`mergeSwPatchFromJson()`に委譲)で行う。既存の`jsonToHwPatch()`/SwBank読み込みロジックと異なり、まっさらなインスタンスからではなく既存の`target`へ差分マージする点が違い。`"ops"`配列は`null`/`{}`要素をスキップとして扱う(疎な指定に対応)。`id`/`name`/`builtin`はこの経路では対象外。SwPatch側のJSONキー名は`saveSwBankJson()`が出力する形式(`"sw":{...}`・`"ops":[...]`・`"fine_transpose"`)に合わせている。

target-type=0x00(チャンネル):
- HwPatch: `IMidiCh::mergeHwPatchOverride(layer, jsonText)` → `CInstCh`実装。`hwPatchOverride_[layer]`(`std::array<HwPatch, MAX_TONE_LAYERS>`)へマージし、`hwPatchOverrideActive_[layer]`を立てる。初回マージ時は`resolver_.layer(layer)->hwPatch`を起点にコピーする(2回目以降は前回のオーバーライド結果に積み上げる)。`noteOn()`で`rl->hwPatch`の代わりに使う(`patch`変数の決定ロジック)ほか、`ISoundDevice::setVoice(ch, patch, true)`で発音中の同レイヤーの全ノートへ即座に反映する。
- SwPatch: `IMidiCh::mergeSwPatchOverride(layer, jsonText)` → `CInstCh`実装。`swPatchOverride_[layer]`へマージ、初回は`resolver_.layer(layer)->swPatch`を起点にコピー。`noteOn()`の`baseSwPatch`決定チェーン(NRPN96,2/3の直後、CC#76/78のオフセットより前)に組み込む。**`ISoundDevice`側に相当するリアルタイムpush手段が無いため、発音中ノートへの即時反映は行わない(次のノートオンから反映、CC#76/78と同じ制約)。**
- 両者とも`progChange()`で全レイヤークリア。JSON本文が(空白を除いて)`"{}"`の場合は特別扱いし、通常のマージではなく該当レイヤーのオーバーライド解除として処理する。

target-type=0x01(プリセットバンク):
- HwPatch: `HwBankRegistry::findMutable()`で対象`HwBank`を取得し、`bank->patches[hwProg]`へマージする。
- SwPatch: `SwBankRegistry::findMutable()`で対象`SwBank`を取得し、`bank->patches[swProg]`へマージする。
- いずれも対象スロットが`isValid()==false`(未定義)の場合は無視する(このSysExは既存プリセットの編集専用、新規作成用途ではないため)。**メモリ上のみの変更で、ディスクへの永続化は行わない**(パッチエディタ側が別途バンクファイルへ書き込む前提)。

**sub-cmd 0x06/0x07(レイヤードパッチ・ドラムキットの直接編集)**

外部パッチエディタがバンクファイルへの保存と対にして送る、編集結果をメモリ上のバンクへ確定させるためのメッセージ。sub-cmd 0x01/0x02の`target-type=0x01`と同じ性質だが、対象が`Patch`/`DrumPatch`なので`target-type`/`layer`の枠組みは使わない(この単位のチャンネルスコープの一時上書きは、NRPN 97とドラム用NRPNが担う)。

```
[0..2] 00 48 01
[3]    sub-cmd (0x06=Patch / 0x07=DrumPatch)
[4]    バンク番号 (sub-cmd=0x06: PatchBank番号 / sub-cmd=0x07: DrumBank番号)
[5]    prog番号
[6..]  JSONペイロード(ASCII)
```

JSONのフィールドマージは`PatchManager::mergePatchFromJsonText()`/`mergeDrumPatchFromJsonText()`(内部的にはそれぞれ匿名名前空間の`mergePatchFromJson()`/`mergeDrumPatchFromJson()`に委譲)で行う。既存プリセットの編集専用で、対象スロットが`isValid()==false`の場合は無視する点、メモリ上のみの変更でディスクへは保存しない点はHwPatch/SwPatchと同じ。

- sub-cmd=0x06(Patch): `PatchManager::findMutablePatchBank()`で対象`PatchBank`を取得し、`bank->patches[prog]`へマージする。JSONフィールドは`name`/`poly`/`layers`(0-4要素の可変長配列、`patchbank.schema.json`の`layers[]`と同じキー名で各`ToneLayer`へ差分マージ)。`layers[]`の各要素は3通りの意味を持つ: `null`または`{}`=そのインデックスは変更しない、文字列`"remove"`=そのレイヤーを削除する(`ToneLayer`をデフォルト値へリセットし、`enabled=false`で事実上無音にする)、オブジェクト=既存の`ToneLayer`へ差分マージ。
- sub-cmd=0x07(DrumPatch): `DrumBankRegistry::findMutable()`で対象`DrumPatchBank`を取得し、`bank->patches[prog]`へマージする。JSONフィールドは`name`/`choke_groups`(指定時は全置換)/`notes`(オブジェクト、キー=MIDIノート番号の文字列"0"-"127"、値は`drumkit.schema.json`の`notes[]`要素と同じキー名で各`DrumNote`へ差分マージ)。`notes{}`の各値は`layers[]`と同じ3通りの意味を持つ。

## ピッチの計算

**マスターチューニング**: `CFITOM::setMasterPitch()`(プロファイル設定の絶対Hz)とは別に、SysEx由来の相対オフセット(`masterFineTuneCents_`/`masterCoarseTuneSemitones_`)を持つ。`FnumRegistry`へ反映する実効ピッチは、両者を合成した`基準Hz × 2^((fineCents/100 + coarseSemitones)/12)`(430〜450Hzにクランプ)で、プロファイル設定の基準Hz自体は上書きしない。

**Scale/Octave Tuning(1byte形式)**: 本来は`F0 7F <devID> 08 08 <channel mask> <xx1>...<xx12> F7`のチャンネルマスクで対象chを絞れるが、**チャンネルマスクを読み飛ばし、常に全チャンネルへ適用する**簡略実装。12半音ごとのセントのオフセットは`CFITOM::scaleTuning_`にグローバルに持ち、各`CInstCh`の`scaleTuningKfs()`が「実際に鳴っているノート」(`NoteHist::devNote`。`ToneLayer.transpose`と`SwPatch.fineTranspose`の半音成分を適用済み)のmod 12で引く。`CRhythmCh`には適用しない。2byte形式は扱わない。

**チャンネルのピッチオフセット**: `CInstCh`が`setNoteFine()`へ渡すfine値(kfs単位)は、`noteOn()`・`applyPitchBendToAll()`・`timerCallback()`のポルタメント更新の3経路すべてで同じ内訳で計算する。経路ごとに式が食い違うと、「発音直後」「その後CC/RPNを受けた後」「ポルタメント中」でピッチが変わってしまうため、共通部分を`CInstCh::commonFineKfs()`に集約している。

| 成分 | 実装 | 単位 |
|---|---|---|
| ピッチベンド × RPN#0センシティビティ | `commonFineKfs()` | kfs |
| RPN#1 チャンネルファインチューニング(14bit) | `commonFineKfs()` | kfs |
| RPN#2 チャンネルコースチューニング(MSBのみ) | `commonFineKfs()` | kfs |
| Scale/Octave Tuning(ノート別) | `scaleTuningKfs(devNote)` | kfs |
| `SwPatch.fineTranspose`の半音未満の端数 | `NoteHist::swFineKfs` | kfs |

`setNoteFine()`へ渡すノート番号は、受信したMIDIノート(`NoteHist::note`、NoteOffの一致判定用)ではなく、必ず`NoteHist::devNote`(トランスポーズ適用後)を使う。

## Bank Select 方式

CC#0(MSB)の値が、モードを排他的に切り替える選択子になっている。

| CC#0の値 | モード | CC#32の意味 | Program Changeの意味 |
|---|---|---|---|
| `0x00` | 通常モード | PatchBank番号 | PatchBank内のPatch番号 |
| `0x01`-`0x6F` | 直接モード(この値がVoicePatchType) | そのVoicePatchType用のHwBankのインデックス | HwBank内のHwProg番号 |
| `0x70` | 内蔵リズム音源専用バンク | 対象チップ(`VOICE_PATCH_OPN2`=OPNA、`VOICE_PATCH_OPLL`=OPLL等) | 楽器(=内蔵リズムのチャンネル番号) |
| `0x78` (`DEVICE_RHYTHM`) | GM2: リズムチャンネルへ切替 | 使わない | ドラムキット(固定のドラムバンク0) |
| `0x79` | GM2: メロディチャンネルへ切替 | — | — |
| `0x7F` (`VOICE_PATCH_SILENCE`) | 無音バンク | 使わない | 使わない |
| `0x71`-`0x77`, `0x7A`-`0x7E` | 予約 | — | — |

- **`VOICE_PATCH_*`の値は全て`0x01`-`0x6F`の範囲に収まるよう採番されている**(`FITOMdefine.h`)。これにより、CC#0の値だけでモードを一意に判別できる
- 内蔵リズム音源専用バンクの解決は`docs/design/patch-resolution.md`の「内蔵リズム音源専用バンク」節
- **リズムチャンネル(`CRhythmCh`)はCC#0(`0x79`によるモード復帰を除く)/CC#32を一切使わない。** `bankSelMSB()`/`bankSelLSB()`は共にno-op。ドラムバンクは固定バンク番号(0)だけを使い、Program Changeがその中のドラムキット(`DrumPatch`)を選ぶ
- **全モード共通で、Program Change は発音中のノートに作用しない。** 新しいパッチは次の NoteOn から適用され、既存の発音はそのまま元の音色で鳴り続ける(強制NoteOffもしない)
- 無効なバンク/プログラム番号を受けた場合は、直前まで有効だったパッチを保つ(チャンネルが無音になるのを防ぐ)

### 無音バンク (`VOICE_PATCH_SILENCE` = `0x7F`)

`PatchManager::resolveTriple()`の入口で、`hwBank`/`hwProg`もデバイスリストも
一切参照せずに`ResolvedTriple::silent`だけを立てて返す。この結果は
「解決成功・ただし発音しない」という第3の状態であり、`isValid()`
(=デバイスが決まったか)とは独立している。

- `resolve()`(レイヤードパッチ): そのレイヤーだけを警告なしでスキップする。
  他のレイヤーは影響を受けない
- `resolveDirect()`(直接モード/DrumNote): `layerCount == 0`だが`patch`が
  非nullの、**有効な**`ResolvedPatch`を返す

後者が要点で、無効な`ResolvedPatch`を返してしまうと`CInstCh::progChange()`が
「直前のパッチを維持」する経路に入り、無音にならない。発音側
(`CInstCh::noteOn()`)は`layerCount == 0`で即returnするため、レイヤーが
1本も無いこと自体が無音を意味する。

チップ種別に依存しないため、チップ族ごとに無音用の`HwPatch`を用意する
必要がない。パッチ定義(`ToneLayer`/`DrumNote`)のプレースホルダとしても
同じ値を使う。

**SF2直行パスとの関係**: 外部のMIDIシーケンサー向けインストゥルメント
定義ファイルには、SF2のバンク名/パッチ名をCC#0/CC#32空間の中で
表現するためにCC#0=`0x7F`を使う流儀がある。これはあくまで定義ファイル
側の便宜的な割り当てで、FITOM_X側の実装は持たない。
**FITOM_Xがこの値を受信してもSF2直行パスへは切り替わらない**——
SF2直行パスとネイティブ経路の振り分けはMPU×chの窓テーブルへの所属
だけで決まる(`docs/design/sf2-direct-path.md`の「窓」参照)。窓に属する
チャンネルではCC#0はfluidsynthへ転送されないため、この値が無音バンクと
解釈されることもない(バンク選択は実際にはCC#32→`sf2_banks`のみで
行われる)。窓に属さないチャンネルで受信した場合は上記の無音バンクと
して扱われ、そのチャンネルは無音になる。

### GM2 リズム/メロディ切替 (CC#0=0x78/0x79)

CC#0=0x78/0x79は`MidiProcessor::processControl()`層で消費され、個々の
チャンネルオブジェクト(`CInstCh`/`CRhythmCh`)の`bankSelMSB()`には一切
転送されない。実際の切替は、そのチャンネルスロットの`unique_ptr<IMidiCh>`
を、**全く新しいオブジェクトに差し替える**ことで実現される
(`MidiProcessor::switchChannelRole()`)。新しいオブジェクトは全メンバー
変数がデフォルト値(GM標準: volume=100, expression=127, pan=64(センター)
等)を持つため、実質的に「そのチャンネルの完全な再初期化」となる。

切替が発生しない場合(既に同じロールの場合)は、オブジェクトの差し替え
自体が起こらないため、既存の状態(選択中のバンク/プログラム/その他の
コントローラー値)が一切変化しない。

| 現在の状態 | 受信メッセージ | 挙動 |
|---|---|---|
| メロディチャンネル | CC#0=0x78 | **再初期化**。新しい`CRhythmCh`を生成し`progChange(0)`を呼ぶ。バンク・プログラムチェンジ・その他の全コントローラーがデフォルト値にリセットされる |
| リズムチャンネル | CC#0=0x78 | **何もしない**。既に同じロールなので`switchChannelRole`は呼ばれず早期return。直前まで選択されていたドラムキット、その他のコントローラー値も維持される |
| メロディチャンネル | CC#0=0x79 | **何もしない**。既に同じロールなので早期return。直前まで選択されていたバンク、CC#32、プログラムチェンジ、その他のコントローラー値も維持される |
| リズムチャンネル | CC#0=0x79 | **再初期化**。新しい`CInstCh`を生成し`setup()`+`progChange(0)`を呼ぶ。CC#0/CC#32/プログラムチェンジ・その他の全コントローラーがデフォルト値にリセットされる |

#### GM2正式仕様との違い(意図的な設計判断)

**FITOM_XはGM2完全準拠を目指していない。** 固有の実装にする理由がない
項目については可能な範囲でGM2に揃える、という緩い参照に留めている。

GM2規格書（AMEI RP-024「3.3.1 cc#0/32：バンク・セレクト」）には
以下の記述がある:

> バンク・セレクトを受信しても，次のプログラム・チェンジの受信までは，
> 前のプログラム・チェンジの音色が有効である。
>
> チャンネル10およびチャンネル11では，バンク・セレクトの値により
> メロディ・チャンネルとリズム・チャンネルの切り替えを行う。バンク
> 78H/00Hが指定され，その後プログラム・チェンジを受けるとリズム・
> チャンネルとなる。また，バンク79H/xxHが指定され，その後プログラム・
> チェンジを受けることでメロディ・チャンネルとなる。

つまり**GM2の正式仕様では、チャンネルロールの切替はBank Select受信の
瞬間ではなく、その後のProgram Change受信時点で発生する2段階方式**
である。

**FITOM_Xは、この2段階方式を採用せず、CC#0=0x78/0x79を受信した
瞬間に即座にチャンネルロールを切り替え、内部で自動的に
`progChange(0)`を発行する、1段階の即時反映方式を採用している。**
GM2正式仕様に合わせる修正は行わない。理由:

- FITOM_Xのチャンネルロールは`unique_ptr<IMidiCh>`の実体差し替え
  (`CInstCh`⇔`CRhythmCh`)という設計であり、「ペンディング状態を
  保持したまま次のメッセージを待つ」という中間状態を持たない
  (常に「今どちらのオブジェクトか」が明確な、シンプルな状態機械)
- 実用上、シーケンサーやDAWは通常Bank SelectとProgram Changeを
  ほぼ同時に送出するため、2段階/1段階の違いが実際の演奏結果に
  与える影響は限定的

この違いは、利用者向けマニュアルにも書く。

## サステイン (CC#64)

チップごとに実装が違う。

| チップ | 実装方式 |
|---|---|
| OPN/OPN2/OPM | キャリアOPのRRを4に固定 |
| OPL/OPL3 | キャリアOPのRRを4に固定（`kFallbackRR`） |
| OPLL系 | `0x20+ch`レジスタのSUSビット(bit5)を操作。ROM音色はEGパラメータを変えられないため |
| DSG (YM2163) | `0x88+ch`レジスタのSUSビット(bit4)を操作。EGパラメータを持たないため、OPLLと同じくSUSビットだけで制御する |
| PSG系 | ソフトウェアエンベロープの`updateSustain`は空実装 |

RRを変える方式では、ALGに基づくキャリアOP判定で、モジュレータOPには適用しない。

## ソステヌート (CC#66)

「NoteOff遅延保持」方式で実装している(チップドライバの変更は不要)。

```
ペダルON:  現在発音中の全ノートに sostenutoHeld=true をスナップショット
NoteOff:   sostenutoHeld なノートは実際の noteOff() を呼ばず
           pendingRelease=true を立てるだけ（notes_[]のエントリは残す）
ペダルOFF: pendingRelease なノートのみ実際に noteOff()
           （まだ鍵盤が押されたままのノートは sostenutoHeld を落として発音継続）
```

解放するときは、次節のとおりデバイスチャンネルの所有権を確かめる。

## デバイスチャンネルの所有権

デバイス側のチャンネルはMIDIチャンネルをまたいで共有される(同一チップに複数MIDIチャンネルの音色を割り当てた場合等)。`findBestCh()`の強制奪取(allowSteal、score=1)は、元のownerに通知せずにチャンネルを奪う。このため、`notes_[]`/`noteSlots_[]`のエントリを持ったまま後から`noteOff()`する経路は、すべて`ISoundDevice::isChOwnedBy(devCh, this)`でそのチャンネルがまだ自分の発音のままかを確かめてから解放する。確かめずに呼ぶと、既に別のMIDIチャンネルや別のノートに奪われたチャンネルの発音を誤って止める。

該当する経路: `CInstCh::noteOff()`/`allNoteOff()`/`allSoundOff()`/`stealOldestNoteIfNeeded()`/モノフォニックの同一レイヤー内スティール、ソステヌートの`releaseSostenutoNotes()`、`CRhythmCh::NoteSlots::stopAll()`。

## ノートオフとチャンネルの再利用の規則

- **`CInstCh::noteOff()`は、ノート番号が一致するエントリを全レイヤー分解放する。** `notes_[]`はレイヤー単位のエントリで、レイヤードパッチでは同じノート番号のエントリが複数ある。最初の1件で打ち切ると、残りのレイヤーのチャンネルが鳴り続ける
- **レガート中でも`noteOff()`は通常どおり処理する。** レガートで前の音を奪うときは、`noteOn()`の`mono_ && timbres_>0`分岐が`leaveNote()`/`enterNote()`で`notes_[]`のノート番号を新しいノートへ差し替えているので、奪われた古いノート番号のnoteOffは一致判定で自然に無視される
- **Running中のチャンネルを奪うときは、ownerが自分かどうかに関わらず必ず先に`noteOff()`する**(`CSoundDevice::assignCh()`)。OPN系等のKey On/Offはエッジトリガなので、Key Offを挟まずにKey Onを書くとエンベロープが再アタックしない
- **`wasReleasing`は`ChState::assign()`で確定させる。** `assign()`がstatusをAssignedに書き換えるより前に判定する。`assign()`を経由しない経路(モノの自己スティール等)では、従来どおり`run()`が判定する。チップドライバのリリース中再トリガー対策(`updateKey()`の`if (keyOn && wasReleasing)`分岐)はこの値で発動する

## レガートとポルタメント (CC#68 / CC#65)

**両方ともモノフォニックチャンネル専用。ポリフォニックチャンネルでは無反応。**

Portamentoは `mono_ == true` かつ有効時のみ動作する。ポリで発動すると
発音中の全ノートに同じグライドが適用され和音が崩れるため、明示的にゲートしている。

グライド元ノートの扱い:
- CC#84で明示指定があればそれを使用（one-shot）
- なければ同一レイヤーで直前に鳴っていたノートから連続的に滑る
  （`PortaCtrl::current_` が前回グライドの実到達点を保持するため、
  途中で次のノートが来ても自然に連続する）
- このチャンネルで最初のノート（グライド元がない）はグライドなしで即座に発音

**レートカーブ**: cc#5(Portamento Time)の値から実際のグライド速度への変換は、
GM2規格書の「Portamento Rate」グラフ（Y軸: Pitch increment speed[cent/msec]、
対数スケール／X軸: cc#5 0-127、線形スケール）を区分指数関数で近似し、
128要素のテーブル`kPortaSpeedTable`(`MidiCh.cpp`)として実装している。
符号によって2つの速度域を切り替えるエンコード方式:
- 正の値: 1tick(既定1ms)に`delta`ステップ進む(高速域)
- 負の値: `-delta`ティックに1ステップ進む(低速域)
- 1ステップ = 1kfs (`PortaCtrl::fine_`(0-63)が半音未満の端数を保持し、
  `current_`(半音)と合成して滑らかに遷移する)

GM2のグラフとは全域で概ね一致する。cc5=64-88付近(1tickあたり±1-2ステップしか取れない領域)でだけ最大約27%の量子化誤差があるが、聴感上の影響は小さいとして許容している。

## CC#120 と CC#123

| CC# | 動作 |
|---|---|
| 120 | `forceDamp()`。サステインを無視し、RRを最大化(OPLL系はSUSビット解除)して急速に減衰させる。ALGでキャリアを判定する |
| 123 | 通常の`noteOff()`。サステイン中なら保持される |

リズムチャンネル(`CRhythmCh`)も`allSoundOff()`を持つ。ADPCM-A(`CAdPcm2610A`)とOPNA内蔵リズム(`COPNARhythm`)は`updateKey(ch,false)`が意図的にno-opでNoteOffでは消音しないため、`forceDamp()`でダンプビットを書く。詳細は`docs/design/patch-resolution.md`の「チョークグループ」節。

## モノモードとボイス数上限 (CC#126 / CC#127)

CC#126の値M(`CInstCh::setMonoMode()`)による分岐:
- **M=1**: `mono_=true`。モノ専用の経路(`noteOn()`内の`mono_ && timbres_>0`分岐、同一レイヤーの前ノートを奪う)を使う。レガート(CC#68)・ポルタメント(CC#5/#65/#84)はこのモードでだけ働く
- **M=0 または M≥2**: `mono_`はfalseのまま(レガート/ポルタメントとの整合性のため)。代わりに`voiceLimit_`(ボイス数上限)を設定する。M=0はパッチの自動算出値(`poly_`、デバイスのチャンネル数から算出)、M≥2はその値(MAX_NOTES=16でクランプ)を使う

ボイス数上限のスティール(`CInstCh::stealOldestNoteIfNeeded()`、`noteOn()`の冒頭で呼ぶ):
- `notes_[]`はレイヤー単位のエントリのため、`NoteHist::seq`(NoteOnイベントごとに採番する単調増加カウンタ、同一イベントの全レイヤーは同じ値)を使ってノート番号ごとにユニークな1ボイスとして数え直す
- 保持中のボイス数が上限に達していれば、seqが最小(発音順が最も古い)ノートを選び、そのノートが使う全レイヤーをまとめて解放する

`voiceLimitOverride_`フラグにより、CC#126で明示した上限は、プログラムチェンジによる`poly_`の再計算の影響を受けない(MIDI規格上、チャンネルモードメッセージはプログラムチェンジで暗黙にリセットされないため)。CC#127で解除され、`poly_`に戻る。

## ハードウェアLFO (CC#14 / CC#15)

OPM/OPZ(`COPM`/`COPZ`)だけが実装し、他のチップは基底クラスのno-op。チップ内蔵LFOのRate/Depthレジスタ(0x18/0x19)はチップ全体で1系統の共有資源だが、チャンネルごとのAMS/PMS感度(reg 0x38+ch)は独立している。

- `setLFODepth`/`setLFORate`はAM/PMの区別を持たない。AM/PMどちらとして効くかは、ボイス自身のAMS/PMSで決まる
- 複数のチャンネルが同時にAMS/PMSを使うのは正当なので、チャンネル間の排他はしない
- `enablePM`/`enableAM`は`noteOn()`で無条件にtrueを送る。AMS/PMS=0のボイスでは結果的に効果が無い
- Depth/Rateは物理チップ単位でしか持てないので、同じチップを共有する複数のチャンネルが別々の値を送ると、最後に送った値が勝つ(実機の制約そのもの)
- `CSpanDevice`/`CUnison`は`enablePM`/`enableAM`/`setLFODepth`/`setLFORate`を各チップへ転送する(`SPAN_DELEGATE`/`UNISON_BROADCAST`)
- OPZの第2LFOには対応していない。対応の設計検討は`docs/proposals/opm-opz-hw-lfo.md`

## ソフトウェアLFO (CC#1 / CC#76 / CC#77 / CC#78)

**CC#1**: `sw.LFR=0`(音色がLFOを持たない)の音色にだけ効くビブラート。波形はsine、周期は`kCC1DefaultRate`(rate=70、約6.23Hz)、最大デプスはRPN#5で変えられる(`VoiceProcessor`)。`sw.LFR>0`の音色では、音色自身のLFOが優先されCC#1は無視される。

**CC#76/77/78**: GM2のSound Controllerに合わせ、値64を「補正なし」とするオフセット方式。

`VoiceProcessor::onNoteOn()`は`ISoundDevice::assignCh()`の内部で、呼び出し元(`CInstCh`)が`devCh`を受け取るより前に呼ばれる。このため、Rate/Delay(LFOを始めるときにしか意味を持たないパラメータ)は、ch単位で後からpushする方式では最初のノートに間に合わない。

- **Rate(CC#76)/Delay(CC#78)**: `CInstCh::noteOn()`が`SwPatch`の一時コピー(`overriddenSw`)を作り、`sw.LFR`/`sw.LFD`に(受信値-64)を加えて0〜127に収めてから`assignCh`/`allocCh`へ渡す。元の`rl->swPatch`(共有パッチデータ)は書き換えない。次のノートオンから効く
- **Depth(CC#77)**: (受信値-64)を±1200セント相当のオフセット(`lfoDepthOverrideCents_`)に換算する。`VoiceProcessor::recalcChLfo()`が毎tick`sw.depthCents`にこのオフセットを加えて計算し直すので、`ISoundDevice::setLfoDepthOverride()`で発音中のノートにもすぐ反映できる

**デバイスチャンネルに値を残さない**: CC#1の値とCC#77のオフセットはデバイスチャンネル側(`VoiceProcessor`)に持つので、前にそのチャンネルを使っていた別のMIDIチャンネルの値が残りうる。これを防ぐため、`CInstCh::noteOn()`は毎ノートオンで現在の値を必ずpushし、`CRhythmCh::applyNoteOn()`も毎ノートオンで`setCC1Modulation(devCh, 0, 0)`と`setLfoDepthOverride(devCh, 0)`(補正なし)をpushする。pushは`assignCh()`の直後、`noteOn()`より前(`ChState::status == Assigned`)に行うので、`CSoundDevice`側のガードは`isActive()`に加えて`isAssigned()`も通す。

## 対応しないもの

- Poly Pressure / Channel Pressure(SF2直行パスの窓では、そのまま転送する)
- CC#2(Breath Controller)/CC#4(Foot Controller): 操作するパラメータが無く空実装。ソフトウェアAM(トレモロ)相当の仕組みを作るなら、割り当ての候補
- CC#67 Soft Pedal(FM音源に対応するパラメータが無い)
- CC#122 Local Control、CC#124/125 Omni Off/On(受けても状態は変わらない。常にOmni Offとして動く)
- Scale/Octave Tuningの2byte形式、Universal Non-Realtime SysEx
- OPZの第2LFO
