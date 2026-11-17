# FITOM_X 用語集

**対象読者**: FITOM_X本体の開発者・AI、利用者向けマニュアルを書く人

FITOM_Xの文書とコードで使う用語の定義。仕様の詳細は、各項目に挙げた文書に書く。

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
を付す程度で十分)。一方、kfs単位の値に"Cents"(プレーンなセント)のような
別の単位を名乗る名前を付けてはならない。

---

## パッチ関連の用語

### レイヤードパッチ (Layered Patch)

CC#0(Bank Select MSB)=0（通常モード）でCC#32が選択するPatchBankに
含まれる`Patch`。複数の`ToneLayer`（各々が独立した`voice_patch_type`/
`hw_bank`/`hw_prog`を持てる）を組み合わせて構成される、FITOM_X独自の
多層パッチ形式。

### ハードウェアパッチ / デバイスパッチ (Hardware Patch / Device Patch)

チップのレジスタに書く音色パラメータ(`HwPatch`)。HwBankに入っている。
レイヤードパッチの各`ToneLayer`から参照されるほか、**直接モード**(CC#0の値で
チップ種別を指定し、PatchBank/ToneLayerを経由せず単層で鳴らす選び方)で
直接選ぶこともできる。

CC#0・CC#32・Program Changeの割り当ては`docs/design/midi-processing.md`の
「Bank Select 方式」節、直接モードの解決の流れは`docs/design/patch-resolution.md`の
「直接モード（CC#0=0x01-0x6F）」節を参照。

### ソフトウェアパッチ / パフォーマンスパッチ (Software Patch / Performance Patch)

`HwPatch`が持つ`sw_bank`/`sw_prog`(-1=参照なし)で選択される
`SwPatch`。ベロシティ感度カーブ、ソフトウェアLFOの変調パラメータ、
固定トランスポーズなど、「演奏表現」に関わるソフトウェア的な
パラメータ群を保持する。参照をHwPatchが持つので、レイヤードパッチの
レイヤーごと、直接モード、内蔵リズム音源など、HwPatchが発音される
すべての経路で同じように効く。
DrumNote側にも`sw_bank`/`sw_prog`の個別上書きフィールドがあり、
HwPatchが楽器を区別できないビルトインリズム音源でも、楽器ごとに
異なるパフォーマンスパッチを指定できる。詳細は
`docs/design/patch-resolution.md`参照。

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

### チャンネルロール (メロディチャンネル / リズムチャンネル)

MIDIチャンネルごとの役割。メロディチャンネルはバンクとプログラムで音色を
選び(`CInstCh`)、リズムチャンネルはドラムキットのドラムマップで
ノートごとに音色を選ぶ(`CRhythmCh`)。CC#0=0x78/0x79で切り替える。
詳細は`docs/design/midi-processing.md`の「GM2 リズム/メロディ切替」節を参照。

---

## 特別なバンク選択値

### 無音バンク (CC#0=0x7F)

「解決は成功したが発音しない」ことを表す、チップ種別に依存しないバンク選択値。
存在しないバンク/プログラムの指定(**解決失敗**で、直前の音色が残る)とは別物で、
確実に音を止めたい箇所や`ToneLayer`/`DrumNote`のプレースホルダにはこちらを使う。
実装は`docs/design/midi-processing.md`の「無音バンク」節を参照。

### 内蔵リズム音源専用バンク (CC#0=0x70)

ROM固定のドラム音を鳴らす内蔵リズムユニットを、CC#32で対象のチップを、
Program Changeで楽器(=内蔵リズムのチャンネル番号)を選んで鳴らすための
バンク選択値。対象チップと解決の流れは`docs/design/patch-resolution.md`の
「内蔵リズム音源専用バンク」節、OPL系の内蔵リズムがこの経路を使わないことは
同じ文書の「OPL系の内蔵リズム」節を参照。

---

## SF2直行パスと窓

一部のMIDIチャンネルを、FITOM_XのFM/PSG向けの処理を通さずにSoundFont音源へ
送る経路を**SF2直行パス**と呼ぶ。どのMPU×chをこの経路に送るかの対応表を
**窓**と呼ぶ。詳細は`docs/design/sf2-direct-path.md`を参照。
