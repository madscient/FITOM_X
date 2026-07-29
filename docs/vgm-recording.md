# VGMレコーディング機能 技術検討

**ステータス**: 設計検討完了、**実装は未着手**(2026年7月)。方針の骨子は確定した: レジスタ書き込みは`HWPort::write()`/`writeBurst()`(既存のシャドウレジスタ更新と同じチョークポイント)にフックして記録する/ タイミングは壁時計ベースでVGMのwaitサンプルへ変換する(FITOM_Xがサンプル精度の内部クロックを持たないため)/ ヘッダは既存の`CFITOM::PhysicalChipInfo`列挙(レジスタダンプモニター用に2026年7月新設)を流用して構築する/ 波形バイナリを使うチップ(ADPCM-A/B、AWM、PCMD8)は`PcmBankRegistry`が持つ実ファイルパスをVGMのROMイメージ型data block(0x67)として埋め込み、他プレイヤーでも再生可能にする/ VGM仕様に対応チップIDが存在しないチップ(OPZ/OPP/VRC7/SID等)が構成に含まれる場合は、記録開始そのものをGUI側で拒否する(部分的な記録は行わない)。

**検討日**: 2026年7月
**検討の目的**: FITOM_Xが実際にMIDI演奏をチップへ流している最中のレジスタ書き込みを、そのままVGM(Video Game Music)形式のファイルとして記録できるかどうかを検証する。GUIから任意のタイミングで開始・停止でき、出力ファイル名は開始時に指定する、というユースケースを想定する。

---

## 1. 要件

- VGMレコーディングはGUIから任意に開始・停止できる。
- 出力ファイル名は開始時に指定する。
- レコーディング開始後、**最初のレジスタ書き込みが発生した時点から**実際のVGMファイルへの記録を始める(開始操作自体のタイミングではない)。
- GUIから停止した時点でVGMファイルをファイナライズする(ヘッダの総サンプル数・EOFオフセット等を確定させる)。
- VGMヘッダ(チップ種別・クロック等)はプロファイル読み込み時点で確定するはず、という前提を検証する。

---

## 2. 現状のアーキテクチャ確認

### a. FITOM_Xは音声合成を行わない

FITOM_XはMIDIメッセージを解釈し、`ISoundDevice`経由でチップのレジスタへ書き込みを行うだけで、音声合成自体はHWプラグイン(実機/エミュレータ)側の責務である。したがってFITOM_X本体には**サンプル精度のオーディオクロックが存在しない**。これは3節で述べるタイミングモデルの制約に直結する。

### b. 全レジスタ書き込みの唯一のチョークポイント

`HWPort::write()`/`writeBurst()`([HWPort.h:137,139](../core/include/fitom/HWPort.h#L137))は、実機・エミュレータを問わず`IHWPlugin`契約を通る**すべての**レジスタ書き込みが最終的に通る場所であり、既にレジスタダンプモニター用の`shadowRegs_`([HWPort.h:181](../core/include/fitom/HWPort.h#L181))をここでミラーしている。VGM記録もこれと全く同じ場所にフックできる。

### c. 物理チップ単位の列挙が既に存在する

レジスタダンプモニター向けに2026年7月新設された`CFITOM::PhysicalChipInfo`([CFITOM.h:153-178](../core/include/fitom/CFITOM.h#L153))・`buildPhysicalChipList()`([CFITOM.cpp:591](../core/src/CFITOM.cpp#L591))が、サブデバイス自動生成(OPNA→FM+SSG+ADPCM-B等)・同種デバイス自動束ね(spanGroups)・2ポートチップ(port/port2)を全て解決した上で、「物理チップ1個」単位に`deviceType`・`port`/`port2`・クロック(`port->getClock()`)を列挙してくれる。これはVGMヘッダが要求する情報(チップ種別・クロック)そのものであり、そのまま流用できる。

### d. 波形バイナリの実ファイルパスも既に引ける

`PcmBankRegistry`が保持する`PcmBank::binPath`([PcmBankData.h:97](../core/include/fitom/PcmBankData.h#L97))・`PcmBank::deviceType`([PcmBankData.h:128](../core/include/fitom/PcmBankData.h#L128))により、ADPCM-A/ADPCM-B/PCMD8等の波形バイナリファイルへのパスをdeviceType単位で解決できる。FITOM_X本体はこのバイナリをチップへ転送しない設計(波形データの配置はhwif側の責務、STATUS.md参照)だが、**VGMファイルへ埋め込む目的でなら、FITOM_X自身がこのファイルを直接読み込んで良い**(hwif側の責務とは別の話)。

---

## 3. 核心的な設計上の緊張関係

### a. タイミングモデル: 壁時計ベースにならざるを得ない

VGM形式は「wait N samples (44100Hz換算)」というコマンドで時間経過を表現する。FITOM_Xは2.aの通りサンプル精度の内部クロックを持たないため、記録は**`std::chrono::steady_clock`の経過時間を44100Hzサンプル数へ換算する**方式にならざるを得ない。これはハードウェアMIDI音源をVGM化する既存ツール群と同種のアプローチであり、OSのスレッドスケジューリング由来のジッタがwait値に多少乗ることは設計上受け入れる(オフラインレンダリングによるサンプル精度の生成とは前提が異なる)。

### b. VGM仕様のチップカバレッジは有限集合である

VGM仕様(v1.71)が直接サポートするチップは固定リストであり、FITOM_Xの`DEVICE_*`(`FITOMdefine.h`)のうち、少なくとも以下は直接対応できる:

| FITOM_X deviceType | 対応するVGMチップ |
|---|---|
| `DEVICE_OPN` (YM2203) | YM2203 |
| `DEVICE_OPNA` (YM2608) | YM2608 |
| `DEVICE_OPN2` (YM2612) | YM2612 |
| `DEVICE_OPNB` (YM2610) / `DEVICE_2610B` (YM2610B) | YM2610 / YM2610B |
| `DEVICE_OPM` (YM2151) | YM2151 |
| `DEVICE_OPLL` (YM2413) | YM2413 |
| `DEVICE_OPL` (YM3526) | YM3526 |
| `DEVICE_OPL2` (YM3812) | YM3812 |
| `DEVICE_OPL3` (YMF262) | YMF262 |
| `DEVICE_Y8950` | Y8950 |
| `DEVICE_OPL4`/`DEVICE_OPL4AWM` (YMF278) | YMF278B |
| `DEVICE_SSG`/`DEVICE_PSG` (AY-3-8910系) | AY8910 |
| `DEVICE_DCSG` (SN76489) | SN76489 |
| `DEVICE_SCC`/`DEVICE_SCCP` | K051649 (SCC) |
| `DEVICE_PCMD8` (YMZ280B) | YMZ280B |
| `DEVICE_SAA` (SAA1099) | SAA1099 |

一方、FITOM_Xが持つ以下のチップにはVGM仕様側に対応するチップIDが**存在しない**:

- `DEVICE_OPP`/`DEVICE_OPZ`/`DEVICE_OPZ2`(YM2164/YM2414/YM2424、OPMの派生)
- `DEVICE_VRC7`(FS1001)
- `DEVICE_EPSG`(AY8930)/`DEVICE_SSGS`/`DEVICE_SSGL`/`DEVICE_SSGLP`
- `DEVICE_SID`(MOS6581/8580)
- `DEVICE_OPQ`/`DEVICE_OPK`/`DEVICE_OPK2`/`DEVICE_RYP4`/`DEVICE_RYP6`/`DEVICE_FMS`/`DEVICE_5232`
- `DEVICE_MA1`/`MA2`/`MA3`/`MA5`/`MA7`/`DEVICE_SD1`
- `DEVICE_OPN2C`/`DEVICE_OPN2L`/`DEVICE_OPN3`/`DEVICE_OPN3L`/`DEVICE_OPNC`/`DEVICE_F286`(OPNA/OPN2の実装差分クローンで、VGM側は無印のYM2608/YM2612として書けば互換動作する可能性はあるが未検証)

この非対称性は本質的にVGM仕様側の制約であり、FITOM_X側の実装でどうにかできるものではない。**ユーザー確認済みの方針**: 記録対象の物理チップ一覧に1つでも非対応deviceTypeが含まれる場合、記録開始そのものをGUI側で拒否する(該当チップだけ無視して部分的に記録する、という選択肢は「後で気づきにくい欠落したVGM」を生みやすいため採らない)。

---

## 4. 提案する設計

### ① 記録の開始・停止(GUI起点)

`FITOMBridge`に以下を新設する(既存の`saveCurrentProfile()`等と同じ、コア層の機能をそのまま呼ぶだけの薄いラッパーパターン)。

```cpp
// 開始失敗時(非対応チップを含む等)はfalseを返し、outReasonに理由文字列を入れる
bool startVgmRecording(const std::string& outFilePath, std::string* outReason = nullptr);
void stopVgmRecording();
bool isVgmRecording() const;
```

`startVgmRecording()`はコア層(後述の`VgmRecorder`)へ委譲し、`CFITOM::getPhysicalChipInfo()`を全走査して4節⑤のマッピング表と照合、非対応deviceTypeが1つでもあれば失敗として返す。GUI側は失敗時にモーダルでエラー表示する(外部パッチエディタ起動失敗時のエラーダイアログと同じパターン)。

### ② `VgmRecorder`の新設・所有・フック位置

`core/include(src)/fitom/VgmRecorder.{h,cpp}`を新設し、`CFITOM`が所有する(`std::unique_ptr<VgmRecorder> vgmRecorder_`、`CFITOM::getVgmRecorder()`)。

`HWPort::write()`/`writeBurst()`内、`shadowRegs_`更新と同じ箇所に、`VgmRecorder`が有効かつ記録中であれば素通しする一行を追加する:

```cpp
if (auto* rec = CFITOM::instance().getVgmRecorder(); rec && rec->isActive())
    rec->onRegisterWrite(this, addr, static_cast<uint8_t>(data));
```

`HWPort`自身はVGMの知識(チップIDやコマンドバイト)を一切持たない。マッピング等はすべて`VgmRecorder`側に閉じ込め、「`HWPort`は薄いI/O層である」という既存設計を崩さない。

### ③ タイミング: 壁時計→VGM waitサンプルへの変換

`VgmRecorder::onRegisterWrite()`内で`steady_clock::now()`を見て、**記録開始後の最初の呼び出し**でt0を確定する(要件どおり「最初のレジスタ書き込みから実際の記録を開始する」)。以降の呼び出しでは、前回書き込み時刻からの経過時間を44100Hzサンプル数に換算し、差分を`wait`コマンド(基本形`0x61 nn nn`、環境によっては1/60秒ショートカット`0x62`も利用)としてバッファへ追記してから、実際のレジスタ書き込みコマンドを追記する。

### ④ deviceType → VGMチップ・ポートのマッピング

`VgmRecorder`内部に、3節bの対応表を実装したテーブル(`CFITOM.cpp`の`kDevMap`と同じ「静的テーブル+線形探索」のパターンを踏襲。VGM固有の知識のため`CFITOM.cpp`本体ではなく`VgmRecorder`側に置く)を持つ。各エントリは deviceType・VGMチップID・ヘッダ上のクロックフィールドオフセット・データコマンドのオペコード(port0/port1が別れているチップはその両方)を持つ。

`PhysicalChipInfo::port`/`port2`([CFITOM.h:161-162](../core/include/fitom/CFITOM.h#L161))は、VGM側のport0/port1コマンド(例: YM2608なら`0x56`/`0x57`)にそのまま対応する。既存の2ポート設計(`SplitPort`/`OffsetPort`)とVGM仕様の2ポート表現が構造的に一致しているのは好都合な点である。

同一チップ種別の物理チップが複数存在する場合(例: OPNAを2枚使う構成)、`getPhysicalChipInfo()`の列挙順で2台目以降を「dual chip」としてクロックフィールドの最上位ビット(0x40000000)を立てる。

なお、コマンドオペコードの正確な値(特にSCC/SAA1099/YMF278Bまわり)は本検討では概要レベルの確認に留めており、**実装時に公式VGM仕様書(vgmrips.net等)で最終確認する**必要がある。

### ⑤ VGM非対応チップの除外(記録開始拒否)

3節bのマッピング表に存在しないdeviceTypeが`getPhysicalChipInfo()`の列挙に1つでも含まれる場合、`VgmRecorder::start()`は失敗として理由(該当デバイスのlabel一覧)を返し、`FITOMBridge::startVgmRecording()`経由でGUIがエラーダイアログに表示する。

### ⑥ 波形バイナリの埋め込み(data block)

ADPCM-A/ADPCM-B/AWM/PCMD8等、波形バイナリを使うチップについては、VGM仕様のROMイメージ型data block(`0x67 0x66 tt <size:4> <ROMサイズ:4> <開始アドレス:4> <データ...>`、`tt`はチップ別のブロック種別、例: YM2608 delta-t=0x82、YM2610 ADPCM-A/delta-t=0x83/0x84、YMF278B=0x87、YMZ280B=0x89)を使う。

`PcmBankRegistry::PcmBank::binPath`/`deviceType`(2.d節)から該当チップの実ファイルを1回読み込み、開始アドレス=0でそのまま埋め込む。FITOM_X自身が既にこの`.bin`を「Start/Endレジスタへ書くバイトオフセット」として扱う設計(`registerVoice()`、STATUS.md 2026年7月の修正群参照)になっているため、**記録済みのStart/Endレジスタ書き込みが、埋め込んだdata blockに対してそのまま正しいオフセットとして機能する**(既存の設計と自然に整合する)。

複数の物理チップが同じ`.bin`を共有する構成(spanGroups等)では、同じ内容を重複して埋め込まず1回にまとめる(`Sf2BankRegistry`のファイル重複除去と同じ考え方)。

### ⑦ ファイナライズ

停止時、`VgmRecorder::stop()`が総サンプル数・EOFオフセット・GD3オフセットをファイルへシークして書き戻す。GD3タグ(トラック名等のメタ情報)は最小限(出力ファイル名や記録日時程度)を付与する。ループポイントは本機能のスコープ外とし、常に「ループ無し」(loop offset=0)として出力する。

### ⑧ スレッド安全性

`HWPort::write()`は複数のMIDI処理スレッド(MPU毎、内部MIDIパイプ等)から並行して呼ばれうるため、`VgmRecorder`は`shadowMutex_`と同じパターンで専用のmutexを持ち、書き込みバッファ(および直近書き込み時刻)への追記を保護する。

---

## 5. 残る検討事項(実装時に詰める必要がある点)

- コマンドオペコード・data blockタイプの正確な値の最終確認(特にSCC/SAA1099/YMF278Bまわり、公式VGM仕様書 v1.71)。
- クローン/派生チップ(`DEVICE_OPN2C`/`OPN2L`/`OPN3`系等)を、無印チップ(YM2612/YM2608等)として書けば実用上問題ないか、それとも安全側に倒して非対応扱いにするか。
- 複数MPU/複数プロファイル同時記録の要否(本検討では「1回のレコーディングで、その時点で構成されている全物理チップの書き込みを1ファイルにまとめる」という前提で設計している)。
- 出力ファイルの書き込み失敗(ディスク容量不足、パス不正等)時のGUI側エラーハンドリング。
- `VgmRecorder`が保持する書き込みバッファのメモリ上限(長時間録音時、逐次ファイル書き込みにするかオンメモリで持ち切るか)。

---

## 6. 結論

現在の構造のまま実装**可能**。レジスタダンプモニター向けに既に存在する基盤(`HWPort`の単一チョークポイント・`CFITOM::PhysicalChipInfo`の物理チップ列挙・`PcmBankRegistry`の波形バイナリパス解決)をほぼそのまま流用でき、新規に必要になるのは次の4点に閉じる。

1. `HWPort::write()`/`writeBurst()`への薄いフック(`VgmRecorder::onRegisterWrite()`の呼び出し1行)。
2. deviceType→VGMチップID/コマンド/クロックオフセットのマッピングテーブル(`VgmRecorder`内、`kDevMap`と同じパターン)と、非対応チップを検出して記録開始を拒否するチェック。
3. 壁時計(`steady_clock`)の経過時間をVGMのwaitサンプルへ変換するタイミングロジック(FITOM_Xがサンプル精度の内部クロックを持たないための必然的な設計)。
4. `PcmBankRegistry`の`binPath`を使ったROMイメージdata blockの埋め込み(既存のStart/Endレジスタのオフセット設計とそのまま整合する)。

いずれも既存アーキテクチャの原則(`HWPort`は薄いI/O層に徹する、コア層に物理チップの構成情報を集約する)から逸脱しない範囲に収まる。実装に着手する際は、5節の残る検討事項、特にコマンドオペコードの最終確認を先に済ませること。
