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

### ネイティブパッチ (Native Patch)

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
自身が持つ設計に変更された**（以前はネイティブパッチ(`Patch`)単位で
1つだけ、全ToneLayerが共有していたが、この変更によりレイヤーごとに
異なるSwPatchを持てるようになり、また直接モードやビルトインリズム
音源等、HwPatchが実際に発音される全ての経路で一貫して適用可能になった。
DrumNote側にも`sw_bank`/`sw_prog`の個別上書きフィールドがあり、
HwPatchが楽器を区別できないビルトインリズム音源でも、楽器ごとに
異なるパフォーマンスパッチを指定できる。詳細は
`docs/patch-structure-design.md`参照）。

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

## Bank Select MSB (CC#0) の値の意味

| CC#0の値 | モード | CC#32(LSB)の意味 | Prog Chg.の意味 |
|---|---|---|---|
| `0x00` | 通常モード | PatchBank番号 | 選択したPatchBank内のPatch番号 |
| `0x01`-`0x6F` | 直接モード | この値自体が`VOICE_PATCH_*`。値がそのチップを直接指定する | そのVoicePatchType用HwBankのインデックス→そのバンク内のHwProg番号 |
| `0x70` | 内蔵リズム音源専用バンク | 対象チップ選択(`VOICE_PATCH_OPN2`=OPNA、`VOICE_PATCH_OPLL`=OPLL) | 楽器(チャンネル)番号をそのまま指定する |
| `0x71`-`0x77` | (将来予約) | - | - |
| `0x78` | GM2: リズムチャンネルへ切替 | 使用しない(無視) | ドラムキット選択(固定バンク0) |
| `0x79` | GM2: メロディチャンネルへ切替 | 使用しない | 通常モードのProgram Change |

---

## 内蔵リズム音源専用バンク (CC#0=0x70)

OPNA(YM2608)/OPLL(YM2413)は、通常のFM/PSGチャンネルとは別に、専用の
固定ドラム音を持つ内蔵リズムユニットを持つ(`COPNARhythm`/
`COPLLRhythm`)。これらのデバイスは`deviceTypeToVoicePatchType()`が
`VOICE_PATCH_NONE`を返す特殊デバイスであり、通常のVoicePatchTypeベース
ルーティング(`resolve()`/`resolveDirect()`)では到達できない。

CC#0=`0x70`は、この内蔵リズムユニットへの専用アクセス経路として予約
されている。

```
CC#0/#32 → PatchManager::resolveTriple(0x70, chipSel, hwProg, ...)
  → PatchManager::resolveBuiltinRhythm(chipSel, hwProg, ...)
      chipSel(CC#32相当): 対象チップを選ぶ。既存のVOICE_PATCH_*定数を
        再利用する (VOICE_PATCH_OPN2=OPNA、VOICE_PATCH_OPLL=OPLL)。
      hwProg(ProgChg相当): そのチップ内の楽器(=デバイスチャンネル)
        番号をそのまま指定する。範囲外の場合は解決失敗(無音、警告ログ)。
      → デバイスとチャンネル(ResolvedLayer::forcedCh)の両方を解決する
        (2026年7月、DrumNote::fixed_ch依存を廃止)。
```

**OPL系内蔵リズムチャンネル(`COPLRhythm`)は、この`0x70`経路を使わない**
(2026年7月に実装、当初は0x70への統合を検討したが設計変更)。OPNA/OPLL
のリズム音がROM固定でHwPatchを要求しないのに対し、OPLのリズム音は
BD(バスドラム、2op)以外は1オペレータのみの実際のFM音色パラメータを
要求するため、**`VOICE_PATCH_OPL_RHY`(0x23)という通常のVoicePatchType**
を持たせ、他の直接モードチップと全く同じ経路(`resolveTriple`→
`HwBankRegistry`)でパッチを解決する。HwBankの名前空間も独立
(`VOICE_GROUP_RHYTHM`)。

「チャンネル番号=楽器」というハードウェア制約自体は`0x70`と同じで
あるため、`resolveTriple()`はHwBankRegistryから実際のHwPatchを解決
した後、`hwProg`(`patch_prog`)をそのままチャンネル番号として検証し、
`ResolvedTriple::forcedCh`に設定する(2026年7月、`0x70`と設計を統一)。
範囲外の`hwProg`は解決失敗として扱う(無音、警告ログ)。この結果、
1チャンネル(楽器)につき使えるHwPatchエントリは`hwProg`=そのチャンネル
番号の1個のみとなる(同一楽器に複数の音色候補を持たせることはできない、
という制約とのトレードオフ)。詳細は`docs/chip-driver-architecture.md`
の「VoicePatchType対応表」参照。

**「楽器番号=物理チャンネル番号」というハードウェア上の制約があるため、
`patch_prog`(hwProg)をそのままチャンネル番号として解釈する**
(2026年7月、`DrumNote::fixed_ch`依存を廃止)。`PatchManager::
resolveBuiltinRhythm()`がhwProgを検証し、解決結果(`ResolvedLayer::
forcedCh`)としてデバイスチャンネル番号を確定させる。呼び出し元
(`CRhythmCh::applyNoteOn`/`CInstCh::noteOn`)は共通して、
`forcedCh>=0`なら`assignCh(forcedCh, ...)`で強制的にそのチャンネル
へ割り当てる。

| チップ | patch_prog範囲 | 対応楽器(参考) |
|---|---|---|
| COPNARhythm(OPNA) | 0-5 (6ch) | 実機レジスタ0x10のビット0-5に対応 |
| COPLLRhythm(OPLL) | 0-4 (5ch) | 0=HH, 1=CYM, 2=TOM, 3=SD, 4=BD |

範囲外の`patch_prog`を指定した場合、`resolveBuiltinRhythm()`が解決
自体を失敗させる(警告ログを出し、`ResolvedTriple::isValid()`が
falseになる)。旧仕様の`fixed_ch`未設定時とは異なり、**動的チャンネル
割り当て(`allocCh`)へのフォールバックは発生しない**——誤った楽器が
鳴るより、発音しない方が安全という判断による。

`patch_prog`をチャンネル選択にそのまま使うことで、`CInstCh`の通常の
Program Change経由でも内蔵リズムの個別楽器を選択できるようになった
(以前は`DrumNote`(`CRhythmCh`)経由でのみ楽器選択が可能だった)。

なお、ハイハットのオープン/クローズ等、ノート間の相互ダンプ(チョーク)
は、以前は`DrumNote::fixed_ch`によるハードウェアチャンネル共有トリック
(同一物理チャンネルへの再アサインで物理的retriggerを起こす)で実現
していたが、2026年7月に**`DrumPatch::chokeGroups`**による、ハードウェア
に依存しない明示的な設計へ置き換えた。詳細は`docs/patch-structure-
design.md`の「チョークグループ」節を参照。`ResolvedLayer::forcedCh`
(本節のハードウェア制約)と`chokeGroups`(ノート間の相互ダンプ)は、
どちらも「チャンネル固定」に関わるが出自も実現レイヤーも別物である
点に注意。

COPNARhythmは各チャンネルが固定ピッチの打楽器を鳴らすのみで、ノート
番号に依存する処理は不要。一方COPLLRhythmは、ノート番号に応じて
Fnum/Blockレジスタを計算し、内蔵ドラム音のピッチシフト演奏を可能に
している(基底クラス`CSoundDevice::getFnumber()`の標準的なノート→Fnum
変換をそのまま使う)。COPLLRhythmは5つの楽器に対し3ch分のFnum
レジスタしかなく、一部の楽器(HH/SD、CYM/TOM)が同じレジスタを共有する
構造のため、異なるノート番号で同時発音すると後着優先で上書きされる
(仕様として許容)。

DrumNote経由で使う場合は、`voice_patch_type=0x70`,
`patch_bank=chipSel`, `patch_prog=instIndex`と設定する。

## GM2 リズム/メロディ切替 (CC#0=0x78/0x79) の詳細挙動

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

実装箇所: `CFITOM.cpp`の`MidiProcessor::processControl()`（分岐判定）、
`MidiProcessor::switchChannelRole()`（実際の切替処理）。

### GM2正式仕様との違い（意図的な設計判断）

**FITOM_XはGM2完全準拠を目指していない。** 仕様決定の指針として、
固有の実装にする理由がない項目については可能な範囲でGM2に揃える、
という緩い参照に留めている。以下はその方針のもとで、意図的に
GM2正式仕様とは異なる挙動を採用している箇所。

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
これは実装上の都合ではあるが、以下の理由から現在のFITOM_X
アーキテクチャとして一貫性があり、固有の実装とする理由があると
判断し、**GM2正式仕様に合わせる修正は行わない方針を確定した**
（2026年7月時点）。

- FITOM_Xのチャンネルロールは`unique_ptr<IMidiCh>`の実体差し替え
  (`CInstCh`⇔`CRhythmCh`)という設計であり、「ペンディング状態を
  保持したまま次のメッセージを待つ」という中間状態を持たない
  (常に「今どちらのオブジェクトか」が明確な、シンプルな状態機械)
- 実用上、シーケンサーやDAWは通常Bank SelectとProgram Changeを
  ほぼ同時に送出するため、2段階/1段階の違いが実際の演奏結果に
  与える影響は限定的

エンドユーザー向けドキュメントでは、「FITOM_XはBank Select
MSB=0x78/0x79受信の瞬間にチャンネルロールが切り替わる(GM2正式仕様の
「Program Change受信まで待つ」という2段階方式とは異なる)」ことを
明記すること。
