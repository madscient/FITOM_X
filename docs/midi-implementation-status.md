# MIDI メッセージ実装状況

FITOM_X が実際に受信・処理する MIDI メッセージ／CC／RPN の対応表。
GM2 規格を完全網羅するものではなく、意図的に対応範囲を絞っている項目がある。

---

## チャンネルメッセージ

| メッセージ | 状態 | 備考 |
|---|---|---|
| Note On (0x90) | ✅ | vel=0 は Note Off として処理 |
| Note Off (0x80) | ✅ | Sostenuto保持時は`pendingRelease`で遅延 |
| Poly Pressure (0xA0) | ⬜ 非対応（意図的） | |
| Program Change (0xC0) | ✅ | 発音中ノートには作用しない（後述） |
| Channel Pressure (0xD0) | ⬜ 非対応（意図的） | |
| Pitch Bend (0xE0) | ✅ | Fnumテーブルステップ単位で実装（RPN#0参照） |

---

## コントロールチェンジ

| CC# | 名称 | 状態 | 備考 |
|---|---|---|---|
| 0 | Bank Select MSB | ✅ | モード選択子 (下記「Bank Select 方式」参照)。0=通常モード、0x01-0x6F=直接モード(VoicePatchType指定)、0x78/0x79=GM2リズム/メロディ切替 |
| 1 | Modulation | ✅ | ソフトウェアLFO専用(`pmDepth_`→`setCC1Modulation`)。`sw.LFR=0`の音色のみCC#1駆動LFOが作用。sine固定・6.25Hz固定。RPN#5でデプス変更可。2026年7月、ハードウェアLFO(CC#14/15)への関与を廃止して分離した |
| 2 | Breath Controller | ⬜ 非対応(2026年7月に無効化) | 旧`amDepth_`(ハードウェアAM用)を廃止したのに伴い、操作対象のパラメータが無くなったため`setBreathCtrl`は空実装にした。ソフトウェアAM(トレモロ)相当の仕組みは現状存在しない |
| 4 | Foot Controller | ⬜ 非対応(2026年7月に無効化) | CC#2と同じ理由 |
| 5 | Portamento Time | ✅ | GM2規格書の「Portamento Rate」グラフから区分指数関数で再構築した128要素テーブル(`kPortaSpeedTable`)を使用。1半音=64ステップ(100/64cent)単位で滑らかに遷移(`PortaCtrl::fine_`) |
| 6 | Data Entry MSB | ✅ | RPN/NRPN経由でディスパッチ |
| 7 | Channel Volume | ✅ | |
| 10 | Pan | ✅ | 対応デバイス（VoicePatchType依存）でのみ効果あり。非対応デバイスは無視（ケイパビリティ判定なし、エンドユーザー責任） |
| 11 | Expression | ✅ | |
| 14 | HW LFO Depth(非標準) | ✅(2026年7月新設) | `CInstCh::hwLfoDepth_`→`ISoundDevice::setLFODepth()`。OPM/OPZ(`COPM`)のみ実装、他チップは基底クラスのno-opにフォールバック。チップ内蔵LFOは1系統のみの共有リソースのため全チャンネル共通(最後の送信値が勝つ)。`CSpanDevice`/`CUnison`にも転送(`SPAN_DELEGATE`/`UNISON_BROADCAST`)を追加済み |
| 15 | HW LFO Rate(非標準) | ✅(2026年7月新設) | `CInstCh::hwLfoRate_`→`ISoundDevice::setLFORate()`。CC#14と同じ制約 |
| 32 | Bank Select LSB | ✅ | CC#0の値によって意味が変わる (下記「Bank Select 方式」参照)。通常モード時=PatchBank番号、直接モード時=HwBankインデックス。リズムチャンネルでは常に無視 |
| 64 | Sustain | ✅ | チップ依存実装（後述） |
| 65 | Portamento On/Off | ✅ | **モノフォニックチャンネル専用**。ポリでは無効 |
| 66 | Sostenuto | ✅ | ペダル押下時に鳴っていたノートのみホールド（NoteOff遅延方式） |
| 67 | Soft Pedal | ⬜ 非対応（意図的） | FM音源に直接対応するパラメータがないため |
| 68 | Legato | ✅ | **モノフォニックチャンネル専用**。ポリでは無効 |
| 76 | Sound Controller 7 / Soft LFO Rate上書き(非標準) | ✅(2026年7月新設) | `CInstCh::lfoRateOverride_`。`assignCh()`内部で`VoiceProcessor::onNoteOn()`が呼ばれるタイミングの都合上、CInstCh側で`SwPatch`の一時コピー(`overriddenSw`)へ焼き込んでから渡す方式(devCh決定後にch単位でpushする方式では最初のノートに間に合わないため)。次のノートオンから反映 |
| 77 | Sound Controller 8 / Soft LFO Depth上書き(非標準) | ✅(2026年7月新設) | `CInstCh::lfoDepthOverrideCents_`(0-127を±1200centsへ線形マッピング)。`VoiceProcessor::recalcChLfo()`が毎tick参照するため、`ISoundDevice::setLfoDepthOverride()`経由で発音中ノートにも即座に反映可能(`setCC1Modulation`と同じ配線パターン)。前の占有者(別MIDIチャンネル)の値が漏れ残らないよう、無効値(-2000)も含め毎ノートオンで必ずpushする |
| 78 | Sound Controller 9 / Soft LFO Delay上書き(非標準) | ✅(2026年7月新設) | `CInstCh::lfoDelayOverride_`。CC#76と同じSwPatch焼き込み方式 |
| 84 | Portamento Control (Source Note) | ✅ | one-shot、次のNoteOnのグライド開始音を明示指定 |
| 96 | Data Increment | ✅(2026年7月新設) | 選択中のRPN/NRPNレジスタの値を1ステップ増加。ステップ幅はレジスタごとに異なる(`dataIncrement()`実装参照。MSBのみ有効なパラメータは128刻み、LSBのみ有効なパラメータは1刻み等) |
| 97 | Data Decrement | ✅(2026年7月新設) | 同上、減少方向 |
| 98/99 | NRPN LSB/MSB | ✅ | |
| 100/101 | RPN LSB/MSB | ✅ | |
| 120 | All Sound Off | ✅ | `forceDamp`実装。RRを最大化し急速減衰（ALGキャリア判定込み） |
| 121 | Reset All Controllers | ✅ | Sostenuto/Portamento保留状態のクリア、HW/Soft LFOオーバーライドの解除を含む |
| 123 | All Notes Off | ✅ | 通常のnoteOff（sustain有効なら保持） |
| 126 | Mono Mode On(非標準の値解釈) | ✅(2026年7月新設) | 値M。M=1: 真のモノ(`mono_=true`、既存のレガート/ポルタメント経路を使用)。M=0またはM≥2: ポリのまま`voiceLimit_`(ボイス数上限)を設定し、`stealOldestNoteIfNeeded()`でノート単位の最古スティールを行う。M=0は自動算出値(`poly_`)を使用 |
| 127 | Poly Mode On | ✅(2026年7月新設) | `mono_=false`、`voiceLimit_`を`poly_`(自動算出値)に戻す |

---

## RPN

| RPN | 名称 | 状態 |
|---|---|---|
| 0x0000 | Pitch Bend Range | ✅ |
| 0x0001 | Fine Tuning | ✅ (14bit全体を使用、±100cents相当) |
| 0x0002 | Coarse Tuning | ✅ MSBのみ有効、±64semitones |
| 0x0005 | Modulation Depth Range | ✅ |
| 0x7F7F | RPN Null | ✅ Data Entry無効化 |

---

## NRPN

| NRPN | 名称 | 状態 | 備考 |
|---|---|---|---|
| 0x3001 (96,1) | 物理チャンネル固定 | ✅(2026年7月に条件付き発動へ再設計) | `CInstCh::applyPhyChOverride()`。旧実装は`val & 0x7F`(LSB由来)を参照しており、DAWがMSBのみ送る一般的な使い方では常に`phyCh_=0`に固定されるバグがあった。修正して`val >> 7`(MSB由来)に変更。加えて「モノフォニックかつ直接モードで有効なデバイスを選択中」の場合のみ発動し、指定チャンネルがそのデバイスの`getChCount()`(スパン時は合算値)の範囲内であることを検証するよう仕様変更した。新たなNRPN受信・プログラムチェンジ・バンクセレクトのいずれかで都度クリアされる |
| 0x3002 (96,2) | パフォーマンスバンクセレクト | ✅(2026年7月新設) | `CInstCh::pendingSwBankOverride_`。単体では確定しない、96,3とセットで使う一時保存用 |
| 0x3003 (96,3) | パフォーマンスプログラムセレクト | ✅(2026年7月新設) | `CInstCh::swBankOverride_`/`swProgOverride_`。受信時点の`pendingSwBankOverride_`と自身のDE(MSB)値をセットで確定させる。`noteOn()`で`rl->swPatch`の代わりに`patchMgr_->resolveSwPatch(swBankOverride_, swProgOverride_)`を基点として使う(解決失敗時は`rl->swPatch`にソフトフォールバック、DrumNote::swBank/swProgと同じ規約)。CC#76/78のRate/Delay上書きは、この基点SwPatchの上にさらに適用される。`progChange()`で3メンバとも`-1`にクリアされる(次のプログラムチェンジまで有効) |
| 0x1800/nn, 0x1A00/nn, 0x1C00/nn | Drum Instrument Pitch/TVA Level/Pan(GM2準拠、リズムチャンネル専用) | ✅(2026年7月にバグ修正) | `CRhythmCh::setNRPNRegister()`。旧実装は14bit合成値`val`をそのまま`val-64`していたため、LSB(CC#38)を送らない一般的な使い方ではオフセットが常に飽和していた(事実上機能していなかった)。`val >> 7`(MSBのみ)を使うよう修正 |
| 0x3080-0x309F (97, 0-31) | ToneLayerオーバーライド | ✅(2026年7月新設) | `CInstCh::applyToneLayerOverride()`。`reg-0x3080`を8で割った商がレイヤー番号(0-3)、余りがパラメータ番号(0-7: voice_patch_type/hwbank/hwprog/transpose/note_range_lo/note_range_hi/volume_offset/pan_offset)。`bankSelM_!=0`(直接モード)のチャンネルでは即座にreturnし何もしない。パラメータ0/1は`pendingVoicePatchType`/`pendingHwBank`に保持するのみ。パラメータ2(hwprog)受信時に、この2つの保持値と併せて`patchMgr_->resolveDirect()`を呼び、`ToneLayerOverride::resolved`(単層`ResolvedPatch`、`resolveDirect()`と全く同じ経路)を確定させる。`noteOn()`のレイヤーループは`resolver_.layerCount()`ではなく`MAX_TONE_LAYERS`まで回すよう変更し(ネイティブパッチが定義していないレイヤー番号にも新規にオーバーライドを割り当てられるようにするため)、`patchActive`なレイヤーは`resolved.layers[0]`を`rl`として使う。パラメータ3-7は個別に独立したactiveフラグを持ち、有効ならToneLayer本来の値の代わりに使う(`noteOn()`内で`effRangeLo`/`effRangeHi`/`effTranspose`/`effVolumeOffset`/`effPanOffset`として計算)。全パラメータ`progChange()`で一括クリア(`clearToneLayerOverrides()`)。既に発音中のノートへの遡及反映は行わない(次のノートオンから反映) |

---

## Universal SysEx (Universal Real Time, F0 7F)

`MidiProcessor::processSysEx()`で受信。Device ID判定は行わず常に自分宛として
処理する。Universal Non-Realtime(0x7E)は非対応。

| Sub-ID1 | Sub-ID2 | 名称 | 状態 |
|---|---|---|---|
| 0x04 | 0x01 | Master Volume | ✅ スタブ実装 (MSBのみ使用、7bit精度) |
| 0x04 | 0x03 | Master Fine Tuning | ✅ 14bit全体使用、±100cents相当 |
| 0x04 | 0x04 | Master Coarse Tuning | ✅ MSBのみ有効、±64semitones |
| 0x08 | 0x08 | Scale/Octave Tuning (1byte形式) | ✅ 簡略実装 (下記注意点参照) |
| 0x08 | 0x09 | Scale/Octave Tuning (2byte形式) | ⬜ 非対応 |

---

## プライベートSysEx (manufacturer 00H 48H 01H) — HwPatchパラメータオーバーライド

2026年7月新設。`MidiProcessor::processPrivateSysEx()`(以前はスタブのみ)に実装した。

```
[0..2] 00 48 01 (呼び出し元processSysEx()で確認済み)
[3]    sub-cmd (0x01固定)
[4]    target-type (0x00=チャンネル / 0x01=プリセットバンク)
[5..]  target-addr (target-typeにより1byteまたは3byte、下記)
[layerOffset]  layer
[jsonOffset..] JSONペイロード(ASCII)
```

- `target-addr`(target-type=0x00): `[5]`=MIDIチャンネル(0-15)
- `target-addr`(target-type=0x01): `[5]`=VoicePatchType `[6]`=HwBankインデックス `[7]`=HwProg

JSONのフィールドマージは`PatchManager::mergeHwPatchFromJsonText()`(内部的には匿名名前空間の`mergeHwPatchFromJson()`に委譲)で行う。既存の`jsonToHwPatch()`と異なり、まっさらな`HwPatch`からではなく既存の`target`へ差分マージする点が違い。`"ops"`配列は`null`/`{}`要素をスキップとして扱う(疎な指定に対応)。`id`/`name`/`builtin`はこの経路では対象外。

**target-type=0x00(チャンネル)**: `IMidiCh::mergeHwPatchOverride(layer, jsonText)` → `CInstCh`実装。`hwPatchOverride_[layer]`(`std::array<HwPatch, MAX_TONE_LAYERS>`)へマージし、`hwPatchOverrideActive_[layer]`を立てる。初回マージ時は`resolver_.layer(layer)->hwPatch`を起点にコピーする(2回目以降は前回のオーバーライド結果に積み上げる)。`noteOn()`で`rl->hwPatch`の代わりに使う(`patch`変数の決定ロジック)ほか、`ISoundDevice::setVoice(ch, patch, true)`で発音中の同レイヤーの全ノートへ即座に反映する。`progChange()`で全レイヤークリア。JSON本文が(空白を除いて)`"{}"`の場合は特別扱いし、通常のマージではなく該当レイヤーのオーバーライド解除として処理する(`hwPatchOverrideActive_[layer]=false`、発音中ノートには`rl->hwPatch`本来の値を`setVoice()`で戻す)。

**target-type=0x01(プリセットバンク)**: `HwBankRegistry::findMutable()`(2026年7月新設、既存の`find()`のconst版と対になる可変アクセサ)で対象`HwBank`を取得し、`bank->patches[hwProg]`(publicメンバへの直接参照)へマージする。対象スロットが`isValid()==false`(未定義)の場合は無視する(このSysExは既存プリセットの編集専用、新規作成用途ではないため)。**メモリ上のみの変更で、ディスクへの永続化は行わない**(将来の別機能として追加予定、今回のスコープ外)。

---

**Master Fine/Coarse Tuning の実現方式**: `CFITOM::setMasterPitch()`
(ユーザー設定の絶対Hz基準、config由来)とは別に、SysEx由来の相対オフセット
(`masterFineTuneCents_`/`masterCoarseTuneSemitones_`)を保持する。実際に
`FnumRegistry`へ反映する実効ピッチは、両者を合成した
`基準Hz × 2^((fineCents/100 + coarseSemitones)/12)`(430〜450Hzにクランプ)
であり、ユーザー設定の基準Hz自体は上書きしない。

**Scale/Octave Tuning の簡略実装について**: 本来の1byte形式は
`F0 7F <devID> 08 08 <channel mask> <xx1>...<xx12> F7`で、チャンネルマスク
により適用対象chを絞れる仕様だが、**現状はチャンネルマスクを読み飛ばし、
常に全チャンネルへ適用する**簡略実装になっている。12半音(C,C#,D...B)ごとの
centsオフセットは`CFITOM::scaleTuning_`にグローバル保持し、各`CInstCh`の
`applyPitchBendToAll()`がノート(mod 12)に応じて加算する。`CRhythmCh`(ドラム
チャンネル)には未適用。2byte形式(より高精度)は非対応。

## Sustain (CC#64) のチップ依存実装

| チップ | 実装方式 |
|---|---|
| OPN/OPN2/OPM | キャリアOPのRRを4に固定 |
| OPL/OPL3 | キャリアOPのRRを4に固定（`kFallbackRR`） |
| OPLL系 | `0x20+ch`レジスタのSUSビット(bit5)を操作。ROM音色はEGパラメータ変更不可のため |
| PSG系 | ソフトウェアエンベロープの`updateSustain`は空実装（EGなし） |

いずれもALGに基づくキャリアOP判定（`isCarrier()`）で、モジュレータOPには適用しない。

---

## Bank Select 方式 (CC#0/CC#32) — 通常モード・直接モード・GM2切替の統合設計

CC#0(MSB)の値が3種類の異なる意味を排他的に切り替える、統合されたモード選択子になっている。

| CC#0(MSB)の値 | モード | CC#32(LSB)の意味 | Prog Chg.の意味 |
|---|---|---|---|
| `0x00` | 通常モード | PatchBank番号 | 選択したPatchBank内のPatch番号 |
| `0x01`-`0x6F` | 直接モード | この値自体が`VoicePatchType`(`VOICE_PATCH_*`)。値がそのチップを直接指定する | そのVoicePatchType用にプロファイル登録された`HwBank`のインデックス |
| ~~`0x01`-`0x6F`(続き)~~ | ~~直接モード~~ | ~~上記~~ | 選択した`HwBank`内の`HwProg`番号 |
| `0x70` | 内蔵リズム音源専用バンク(OPNA/OPLL、詳細は`docs/terminology.md`参照) | - | - |
| `0x71`-`0x77`, `0x7A`-`0x7F` | (将来予約) | - | - |
| `0x78` (`DEVICE_RHYTHM`) | GM2: リズムチャンネルへ切替 | 使用しない | 使用しない |
| `0x79` | GM2: メロディチャンネルへ切替 | 使用しない | 使用しない |

**`VOICE_PATCH_*`の値は全て`0x01`-`0x6F`の範囲に収まるよう採番されている**
(`FITOMdefine.h`参照)。これにより、CC#0の値だけで「通常/直接/GM2切替」の
どのモードかを一意に判別できる。`0x70`-`0x7F`は将来の拡張・GM2専用として
予約し、通常のVoicePatchType値としては使わない。

**GM2切替(`0x78`/`0x79`)は`MidiProcessor::processControl()`層で消費され、
個々のチャンネルオブジェクト(`CInstCh`/`CRhythmCh`)の`bankSelMSB()`には
一切転送されない。** そのチャンネルの役割(リズム/メロディ)自体が
**受信の瞬間に即座に**切り替わる(内部的には`unique_ptr<IMidiCh>`の
差し替え)。

**この即時切替は、GM2正式仕様(Bank Select受信ではなく後続のProgram
Change受信時点で切り替わる、2段階方式)とは意図的に異なる設計判断
である。** FITOM_XはGM2完全準拠を目指しておらず、固有の実装にする
理由がない項目についてのみ緩くGM2に揃える方針のため、この差異は
是正しない(2026年7月時点で確定)。詳細・理由は`docs/terminology.md`の
「GM2正式仕様との違い」を参照。

**リズムチャンネル(`CRhythmCh`)はCC#0(`0x79`によるモード復帰を除く)/CC#32を
一切使わない。** `bankSelMSB()`/`bankSelLSB()`は共にno-op。ドラムバンクは
固定バンク番号(0)のみを使い、MIDI経由でのドラムバンク切替はできない。
Prog Chg.の値が、そのバンク内のドラムキット(`DrumPatch`)を選択する。


**全モード共通で、Program Change は発音中のノートに一切作用しない。**
新しいパッチは次の NoteOn から適用され、既存の発音はそのまま元の音色で
鳴り続ける（強制NoteOffもしない）。

無効なバンク/プログラム番号を受信した場合、直前まで有効だったパッチを
保持する（チャンネルが無音状態に陥ることを防ぐ）。

---

## Sostenuto (CC#66) の実装方式

「NoteOff遅延保持」方式で実装（チップドライバへの変更は一切不要）。

```
ペダルON:  現在発音中の全ノートに sostenutoHeld=true をスナップショット
NoteOff:   sostenutoHeld なノートは実際の noteOff() を呼ばず
           pendingRelease=true を立てるだけ（notes_[]のエントリは残す）
ペダルOFF: pendingRelease なノートのみ実際に noteOff()
           （まだ鍵盤が押されたままのノートは sostenutoHeld を落として発音継続）
```

ボイススティールでチャンネルが別ノートに再利用されていないか
`ISoundDevice::isChOwnedBy()` で確認してから解放する。

---

## Legato (CC#68) / Portamento (CC#65) の仕様

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
旧FITOM(`ROM::portspeed[]`)と同じ、符号によって2つの速度域を切り替える
エンコード方式を踏襲する:
- 正の値: 1tick(既定1ms)に`delta`ステップ進む(高速域)
- 負の値: `-delta`ティックに1ステップ進む(低速域)
- 1ステップ = 100/64 cent (`ChState::fineFreq`と同じ単位系。
  `PortaCtrl::fine_`(0-63)が半音未満の端数を保持し、`current_`(半音)と
  合成して滑らかな遷移を実現する)

GM2グラフとの照合により全域で概ね良好な一致を確認済み。cc5=64-88付近
(1tickあたり±1-2ステップしか取れない領域)でのみ最大約27%の量子化誤差が
あるが、聴感上の影響は小さいと判断し許容している。

---

## CC#120 (All Sound Off) vs CC#123 (All Notes Off)

| CC# | 動作 |
|---|---|
| 120 | `forceDamp()`。sustainを無視し、RRを最大化（またはOPLL系はSUSビット解除）して急速減衰 |
| 123 | 通常の`noteOff()`。sustain有効なら保持される |

---

## CC#126 (Mono Mode On) / CC#127 (Poly Mode On) とボイス数上限

2026年7月新設。旧FITOM(`MIDI.cpp`、ビルド対象外)には存在したが、現行実装(`CFITOM.cpp`/`MidiCh.cpp`)には移植されていなかった。

CC#126の値M(`CInstCh::setMonoMode()`)による分岐:
- **M=1**: `mono_=true`。既存のモノ専用経路(`noteOn()`内の`mono_ && timbres_>0`分岐、同一レイヤーの前ノートを奪う)を有効化する。レガート(CC#68)・ポルタメント(CC#5/#65/#84)はこのモードでのみ機能する
- **M=0 または M≥2**: `mono_`はfalseのまま(レガート/ポルタメントとの整合性のため)。代わりに`voiceLimit_`(ボイス数上限)を設定する。M=0はパッチの自動算出値(`poly_`、デバイスのチャンネル数から算出)、M≥2はその値(MAX_NOTES=16でクランプ)を使う

ボイス数上限のスティールアルゴリズム(`CInstCh::stealOldestNoteIfNeeded()`、`noteOn()`冒頭で呼ばれる):
- `notes_[]`はレイヤー単位のエントリのため、`NoteHist::seq`(NoteOnイベントごとに採番する単調増加カウンタ、同一イベントの全レイヤーは同じ値)を使ってnote番号ごとにユニークな1ボイスとして再集計する
- 保持中のボイス数が上限に達していれば、seqが最小(発音順が最も古い)ノートを選び、そのノートが使う全レイヤーをまとめて解放する

`voiceLimitOverride_`フラグにより、CC#126で明示指定した上限は、プログラムチェンジによる`poly_`再計算の影響を受けずに維持される(MIDI規格上、チャンネルモードメッセージはプログラムチェンジで暗黙にリセットされないため)。CC#127で解除され、`poly_`に戻る。

副次的な修正: `enterNote()`の「空きスロットなし」フォールバックが、上書きされる側のデバイスチャンネルに`noteOff()`を呼ばずデバイスチャンネルが迷子になるバグがあったため修正した(スティール導入により通常はこのパスへ到達しない)。

---

## HW LFO (CC#14/15) のリソース管理

OPM/OPZ(`COPM`/`COPZ`)のみ実装。チップ内蔵LFOはRate/Depthレジスタ(0x18/0x19)がチップ全体で1系統のみの共有リソースだが、チャンネルごとのAMS/PMS感度(reg 0x38+ch)は独立している。

- `setPMDepth`/`setAMDepth`・`setPMRate`/`setAMRate`は、AM/PMどちらとして効くかがボイス自身のAMS/PMSで決まるだけで実装上の違いが無いため、`setLFODepth`/`setLFORate`に統合した(`ISoundDevice`インターフェース変更)
- 旧`lfoOwner_`/`lfoUsed_`による排他制御(後着MIDIチャンネルが他チャンネルのAMS/PMSビットを強制クリアする)を撤廃した。複数チャンネルが同時にAMS/PMSを使うのは正当なケースであり、誤って阻害していたため
- `enablePM`/`enableAM`は`noteOn()`で無条件にtrueを送るだけにし、AMS/PMS=0のボイスでは各デバイス実装が結果的に無効果にする設計に統一した
- **既知の制限**: Depth/Rateは物理チップ単位でしか持てないため、同一チップを共有する複数チャンネルが異なるDepth/Rateを同時に要求した場合、最後に送信したCCが勝つ(実機のハードウェア制約そのもの)
- **スパニング対応**: `CMultiDevice`/`CSpanDevice`/`CUnison`が`enablePM`/`enableAM`/`setLFODepth`/`setLFORate`を一切オーバーライドしておらず、スパン構成では常に無効化されていたバグを修正した(`SPAN_DELEGATE`/`UNISON_BROADCAST`で転送)
- OPZの第2LFO系統には未対応(前述の「既知の非対応項目」参照)

## Soft LFO (CC#76/77/78) の実現方式

`VoiceProcessor::onNoteOn()`は`ISoundDevice::assignCh()`の内部で、呼び出し元(`CInstCh`)が`devCh`を受け取るより前に呼ばれる。このため、Rate/Delay(LFO再始動時にしか意味を持たないパラメータ)は、ch単位で後からpushする方式では最初のノートに間に合わない。

- **Rate(CC#76)/Delay(CC#78)**: `CInstCh::noteOn()`が`SwPatch`の一時コピー(`overriddenSw`)を作り、`sw.LFR`/`sw.LFD`を上書きした上で`assignCh`/`allocCh`へ渡す。元の`rl->swPatch`(共有パッチデータ)は直接書き換えない
- **Depth(CC#77)**: `VoiceProcessor::recalcChLfo()`が毎tick再計算するため、`lfoDepthOverrideCents_`メンバへの上書きが即座に反映される。`ISoundDevice::setLfoDepthOverride()`(`setCC1Modulation`と同じ配線パターン)経由で発音中の全ノートへpush可能
- 前の占有者(別MIDIチャンネル)のDepthオーバーライドがデバイスチャンネルに残留しないよう、無効値(-2000)も含め毎ノートオンで必ず`setLfoDepthOverride`を送る

---

## 既知の非対応項目（今回のスコープ外として確定）

- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal
- CC#2(Breath Controller)/CC#4(Foot Controller): 2026年7月にハードウェアAM用の`amDepth_`を廃止して以降、対応するパラメータが無く無効化したまま。ソフトウェアAM(トレモロ)相当の仕組みを新設する場合の割り当て候補
- CC#124/125(Omni Off/On): 受信しても状態変化なし。常にOmni Offとして動作
- CC#122(Local Control): 非対応
- OPZの第2LFO系統: `COPZ`は`COPM`を継承しているのみで、実機が持つとされる2系統目のLFOには未対応(2026年7月時点でレジスタ仕様が未確認のため保留)
