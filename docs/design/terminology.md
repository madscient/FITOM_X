# FITOM_X 用語集

このドキュメントは、FITOM_Xプロジェクトの開発セッションおよび将来の
エンドユーザー向けマニュアル作成の両方から参照される、独立した用語集。
各用語の定義は実装コードと照合済み。

---

## 単位: kfs (KF Step)

FITOM_Xのコード内部で広く使われる「1半音 = 64ステップ(=100/64セント
≈1.5625セント/ステップ)」という単位を、**kfs (KF Step)** と呼ぶ。
OPM(YM2151)のKF(Key Fraction)レジスタの分解能に由来する。

`ChState::fineFreq`、`ISoundDevice::setNoteFine()`の`fine`引数、
`getFnumber()`の`offset`引数、`PortaCtrl`(ポルタメント)の内部状態
(`fine_`、`kPortaSpeedTable`)など、ノート番号からのピッチオフセットを
扱う箇所の多くがこの単位を使う。

**既存の変数名は、単位を偽っていない限りリネームしない**(例:
`fineFreq`/`fine`は単に「微細な値」を意味するだけで、誤った単位を
主張していないため、そのままでよい。宣言部にkfs単位である旨のコメント
を付す程度で十分)。一方、`bendCents`/`tuneCents`のように、実際には
kfs単位の値を**誤って**"Cents"(プレーンなセント)と命名していた変数は、
実体に合わせて`bendKfs`/`tuneKfs`にリネーム済み(2026年7月)。

---

## パッチ関連の用語

### レイヤードパッチ (Layered Patch)

CC#0(Bank Select MSB)=0（通常モード）でCC#32が選択するPatchBankに
含まれる`Patch`。複数の`ToneLayer`（各々が独立した`voice_patch_type`/
`hw_bank`/`hw_prog`を持てる）、`SwPatch`（ベロシティ感度・LFO変調）を
組み合わせて構成される、FITOM_X独自の多層パッチ形式。

### ハードウェアパッチ / デバイスパッチ (Hardware Patch / Device Patch)

CC#0=**0x01-0x6F**（直接モード）で選択されるHwBankに含まれる`HwPatch`。
CC#0の値そのものが`VOICE_PATCH_*`（対象チップ種別）を直接指定し、
CC#32がそのチップ種別用に登録されたHwBankのインデックスを、Program
Changeがそのバンク内のプログラム番号を選択する。

**`0x70`-`0x7F`は将来の拡張用に予約されており、現状は`0x01`-`0x6F`のみ
有効**（`0x78`/`0x79`はGM2のリズム/メロディ切替専用、後述）。

PatchBank/ToneLayerを一切経由しない、単層のパッチ。**ベロシティ感度・
ソフトLFO変調(SwPatch)は、HwPatch自身が`sw_bank`/`sw_prog`を持って
いれば適用される**（2026年7月〜。以前は直接モードでは常に無効
だったが、SwPatch参照がPatch単位からHwPatch単位に変更されたことで、
直接モードでも効くようになった）。

### ソフトウェアパッチ / パフォーマンスパッチ (Software Patch / Performance Patch)

`HwPatch`が持つ`sw_bank`/`sw_prog`(-1=参照なし)で選択される
`SwPatch`。ベロシティ感度カーブ、CC#1駆動LFOの変調パラメータ、
固定トランスポーズなど、「演奏表現」に関わるソフトウェア的な
パラメータ群を保持する。**2026年7月〜、SwPatchへの参照はHwPatch
自身が持つ設計に変更された**（以前はレイヤードパッチ(`Patch`)単位で
1つだけ、全ToneLayerが共有していたが、この変更によりレイヤーごとに
異なるSwPatchを持てるようになり、また直接モードやビルトインリズム
音源等、HwPatchが実際に発音される全ての経路で一貫して適用可能になった。
DrumNote側にも`sw_bank`/`sw_prog`の個別上書きフィールドがあり、
HwPatchが楽器を区別できないビルトインリズム音源でも、楽器ごとに
異なるパフォーマンスパッチを指定できる。詳細は
`docs/design/patch-resolution.md`参照）。

---

## ドラム関連の用語

### ドラムバンク (Drum Bank)

リズムチャンネル（後述）が常に参照する、**固定バンク番号(0)のバンク**。

**注意**: 「CC#0=0x78で選択されるバンク」ではない。CC#0=0x78は
「そのMIDIチャンネル自体をリズムチャンネルに切り替える」という
**チャンネルロールの切替トリガー**であり、バンク選択の役割は持たない。
リズムチャンネルになった後は、CC#32によるバンク選択も一切無視され、
常に固定バンク番号(0)のドラムバンクのみが参照される。

### ドラムキット (Drum Kit)

ドラムバンクに含まれる`DrumPatch`（1プログラム分のドラムマップ、
`*.drumkit.json`で定義）。リズムチャンネルのProgram Changeで選択される。

`*.drumkit.json`は`"type"`フィールドで2種類の記述形式を持てる:
- `"routed"`: ノートごとに任意のPatch(bank/prog)へ個別にルーティング
  する、従来型の`notes[]`配列形式。
- `"direct"`: 単一のPatch(bank/prog)への圧縮パススルー形式
  (OPL4 AWM等、チップ自身が内蔵のキーゾーン切り替えを持つ場合に使う)。

---

## 無音バンク (CC#0=0x7F)

`PatchManager::resolveTriple()`が、`hwBank`/`hwProg`もデバイスリストも
一切参照せずに「解決成功・ただし発音しない」を返す唯一の経路。
チップ種別に依存しないため、チップ族ごとに無音用の`HwPatch`を
用意しなくてよい。

「存在しないバンク/プログラムを指定する」こととは明確に区別される。
そちらは**解決失敗**であり、`CInstCh::progChange()`が直前のパッチを
維持するため無音にならない。確実に音を止めたい箇所、および
`ToneLayer`/`DrumNote`のプレースホルダには、未定義の番号ではなく
この値を使う。実装上の要点は
`docs/design/midi-processing.md`の「無音バンク」節を参照。

## 内蔵リズム音源専用バンク (CC#0=0x70)

OPNA(YM2608)/OPLL(YM2413)は、通常のFM/PSGチャンネルとは別に、専用の
固定ドラム音を持つ内蔵リズムユニットを持つ(`COPNARhythm`/
`COPLLRhythm`)。これらのデバイスは`deviceTypeToVoicePatchType()`が
`VOICE_PATCH_NONE`を返す特殊デバイスであり、通常のVoicePatchTypeベース
ルーティング(`resolve()`/`resolveDirect()`)では到達できない。

CC#0=`0x70`は、この内蔵リズムユニットへの専用アクセス経路として予約
されている。
