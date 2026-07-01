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
| 0 | Bank Select MSB | ✅ | 通常モード: PatchBank番号 / 直接モード: HwBank番号 |
| 1 | Modulation | ✅ | `LFR=0`の音色のみCC#1駆動LFOが作用。sine固定・6.25Hz固定。RPN#5でデプス変更可 |
| 2 | Breath Controller | ⚠️ | `amDepth_`に保存のみ、発音中チャンネルへの反映は限定的 |
| 4 | Foot Controller | ⚠️ | CC#2と同じ`amDepth_`を共有（既知の制限） |
| 5 | Portamento Time | ✅ | |
| 6 | Data Entry MSB | ✅ | RPN/NRPN経由でディスパッチ |
| 7 | Channel Volume | ✅ | |
| 10 | Pan | ✅ | 対応デバイス（VoicePatchType依存）でのみ効果あり。非対応デバイスは無視（ケイパビリティ判定なし、エンドユーザー責任） |
| 11 | Expression | ✅ | |
| 32 | Bank Select LSB | ✅ | 0=通常モード、>0=直接モード（VoicePatchType指定） |
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

## Program Change の仕様（重要）

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
