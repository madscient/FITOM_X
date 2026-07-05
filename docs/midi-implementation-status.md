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
| 1 | Modulation | ✅ | `LFR=0`の音色のみCC#1駆動LFOが作用。sine固定・6.25Hz固定。RPN#5でデプス変更可 |
| 2 | Breath Controller | ⚠️ | `amDepth_`に保存のみ、発音中チャンネルへの反映は限定的 |
| 4 | Foot Controller | ⚠️ | CC#2と同じ`amDepth_`を共有（既知の制限） |
| 5 | Portamento Time | ✅ | |
| 6 | Data Entry MSB | ✅ | RPN/NRPN経由でディスパッチ |
| 7 | Channel Volume | ✅ | |
| 10 | Pan | ✅ | 対応デバイス（VoicePatchType依存）でのみ効果あり。非対応デバイスは無視（ケイパビリティ判定なし、エンドユーザー責任） |
| 11 | Expression | ✅ | |
| 32 | Bank Select LSB | ✅ | CC#0の値によって意味が変わる (下記「Bank Select 方式」参照)。通常モード時=PatchBank番号、直接モード時=HwBankインデックス。リズムチャンネルでは常に無視 |
| 64 | Sustain | ✅ | チップ依存実装（後述） |
| 65 | Portamento On/Off | ✅ | **モノフォニックチャンネル専用**。ポリでは無効 |
| 66 | Sostenuto | ✅ | ペダル押下時に鳴っていたノートのみホールド（NoteOff遅延方式） |
| 67 | Soft Pedal | ⬜ 非対応（意図的） | FM音源に直接対応するパラメータがないため |
| 68 | Legato | ✅ | **モノフォニックチャンネル専用**。ポリでは無効 |
| 84 | Portamento Control (Source Note) | ✅ | one-shot、次のNoteOnのグライド開始音を明示指定 |
| 98/99 | NRPN LSB/MSB | ✅ | |
| 100/101 | RPN LSB/MSB | ✅ | |
| 120 | All Sound Off | ✅ | `forceDamp`実装。RRを最大化し急速減衰（ALGキャリア判定込み） |
| 121 | Reset All Controllers | ✅ | Sostenuto/Portamento保留状態のクリアを含む |
| 123 | All Notes Off | ✅ | 通常のnoteOff（sustain有効なら保持） |

---

## RPN

| RPN | 名称 | 状態 |
|---|---|---|
| 0x0000 | Pitch Bend Range | ✅ |
| 0x0001 | Fine Tuning | ✅ |
| 0x0002 | Coarse Tuning | ⬜ 非対応 |
| 0x0005 | Modulation Depth Range | ✅ |
| 0x7F7F | RPN Null | ⬜ 非対応 |

---

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
| `0x70`-`0x77`, `0x7A`-`0x7F` | (将来予約) | - | - |
| `0x78` (`DEVICE_RHYTHM`) | GM2: リズムチャンネルへ切替 | 使用しない | 使用しない |
| `0x79` | GM2: メロディチャンネルへ切替 | 使用しない | 使用しない |

**`VOICE_PATCH_*`の値は全て`0x01`-`0x6F`の範囲に収まるよう採番されている**
(`FITOMdefine.h`参照)。これにより、CC#0の値だけで「通常/直接/GM2切替」の
どのモードかを一意に判別できる。`0x70`-`0x7F`は将来の拡張・GM2専用として
予約し、通常のVoicePatchType値としては使わない。

**GM2切替(`0x78`/`0x79`)は`MidiProcessor::processControl()`層で消費され、
個々のチャンネルオブジェクト(`CInstCh`/`CRhythmCh`)の`bankSelMSB()`には
一切転送されない。** そのチャンネルの役割(リズム/メロディ)自体が
そのタイミングで動的に切り替わる(内部的には`unique_ptr<IMidiCh>`の
差し替え)。詳細は`docs/gm2-dynamic-channel-switching.md`(存在する場合)
または`CFITOM.cpp`の`MidiProcessor::switchChannelRole()`を参照。

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

---

## CC#120 (All Sound Off) vs CC#123 (All Notes Off)

| CC# | 動作 |
|---|---|
| 120 | `forceDamp()`。sustainを無視し、RRを最大化（またはOPLL系はSUSビット解除）して急速減衰 |
| 123 | 通常の`noteOff()`。sustain有効なら保持される |

---

## 既知の非対応項目（今回のスコープ外として確定）

- Poly Pressure / Channel Pressure
- CC#67 Soft Pedal
- RPN 0x0002 Coarse Tuning
- RPN 0x7F7F Null
- CC#2/CC#4 の変数分離（現状共有）
