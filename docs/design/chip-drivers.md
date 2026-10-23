# チップドライバ アーキテクチャ

新FITOMの全チップドライバの継承構造、sub-device自動生成、同種デバイス自動束ね、
VoicePatchType対応を横断的にまとめる。個々のチップドライバの検証過程で得られた
設計判断・既知の制限も併記する。

---

## 1. 基底クラス階層

```
ISoundDevice (抽象インターフェース)
  ├── CSoundDevice          単体チップの共通実装 (レジスタキャッシュ・
  │                         ChState管理・VoiceProcessor連携等)
  │     └── (各チップドライバの直接の基底、下記2章参照)
  │
  └── CMultiDevice           複数 ISoundDevice を束ねる共通基盤
        ├── CSpanDevice      グローバルch→実チップ+ローカルchに変換して委譲
        │                    (sub-device自動生成・同種デバイス束ね両方で使用)
        └── CUnison          全チップに同一ch番号をブロードキャスト
                             (デチューン等のユニゾン効果、現状未使用)
```

`CMultiDevice`/`CSpanDevice`/`CUnison`は`MultiDevice.h`にヘッダーとして実装されており、
チップドライバ側（`OPN2_new.cpp`/`OPL_new.cpp`等）から`#include`して直接継承できる。

### Fnumberテーブルとnoteoffsetの二重適用に関する既知の注意点

`CSoundDevice`のコンストラクタは`FnumRegistry::instance().getTable(fnumType,
master, divide, noteOffset_)`で`fnumTable_`を生成する際、`noteOffset_`を
テーブル生成式`(offset+i)/768`に**既に焼き込んでいる**。したがって基底クラス
`CSoundDevice::getFnumber()`(および同じ`fnumTable_`を参照する`CAdPcmBase`系の
`getFnumber()`)が実行時に`s.lastNote*64`へさらに`noteOffset_`由来の項を
加算するのは二重適用であり、block(オクターブ)が数オクターブ分ズレるバグと
なる(2026年7月に発見・修正。ノート48(C3)入力でBlk=0固定になる不具合として
表面化した)。`COPM::getFnumber()`のように`fnumTable_`を使わず独自テーブル
(KeyCode等)で計算するチップドライバは、実行時に`noteOffset_`相当の値を
加算する設計で問題ない(ただしその場合は`* 64`であり`* 64 / 12`のような
半音→セント変換の再スケーリングは不要。`noteOffset_`は既にテーブル
インデックス単位＝100/64セント単位で渡されているため)。

### ISoundDeviceで新設した主要メソッド（本来チップ固有だがpublic化したもの）

| メソッド | 理由 |
|---|---|
| `updateTL(ch, op, lev)` | ソフトLFO(トレモロ)の変調結果を`CSoundDevice::timerCallback()`から反映するため(かつては`CRhythmCh`も独自に呼んでいたが、2026年8月に二重tick解消のためデバイス側へ一本化した) |
| `getChState(ch)` | `VoiceProcessor::onNoteOn`等が`CSpanDevice`経由でも正しく実チップの状態を取得できるようにするため |
| `setCC1Modulation(...)` | 同上 |
| `forceDamp(ch)` | デフォルト実装は`noteOff(ch)`のみ。`CMultiDevice`にも委譲実装が必要（3章参照） |

---

## 2. Sub-device 自動生成 (composite chip)

**1つの物理/エミュレーターチップ指定から、内部的に複数の`ISoundDevice`インスタンスを
自動生成する仕組み。** 実機の1チップが複数の独立した音源機能（FM本体・SSG・ADPCM・
内蔵リズム等）を持つ場合に対応する。`Config::resolveCompositeSpec()`が展開ルールを持ち、
`Config::pushDeviceEntries()`が実際に複数の`DeviceEntry`を生成する。各サブデバイスは
同一の物理ポート(`e.port`)を共有するが、`usesExtraPort=true`のサブデバイスは
`e.port2`も受け取る(2ポート目=`extraPort`が必要なもの。`DEVICE_ADPCMB_OPNA`/
`DEVICE_ADPCMA`もここに該当。実チップ上これらのレジスタは`port2`側[アドレス
0x100以降]に配置されるため。`CFITOM::resolveHighBankPort()`が実際のポート
差し替えを行う。2026年7月、ユーザー指摘で発覚: 以前はSSGと同じport1のまま
割り当てておりレジスタアドレスが衝突していた)。

`DEVICE_OPL4AWM`(OPL4のAWM/PCM部)はこれとは別の仕組みで高位バンクへ
差し替わる: `usesExtraPort=false`のまま(`e.port2`=SplitPort用の2ポート目は
使わない)、`CFITOM::resolveHighBankPort()`がdeviceType判定で
`OffsetPort(port, 0x200)`を自前生成して割り当てる。OPL4は実チップ上
FM部(port1[アドレス0x000-0x0FF]+port2[アドレス0x100-0x1FF]の2バンク、
`DEVICE_OPL3`/`DEVICE_OPL3_2`が共有)とは独立した3つ目のレジスタバンク
(アドレス0x200以降)にAWM部が配置されるため、既存の2ポート(`port`/`port2`)
モデルでは表現できず、ADPCM同様のオフセットポート機構を流用している
(2026年7月、ユーザー指摘で発覚: 以前はAWM部がFM部のport1と同じ低位バンクに
割り当てられておりレジスタアドレスが衝突していた)。オフセット量そのものは
`CFITOM::getHighBankOffset(deviceType)`(ADPCM-A/ADPCM-B[OPNA]は0x100、
OPL4AWMは0x200、それ以外は0)として`resolveHighBankPort()`から切り出して
あり、下記のレジスタダンプモニターの表示サイズ算出とも共有する単一の
情報源になっている。

**レジスタダンプモニター(GUI `RegisterDumpWindow`)との整合**: `CFITOM::
buildPhysicalChipList()`は物理ポート単位で表示サイズ(`dumpSize`)を決める際、
同一物理ポートを共有する全サブデバイスについて
「`getHighBankOffset()`+`getDeviceRegSize()`」を計算し、その最大値を採用する
(*最初に登録されたもの*だけでは足りない。OPL4は`DEVICE_OPL3`[0x000-0x1FF]が
先に登録されるが、後から登録される`DEVICE_OPL4AWM`[0x200-0x2FF]の方が高位)。

`getDeviceRegSize()`が返すレジスタ空間サイズは`CFITOM.cpp`の`kRegSizeMap`に
持つ。値はチップドライバが実際に書き込む最終レジスタアドレスを16byte境界へ
切り上げたもので(例: OPLLの有効レジスタは0x38までなので0x40、OPNAは後半3ch
のport2側0x1B6までなので0x1C0)、1ポート分(0x100)より小さいチップはそのまま
小さい範囲を表示する。分類の単位は`DeviceFactory::create()`のswitch
(=どのチップドライバが担当するか)に一致させること。`kRegSizeMap`に未登録の
`deviceType`しか乗っていない物理チップのみ、フォールバックとして0x100を出す。

`RegisterDumpWindow.cpp`側はダンプの実バイト数から表示アドレス範囲・行数を
動的に組み立てる設計のため、`dumpSize`さえ正しければGUI側の変更は不要
(ただし16列固定グリッドのため、`dumpSize`は必ず16の倍数であること)。

**波形番号レジスタ(reg 0x08/reg 0x20)の書き込み順**: `COPL4AWM::
updateVoice()`(`core/src/OPL4.cpp`)は、波形番号の上位1bit(reg 0x20+ch
bit0)を下位8bit(reg 0x08+ch)より**先に**書かなければならない。YMEngineが
使うエミュレーションコア`ymfm`(`extern/ymfm/src/ymfm_pcm.cpp`の
`pcm_engine::write()`)は、reg 0x08-0x1Fへの書き込みで即座に
`load_wavetable()`をトリガーし、その時点のreg 0x20+ch bit0と組み合わせて
9bit波形番号を確定させる。順序を逆にすると、新しい波形番号のbit8が反映
される前(1つ前のノートのbit8のまま)でロードが実行され、意図した波形と
256ずれた別の波形が鳴ってしまう(2026年8月、ユーザー報告「AWMは音は出るが
意図した波形でない」により発覚)。ALSAの`sound/drivers/opl4/opl4_synth.c`
(`snd_opl4_note_on()`)でも、`OPL4_REG_F_NUMBER`(reg 0x20+ch)を
`OPL4_REG_TONE_NUMBER`(reg 0x08+ch)より先に書いており、後者の呼び出しに
「triggers header loading」と明示的にコメントされている(実チップ・
エミュレータ双方でこの順序が必須であることの裏付け)。この制約はOPN系の
F-number書き込み(reg 0xA4[高位]→reg 0xA0[低位]の順で、低位バイト書き込みが
確定トリガーになる設計)と同種のYamaha FMチップ共通の慣習でもある。

**Fnumber/Octave計算式と波形ごとのピッチ/音量校正**: 上記の書き込み順を
直しても、ユーザーから「まだ意図した波形ではないような鳴り方」と報告があり、
ALSAドライバとの突き合わせでさらに2件の根本原因が判明した(2026年8月)。

1. **計算式そのものがAWM用ではなかった**: `COPL4AWM::getFnumber()`が誤って
   共通の`getFnumberFromHz()`(OPN/OPM系FM合成のFnum位相累算器用、`fnumMaster_`/
   `fnumDivide_`という無関係な定数を使い、Octaveを0〜7[常に非負]にクランプ
   する)を流用していた。実際のAWMエンジン(`extern/ymfm/src/ymfm_pcm.cpp`)は
   `step=((0x400|fnum)<<(octave+7))>>2`(ROM上のバイトを1出力サンプルあたり
   何byte進めるか)という全く別の式で、Octave(reg 0x38 bit7-4)は符号付き4bit
   (-8〜+7、`int8_t(ch_octave<<4)>>4`で符号拡張)。ALSAの
   `snd_opl4_update_pitch()`も`octave=pitch/0x600-8`と明示的に符号付き計算
   している。`getFnumber()`をALSAと同じ基準点(note=60・オフセット0で
   `pitch=60*128=7680→octave=-3,fnum=0`)を持つ専用式に書き換えた。
2. **波形ごとの校正データが欠落**: ROM波形は実測でないと絶対ピッチ・音量が
   分からない(ウェーブヘッダにその情報が無い)ため、1を直しただけでは
   波形によって数オクターブ単位でピッチがずれたままだった。ALSAの
   `sound/drivers/opl4/yrw801.c`(`opl4_sound`構造体の`pitch_offset`
   [100/128セント単位]・`key_scaling`[%]・`tone_attenuate`[加算減衰]・
   `volume_factor`[0-254の乗算スケール])をPythonスクリプトで機械的に
   パースし、`SampleZone`に追加した同名4フィールド
   (`pitchOffset`/`keyScaling`/`toneAttenuate`/`volumeFactor`、
   `config_schema/samplezonebank.schema.json`にもスキーマ追加)へ、既存の
   `config/profiles/opl4awm_yrw801_gm/drum.samplezonebank.json`の各ゾーンを
   `wave_index`(+key範囲)完全一致でマージした(GM 553ゾーン・ドラム57
   ゾーンとも欠落・曖昧一致ゼロで全件マッチ)。`COPL4AWM::getFnumber()`/
   `updateVolExp()`はこれらの値を解決したゾーンから読み、ALSAの
   `snd_opl4_update_pitch()`/`snd_opl4_update_volume()`と同じ規約で適用する。

**固定音量ブースト(`kVolumeBoost`)**: 上記2の`volumeFactor`適用後、ユーザーから
「OPL4AWMだけミックスレベルがかなり低い」と報告(2026年8月)。`core/src`全体を
grepし、reg 0xF8/0xF9(ミキサーレベル)を書いているのは`COPL4AWM::init()`
だけで、そちらはymfmの8段階テーブル(`s_mix_scale[8]={0x7fa,...,0}`、値が
小さいほど大音量)のindex0=最大音量のまま初期化されており無関係と確認した。
真因は`volumeFactor`(GM移植データでは概ね140〜240、254=無補正)適用の副作用:
`totalLevel=127-(127-totalLevel)*volumeFactor/254`という式は、CC#7/CC#11/
velocityを全て最大にして生の`totalLevel=0`になっても、`volumeFactor<254`の
波形では0まで下がりきらない(例: volumeFactor=200なら27が下限)ため、AWM
全体が常に一定量だけ静かになっていた。ALSAの`sound/drivers/opl4/opl4_seq.c`
はこれを見込んで既定値8の`volume_boost`(モジュールパラメータ、コメントは
「Additional volume for OPL4 wavetable sounds」)を`snd_opl4_update_volume()`
内で差し引いており、`COPL4AWM::updateVolExp()`にも同じ固定値
(`kVolumeBoost=8`)の減算を追加し、上限クランプもALSAと同じ`0x7e`
(0x7fではない)に合わせた。あわせて、reg 0xF8/0xF9のビット幅を4bitニブルと
誤記していたコメント(実際はymfm/ALSAとも3bitずつで、線形の減衰量ではなく
8段階テーブルからの選択式)と、「FM出力ミキサーはCOPL3側が別途担当する」
という誤った想定のコメント(reg 0xF8/0xF9はAWM側レジスタバンクにしか存在
せず別バンクのCOPL3からは書き込めないため、実際は`COPL4AWM::init()`が
FM/PCM両方のミキサーレベルを初期化する唯一の経路)も訂正した。

**LevelDirect(reg 0x50 bit0)** — ラウンドロビン一周後に音量・音色が変わる
不具合の真因(2026年8月、最終決着): 上記の修正を全て適用してもなお
「AWMチャンネルを連打してラウンドロビンで1周すると、以後ずっと音量が
小さくなり音色も変わる」という報告が残った。FITOM_X側のレジスタ
シャドウキャッシュ(`getReg()`)を見る限りwaveIndex/fnum/octave/
finalTotalLevelは1周前後で完全に一致しており、「FITOM_Xが書いたつもりの
値」には異常が無かった。`setReg()`の`forceWrite=false`時のキャッシュ
スキップ機構(値が変わらなければ実書き込み自体をスキップする)、
`reg 0x105`(NEW1/NEW2)の再クリア、の2つの仮説を検証したがいずれも
不一致(前者はforceWrite=trueにしても再現、後者はラウンドロビン一周
というトリガーと整合しないため撤回)。

最終的に、ユーザー了承のもと`YMEngine`(`extern/ymfm/src/ymfm_pcm.cpp`、
submodule、ローカル検証用でcommit対象外)へ`load_wavetable()`/
`keyonoff()`/`prepare()`の内部状態を直接ファイル出力するデバッグ計装を
追加し、ビルドした`YMFMEngine.dll`を検証環境へ一時差し替えて確認した
ところ、**`m_total_level`(reg 0x50由来のTotalLevelをオーディオレート側で
補間する内部状態)が、ATTACK開始時点で直前のノートの値のままで、目標値
まで78.2ms(最小→最大)/156.4ms(最大→最小)かけてゆっくり近づいていく**
ことが判明した。真因は`COPL4AWM::updateVolExp()`がreg 0x50 bit0
(LevelDirect)を常に0(補間あり)で書いていたこと。ymfmの
`pcm_channel::prepare()`はLevelDirect=1の場合のみ`m_total_level`を
目標値へ即座にスナップする実装で、ALSAの`snd_opl4_note_on()`もノートオン
時の最初の音量セットだけ`level_direct=1`(即時反映)にし、以降のCC由来の
更新は`level_direct=0`(補間)に戻す設計になっている。FITOM_X側は常に0で
書いていたため、チャンネル再利用時など直前のTotalLevelと新しいノートの
目標値が離れていると音量が徐々にしか立ち上がらず(短いノートだと立ち
上がりきる前に終わる)、これが「音量が大きく下がる」「(補間途中の
中間的な減衰量域での聴こえ方の違いにより)音色も変わって聞こえる」症状の
直接原因だった。

`updateVolExp()`をreg 0x68のKEYONビット(bit7)で判定し、KEYON未セット
(ノートオン処理中、まだ実際のKEY ON書き込み前)ならLevelDirect=1、既に
KEYON中(発音中のリアルタイムCC#7/CC#11変化)ならLevelDirect=0とするよう
修正した。判定に`ChState::isRunning()`を使わなかったのは、
`CSoundDevice::noteOn()`内で`s.run()`が実際の`updateKey(true)`呼び出し
より先に実行されるため、まだKEY ON未送信の段階で`isRunning()`が`true`に
なってしまい判定に使えないため(`getReg()`でreg 0x68の実際のKEYONビットを
直接見ることで、この呼び出し順序に依存しない判定にした)。ユーザーによる
再現テスト(キーオン168回、ラウンドロビン約7周)で症状が再現しなくなった
ことを確認済み。

```cpp
struct SubDeviceSpec {
    uint32_t    deviceType;
    const char* labelSuffix;
    bool        usesExtraPort;  // 2ポート目(extraPort)を必要とするか
    bool        rhythmCapable;  // rhythm_modeプロファイル設定をこのサブデバイスに適用するか
};
```

### 展開ルール一覧（`resolveCompositeSpec`）

| baseDeviceType | 展開されるサブデバイス |
|---|---|
| `DEVICE_OPNA` / `DEVICE_F286` / `DEVICE_OPN3` | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMB_OPNA`(port2側) + `DEVICE_OPNA_RHY`(6パート) |
| `DEVICE_OPNB` (OPNB無印、YM2610) | FM本体(`COPNB`、実効4ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMA`(6ch、port2側) + `DEVICE_ADPCMB`(1ch、port1側) |
| `DEVICE_2610B` (OPNBB、YM2610B) | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMA`(6ch、port2側) + `DEVICE_ADPCMB`(1ch、port1側) |
| `DEVICE_OPL` | FM本体(9ch) + `DEVICE_OPL_RHY`(5パート、そのインスタンスの`rhythm_mode:true`時のみ) |
| `DEVICE_Y8950` | FM本体(9ch) + `DEVICE_ADPCMB_Y8950`(1ch、内蔵ADPCM-B、常に生成) + `DEVICE_OPL_RHY`(5パート、そのインスタンスの`rhythm_mode:true`時のみ) |
| `DEVICE_OPL2` | FM本体(9ch) + `DEVICE_OPL_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_OPL3` / `DEVICE_OPN3_L3` | `DEVICE_OPL3`(4OPモード,6ch) + `DEVICE_OPL3_2`(2OP残余,6ch) + `DEVICE_OPL_RHY`(5パート、`rhythm_mode:true`時のみ。COPL3_2側port1サブチップのch6-8を専有) |
| `DEVICE_OPL4` | `DEVICE_OPL3`(4OPモード,6ch) + `DEVICE_OPL3_2`(2OP残余,6ch) + `DEVICE_OPL4AWM`(PCM部,24ch、高位バンク[アドレス0x200以降]側) + `DEVICE_OPL_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_OPLL` / `OPLL2` / `OPLLP` / `OPLLX` | FM本体(9ch) + `DEVICE_OPLL_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_OPLLEX` (Y8960) | FM本体(9ch) + `DEVICE_OPLLEX_RHY`(5パート、`rhythm_mode:true`時のみ。4.5.3節) |
| `DEVICE_OPL2EX` (Y8960) | FM本体(9ch) + `DEVICE_ADPCMB_OPL2EX`(1ch、常に生成) + `DEVICE_OPL2EX_RHY`(5パート、`rhythm_mode:true`時のみ。4.5.3節) |
| `DEVICE_DSG` (YM2163) | 楽音部(`CDSG`、4ch) + `DEVICE_DSG_RHY`(5パート、**常に生成**。レジスタ空間が楽音部と独立しているため`rhythm_mode`による出し分けが不要) |
| `DEVICE_SSGS` (YMZ705) / `DEVICE_SSGS2` (YMZ732) | SSG互換部(`CSSGS`、YM2149相当×2 = 6ch) + `DEVICE_SSGS_ADPCM`(8ch)。両者は同一の8bitレジスタ空間($00-$3F / $40-$B3)を共有するためport2・オフセットとも不要。**ADPCM部のdeviceTypeはSSGS/SSGS2で共有する**(制御が完全に同一のため。混在構成でも自動的に同一グループとして束ねられ、PCMバンク/メモリイメージも1つで足りる)。**`DEVICE_SSGS3`(YMZ771)は展開対象外** — AMM部が当面非対応で生成すべきサブデバイスがSSG部しかないため、単一デバイスのままにする |

上記以外（単体`COPN`、`COPM`系、`CSSG`単体等）は展開されず、1エントリ=1デバイスのまま。
`DEVICE_OPNA`系の`DEVICE_OPNA_RHY`のみ例外で、`rhythm_mode`の値に関わらず常に
生成される(下記「OPL/OPLL系ビルトインリズムのプロファイル明示化」参照)。

**OPL系リズムモード対応(2026年7月)**: `COPLRhythm`(`DEVICE_OPL_RHY`)自体の実装・
`VOICE_PATCH_OPL_RHY`によるパッチ解決経路・HwBank/DrumKitデータは先行して
整備されていたが、`resolveCompositeSpec`側に`DEVICE_OPL_RHY`を生成する
分岐が無く、OPL/OPL2/OPL3/OPL4のいずれのプロファイルでも内蔵リズム
チャンネルのデバイス自体が実行時に存在しない(=一切発音しない)状態が
残っていた。OPLLで確立済みの構成パターン(FM本体+`_RHY`を同一ポートで
共有し、`rhythm_mode`プロファイル設定でFM本体側のch6-8を無効化する)を
OPL系にもそのまま適用して解消した。OPL3/OPL4はch6-8が`COPL3_2`側の
port1サブチップにあるため、`rhythm_mode`時はport1のch6-8のみを無効化し、
port2側の3chは通常の2OPとして引き続き使える(`COPL3_2`のコンストラクタ
コメント参照)。

**OPL/OPLL系ビルトインリズムのプロファイル明示化(2026年7月追加修正)**:
上記対応の直後、`_RHY`サブデバイス自体が`rhythm_mode`の値に関わらず
`resolveCompositeSpec`内で常に生成されており(=そのチップ種別が
devices[]に1つでもあれば、rhythm_mode指定の有無に関係なくリズム
チャンネル用デバイスが生成されてしまう)、`rhythm_mode:false`(既定値)の
インスタンスではFM本体側のch6-8が通常の楽音chとして有効なまま
`COPLRhythm`/`COPLLRhythm`と同じレジスタを無調整で共有する状態だったと
判明。ビルトインリズムと通常の楽音chをまたいだDVA(動的ボイス割当)は
実装されていないため、この共有状態はレジスタの奪い合いを招く。
`resolveCompositeSpec`の第2引数にそのデバイスインスタンス自身の
`rhythm_mode`設定値(`rhythmModeFromProfile`)を渡すよう変更し、OPL系/OPLL系
(`DEVICE_OPL_RHY`/`DEVICE_OPLL_RHY`)は`rhythmModeFromProfile==true`の
場合に限りリズムサブデバイスをoutSpecへ追加するよう修正した。これにより
チップインスタンスごとにビルトインリズムの使用可否を明示的に選べるように
なり、同一チップ種別を複数instances持つ場合でも、`rhythm_mode:true`が
1つも無ければリズムサブデバイス自体が一切生成されない(devices_全体に
`DEVICE_OPL_RHY`/`DEVICE_OPLL_RHY`が存在しない)。OPNA系の`DEVICE_OPNA_RHY`
はFM本体と完全に独立したレジスタ空間(0x10/0x11/0x18+ch)を持ち、ch0-5との
共有・DVA調整の必要が無いため、この対応の対象外とし従来通り常時生成する
(`resolveCompositeSpec`の第2引数はOPNA系のcase文では単に無視される)。

`DEVICE_OPNB`と`DEVICE_2610B`のサブデバイス構成(SSG/ADPCM-A/ADPCM-B)は同一。
両者の違いはFMチャンネル数(無印=実効4ch、B=6ch)のみで、それ以外のケーパビリティ
(SSG・ADPCM-A・ADPCM-Bの搭載)は共通(2026年7月、ステージング環境からの指摘で
訂正: 以前は無印にADPCM-B用メモリ空間が無いという誤った前提でADPCM-Bサブ
デバイスを生成していなかった)。

**Y8950のADPCM-B登録漏れ(2026年8月修正)**: `DEVICE_Y8950`(YM3801、
MSX-AUDIO)は実機にADPCM-B(Delta-T方式)を内蔵しており、`DEVICE_ADPCMB_Y8950`
というdeviceType・`DeviceFactory`のルーティング・`CYmDelta`による実体・
`VoicePatchType`変換(`VOICE_PATCH_ADPCMB`)まで一通り実装済みだったが、
`resolveCompositeSpec()`側では`DEVICE_OPL`と同一の`case`にまとめられており
FM本体+(rhythm_mode時のみ)`DEVICE_OPL_RHY`しか生成していなかった。この
ためプロファイルに`chip: "Y8950"`と書いても内蔵ADPCM-Bのインスタンスが
一切生成されない状態だった。`DEVICE_OPL`(YM3526、ADPCM非搭載)から`case`を
分離し、`DEVICE_Y8950`のみ`DEVICE_ADPCMB_Y8950`を追加するよう修正した。
ADPCM-Bのレジスタ空間はFM本体のオペレータレジスタ・リズム用ch6-8とは
独立しているため、OPNAの`DEVICE_ADPCMB_OPNA`と同様`rhythm_mode`の値に
関わらず常に生成する。

---

## 3. 同種デバイス自動束ね (CSpanDevice bundling)

**プロファイルに同一`VoicePatchType`・同一物理接続種別・同一パン設定のデバイスが
複数存在する場合、`CSpanDevice`で1つの論理デバイスに自動統合する。** 旧FITOMの
`isSpannable`機構の後継だが、判定基準を**厳密な`deviceType`一致**から
**`VoicePatchType`一致**に緩和している（`DEVICE_OPNA`と`DEVICE_2610B`のFM部分は
どちらも`VOICE_PATCH_OPN2`であり、新FITOMでは同じ`createCOPNA()`実装にルーティング
されるため、束ね対象になる）。

`Config::mergeSpannableDevices()`が`buildDevice()`完了後（**sub-device展開の後**）に
1回実行され、統合されたエントリは`devices_[]`から削除される。実際のマルチチップ
`ISoundDevice`生成は`CFITOM::initDevices()`側が担当し、束ね候補ポートそれぞれに
`DeviceFactory::create()`を呼んだ上で`CSpanDevice`にラップする。

```
グループ化キー: (VoicePatchType, IPort::getInterfaceDesc(), IPort::getPanpot())
```

`forceDamp`が`CMultiDevice`にデフォルトで委譲実装を持たない問題（`noteOff`にフォール
バックしてしまい急速減衰が効かない）を発見し、`CSpanDevice`/`CUnison`双方に明示的な
委譲実装を追加済み。

**サブチップごとのdeviceType/rhythm_modeの独立性**: グループ化キーは
`VoicePatchType`のみで判定するため、束ねられる複数のデバイスエントリは
`deviceType`(実装クラス、例:`DEVICE_OPNB`のch0/ch3無効化)や`rhythm_mode`
(OPL/OPLL系、ch6-8の内蔵リズム専用化)がそれぞれ異なりうる。そのため
`PortGroup`(`Config.h`)は`deviceType`・`rhythmMode`の両方を代表デバイスから
独立してサブチップごとに保持し、`CFITOM::initDevices()`のサブチップ生成
ループは代表の値(`config_->getDeviceType(i)`/`getDeviceRhythmMode(i)`)では
なく`config_->getDeviceSpanGroupDeviceType(i,k)`/`getDeviceSpanGroupRhythmMode(i,k)`
を使う。rhythmMode側は2026年8月に追加(それまでは代表デバイスの
`rhythm_mode`値を全サブチップへ一律適用していたため、`rhythm_mode:true`の
チップが別の`rhythm_mode:false`チップと同一`VoicePatchType`で束ねられると、
どちらが代表になるかによって「束ねられた側のch6-8無効化が効かない」または
「無効化不要な側にまで誤って波及する」のいずれかが起きていた。ユーザーが
「OPL/OPLLビルトインリズム使用時、元チップのch6-8はDVA対象外のはずなので
レベルメーター上も無効表示にしたい」と指摘したのを機に発覚・修正)。

**`allocCh()`のチップ試行順序はローテーションする**：`CSpanDevice::allocCh()`
は、mode=1(奪取なし)→0(奪取あり)を試す前段階で、束ねた`chips_`のどのチップ
から先に空きchを探すかを`nextChipIdx_`(割り当て成功のたびに1つ進める)で
毎回ずらす(2026年8月)。単純に常に`chips_[0]`から試すと、GM準拠のリズム
パートはほとんどのドラムノートにゲートタイムの概念が無く(規格上シーケンサー
が送るノートオンのゲートタイムはごく短い)、ノートオフ受信後も実際の発音は
すぐには止まらない(クラッシュシンバル等の長い減衰)ため、「空き/Releasingの
chが1つでもあれば即座にそこで確定する」という設計のまま常に1台目を優先
すると、1台目に(software上は`isReleasing()==true`だが実際にはまだ減衰中の)
chが1つでもある限り常に1台目だけが選ばれ続け、2台目以降のチップに
ほとんど到達しない(2台目が使われるのは1台目が"真に"全ch Running状態の
瞬間のみ)。これは同一物理chの再利用サイクルが早まり、まだ減衰中の
ノートが不用意にチョークされるリスクを高める。実際の発音時間(ROM解析による
減衰時間テーブル等)まで管理するのは大掛かりすぎるため、代わりに試行開始
チップを毎回ローテーションし、束ねた全チップに均等にラウンドロビンさせる
ことで同一chの再利用頻度を下げ、チョークの可能性を減らす方針とした
(各チップ固有の`queryCh`制約[OPMノイズch7固定等]には手を加えず、
チップの試行順序だけを変えるため既存の制約は保たれる)。

### 3.1 リニアステレオ化 (CLinearPanDevice) — 明示指定のL/Rペア束ね

**物理的にL/Rへ固定配線された同一チップ2台を、1つのステレオデバイスとして
束ねる機構**(旧FITOM `CLinearPan`の移植)。OPLL/OPL/PSG系のようにチップ自体が
モノラル出力しか持たない音源で、2台を左右に振り分けてパンポットを表現する
ための構成を想定している。`CLinearPanDevice`(`MultiDevice.h`)は`CUnison`
(全チップへ同一chで同時発音)を基底とし、`setVolume`/`setPanpot`だけを
オーバーライドして、パンポットに応じた等パワーパンニング(`lgain=cos`,
`rgain=sin`)でL側チップとR側チップの音量をクロスフェードする。両チップは
常に同時に鳴り続け、「左右どちらか一方だけを鳴らす」切り替えは行わない。

**自動検出はしない。** プロファイルの`devices[]`エントリ**両方**に
`stereo_pair`を明示指定した場合にのみ発動する
(`config_schema/profile.schema.json`)。`Config::mergeStereoPairDevices()`が
`buildDevice()`完了後・`mergeSpannableDevices()`より**前**に1回実行され、
R側エントリは`devices_[]`から削除された上で、L側エントリの`stereoPairPort`に
統合される。実際の`ISoundDevice`生成は`CFITOM::createLeveledDevice()`が担当し、
L/R各ポートに`DeviceFactory::create()`を呼んだ上で`CLinearPanDevice`にラップする。

```
ペアリング条件: (deviceType, IPort::getInterfaceDesc()) が一致し、
                L側エントリとR側エントリの組になっていること
```

**L/R役割の宣言方法は2通りあり、それが同時に「左右をどこで分離するか」も
決める**(`StereoSide`、2026年8月新設)。

| 指定 | L/R役割の判定 | 左右の分離を行う場所 | プラグイン側の`pan` |
|---|---|---|---|
| `"stereo_pair": true` | `IPort::getPanpot()`の1(L)/2(R) | **プラグインの出力ルーティング** | `1`(L) / `2`(R) が必須 |
| `"stereo_pair": "L"` / `"R"` | この値で明示 | **チップ自身のL/R出力ビット** | `0`(Stereo)のままでよい |

後者(チップ内L/R分離方式)では、`CLinearPanDevice`が束ねた2チップ自身の
`ChState::panpot`を左右端(-64 / +63)へ固定し、各ドライバの`updatePanpot()`が
チップのL/R出力ビットを書く。**負値=L / 正値=R は全ドライバ共通の規約**
(OPM: `0x20` bit7/6、OPL3: `0xC0` bit5/4、OPN2/OPNA: `0xB4` bit7/6)。
対応の可否は`FITOMConfig::deviceHasChipLevelPanpot()`が`deviceType`側の索引
として持つ(真の情報源は各ドライバの`updatePanpot()`実装であり、
`updatePanpot()`を実装・変更したら必ずこの一覧も見直すこと)。非対応チップに
`"L"`/`"R"`を指定した場合は読み込み時に警告を出す(モノラルチップでは
`updatePanpot()`が no-op のため、黙って左右に分離されないという静かな失敗に
なるのを防ぐ)。

対応チップ: **OPM/OPP・OPZ/OPZ2・OPL3/OPL3_2(=OPL4のFM部)・OPN2/OPNA/OPNB/
OPNBB系とそのADPCM-A/ADPCM-Bサブデバイス**。OPLL系・OPL/OPL2・SSG/PSG系・
YM2203は非対応(モノラル出力)。

なお片側だけ`"L"`/`"R"`・もう片側は`true`という混在指定の場合は、プラグイン側の
ルーティングが絡んで意図が確定しないため、従来方式(プラグイン側で分離)に倒す。

この方式の利点は、**プラグイン側の`pan`を`0`(Stereo)に保てる**こと。同一物理
チップ上の他のサブデバイス(OPL4のAWM部等)が、自前のハードウェアパンポットを
潰されずにフル活用できる。

制約として、OPNAのSSG部のように**チップ内にL/R出力の分離手段が無いサブ
デバイス**は、この方式でも左右に分かれない(音量クロスフェードだけが効くため、
パンを振っても定位ではなく音量だけが変化する)。これは実機の制約なので、
該当サブデバイスは`INFO`でその旨を記録するに留め、**ペア内の全サブデバイスが
非対応だった場合にのみ`WARN`**を出す(指定が完全に無効なケース)。

判定基準が`CSpanDevice`側(`VoicePatchType`)より厳しい**`deviceType`一致**で
あるのは、`CLinearPanDevice`が「物理的に同一のチップ2台」を前提とする構成
だからである(2026年8月に`VoicePatchType`基準から変更。`VoicePatchType`基準
では、専用の`VoicePatchType`を持たない内蔵リズムサブデバイス
[`COPLLRhythm`/`COPNARhythm`は`VOICE_PATCH_NONE`]が常に対象外となり、
R側だけが孤立して残ってしまう)。

**composite展開されたチップにも対応する**(2026年8月対応。それ以前は
`pushDeviceEntries()`が警告を出して`stereo_pair`を無視していたため、
`rhythm_mode:true`のOPLL/OPL系やOPNA系など、sub-device自動生成の対象となる
チップでは指定しても一切発動しなかった)。composite展開されたエントリ群は
同一の物理ポートを共有するため、**サブデバイス単位ではなく`compositeGroup`
単位**でペアリングする。L側グループとR側グループの
**「`stereo_pair`対象サブデバイスの部分集合」**が同数で、かつ`deviceType`が
1対1で対応できる場合に限り、対応するサブデバイス同士をまとめてステレオ化
する(サブデバイスごとに独立してマッチさせると、別チップのグループと
サブデバイス単位で混線した組み合わせになりうるため、グループ単位で判定して
から確定する)。

**ハードウェアパンポットを持つサブデバイスは対象外**:
`FITOMConfig::subDeviceAcceptsStereoPair()`が`false`を返すサブデバイスには
`stereo_pair`を伝播しない。対象は**OPL4のAWM/PCM部
(`DEVICE_OPL4AWM`、YMF278B)**、SSGS系(YMZ705/YMZ732/YMZ771)のSSG部・ADPCM部、
Y8960のOPL2EX/OPLLEX(FM部・ADPCM部・内蔵リズム)。AWM部はチャンネルごとのパンポットを
ハードウェアで持っており、`CLinearPanDevice`の束ね(モノラル出力しか持たない
音源に対する代替手段)を適用すると、本来のハードウェアパンポットを潰した上で
2倍のチップを消費することになるため(2026年8月、ユーザー指摘)。

判定対象がグループ全体ではなく部分集合なのはこのためで、これにより
**「OPL4のFM部」と「OPL3」をL/Rペアとしてステレオ化する**用法が成立する
(OPL4のcomposite展開は`DEVICE_OPL3`+`DEVICE_OPL3_2`+`DEVICE_OPL4AWM`、
OPL3は`DEVICE_OPL3`+`DEVICE_OPL3_2`であり、AWM部を除いた部分集合の
`deviceType`が一致する)。このときAWM部は独立したモノラルデバイスとして
`devices_[]`に残る。

**動的ボイス割当(DVA)**: `CLinearPanDevice`は`CUnison`を基底とするため、
`queryCh()`は代表チップ(`chips_[0]`)の判断を採用し、他チップは同じchが同じ
条件で使えるかだけを確認する。**この確認の基準は`mode`と一致させること**
(mode=1→`Empty`または`Releasing`、mode=0→`Disabled`でなければ可)。
`findBestCh()`はmode=1で`Releasing`を、mode=0では`Running`(強制奪取)を返す
設計なので、ここで一律に`isEmpty()`を要求すると代表チップが正当に返したchを
ほぼ必ず弾いてしまい、「全チップが完全に`Empty`」のときしか発音できなくなる
(2026年8月に実機で発覚・修正。`no channel available`が大量発生していた)。

**デチューンを適用しないこと**: `CUnison`はユニゾン/デチューン用途では
`noteOn()`時にチップ間へデチューンを与えるが、`CLinearPanDevice`は
「同じ音を同じ音程で左右に鳴らす」構成のため、コンストラクタで
デチューン量0を渡している。左右で音程がずれると定位ではなくビート(うなり)に
なるうえ、`ChState::fineFreq`へ代入する実装だったため、直前の
`setNoteFine()`が書いた音色側のfine tuneも失われていた(同じく2026年8月修正)。

**チャンネルレベルメーターでの扱い**: L/R分割表示(バー1本を左右half-widthの
2本に分けて描く)の有無は、**ビューの単位に従って決まる**(2026年8月、
ユーザーの実機確認で整理)。

| ビュー | リニアステレオ化ペア | チップ自身がL/R出力を持つ場合 |
|---|---|---|
| 物理チップ単位 | L側・R側それぞれ**独立したモノラルバンド**(統合しない) | 左右分割表示 |
| 論理チップ単位 | 1つのステレオデバイスとして**左右分割表示** | 左右分割表示 |

物理ビューで統合しないのは、OPLLのようなモノラル出力チップを2台使う構成に
おいて「物理チップ1台＝モノラル」が事実だからで、右ペインのレジスタダンプ
一覧(L側・R側が「(stereo pair)」付きラベルで別項目)とも一対一で対応する。
ステレオなのは束ねた結果できる**論理デバイス**の方であり、その定位は
`ISoundDevice::getStereoGains()`(`CLinearPanDevice`が実装、
`applyLinearPan()`と同じ等パワーパンニングの係数を正規化して返す)から得る。

チップ自身がL/R出力を持つ場合(`FITOMConfig::getChipPanType()`が`Mono`以外を
返すチップ)は、物理・論理どちらのビューでも左右分割表示になる。こちらの定位
係数は`ChState::panpot`から求め、`ThreeWay`(L/Rイネーブルビットのみ)は各
ドライバの`updatePanpot()`と同じ閾値で、`Continuous`(OPL4 AWM・YMZ280B・
SAA1099・SSGS系・Y8960の拡張ブロック)は等パワーパンニングの式で算出する。

なお`stereo_pair:"L"/"R"`(チップ内L/R分離方式)では、各物理チップのパンポットが
左右端に固定されるため、物理ビューではL側チップの全バーが左半分だけ、R側
チップの全バーが右半分だけ点灯する(実際にハードパンされていることが見える)。

---

## 5. VoicePatchType 対応表（チップドライバ横断）

`Config::deviceTypeToVoicePatchType()`が`deviceType`(DEVICE_*)→`VoicePatchType`
(VOICE_PATCH_*)を変換する。sub-device自動生成・同種デバイス束ねは全てこの値を
基準に動作する。詳細は`docs/design/patch-resolution.md`参照。

| VoicePatchType | 対応する deviceType | 生成クラス | オペレータ数 |
|---|---|---|---|
| `VOICE_PATCH_OPN`(0x10) | OPN, OPNC | `COPN` | 4 |
| `VOICE_PATCH_OPN2`(0x11) | OPN2, OPN2C, OPN2L, OPNA, OPN3L, OPNB, 2610B, F286, OPN3 | `COPNA` / `COPN2` / `COPNB` | 4 |
| `VOICE_PATCH_OPM`(0x19) | OPM, OPP | `COPM` / `COPP` | 4 |
| `VOICE_PATCH_OPZ`(0x1a) | OPZ | `COPZ` | 4 |
| `VOICE_PATCH_OPZ2`(0x1b) | OPZ2 | `COPZ`（共用） | 4 |
| `VOICE_PATCH_OPL`(0x20) | OPL, Y8950 | `COPL` | 2 |
| `VOICE_PATCH_OPL2`(0x21) | OPL2, OPL2EX | `COPL2` / `COPL2EX` | 2 |
| `VOICE_PATCH_OPL3_2`(0x22) | OPL3_2（OPL3の2opモード） | `COPL3_2` | 2 |
| `VOICE_PATCH_OPL_RHY`(0x23) | OPL_RHY（OPL系内蔵リズムチャンネル）, OPL2EX_RHY | `COPLRhythm` / `COPL2EXRhythm` | 1または2(楽器により異なる、混在可) |
| `VOICE_PATCH_OPLL`(0x28) | OPLL, OPLL2 | `COPLL` / `COPLL2` | 2 |
| `VOICE_PATCH_OPLLP`(0x29) | OPLLP | `COPLLP` | 2 |
| `VOICE_PATCH_OPLLX`(0x2a) | OPLLX | `COPLLX` | 2 |
| `VOICE_PATCH_VRC7`(0x2b) | VRC7 | `CVRC7` | 2 |
| `VOICE_PATCH_OPLLEX`(0x2c) | OPLLEX | `COPLLEX` | 2 |
| `VOICE_PATCH_OPL3`(0x30) | OPL3, OPN3_L3 | `COPL3`（4OPモード専用） | 4 |
| `VOICE_PATCH_SD1`(0x38) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_MA3`(0x39) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_MA5`(0x3a) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_MA7`(0x3b) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_SSG`(0x40) | SSG, PSG, SSGL, SSGLP, **SSGS**, **SSGS2**, **SSGS3**, **SSGEX** | `CSSG` / `CSSGS` / `CSSGS3` / `CSSGEX` | 1 |
| `VOICE_PATCH_EPSG`(0x41) | EPSG | `CSSG`（共用） | 1 |
| `VOICE_PATCH_DCSG`(0x42) | DCSG | `CDCSG` | 1 |
| `VOICE_PATCH_SAA`(0x43) | SAA | `CSAA1099` | 1 |
| `VOICE_PATCH_DSG`(0x44) | DSG | `CDSG` | HwPatch対象外(ROM固定音色、暗黙のバンク) |
| `VOICE_PATCH_SCC`(0x48) | SCC, SCCP, SCCEX | `CSCC` | 1 |
| `VOICE_PATCH_ADPCMB_Y8950`(0x50) | Y8950(ADPCM部) | `CYmDelta` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_ADPCMB`(0x51) | ADPCMB, **ADPCMB_OPNA** | `CYmDelta` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_ADPCMA`(0x52) | ADPCMA | `CAdPcm2610A` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_PCMD8`(0x53) | PCMD8 | `CAdPcmZ280` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_AWM`(0x54) | AWM | `COPL4AWM` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_SSGS_ADPCM`(0x55) | SSGS_ADPCM (YMZ705/YMZ732共通) | `CSSGSAdPcm` | HwPatch対象外(SampleZonePatch使用) |
| なし(`VOICE_PATCH_NONE`) | OPNA_RHY, OPLL_RHY, DSG_RHY 等リズムデバイス | `COPNARhythm` / `COPLLRhythm` / `CDSGRhythm` | HwPatch対象外(内蔵リズム、ダミーHwPatch使用) |

太字は複数の`deviceType`が同じ`VoicePatchType`に統合されている箇所（同種デバイス
自動束ねやsub-device生成の都合で意図的に統合したもの）。

「オペレータ数」列は、`hwbank.schema.json`の`ops`配列の実際の要素数
(1〜4で可変)、および`PatchManager::hwPatchToJson()`がHwBank保存時に
書き出すべき要素数を判定するための、正式な参照情報として使う
(2026年7月〜)。`ops`が「HwPatch対象外」の行は、そもそもHwPatchでは
なくSampleZonePatch(またはダミーHwPatch)を使うため、この文脈での
オペレータ数の概念自体が適用されない。

### 設計原則: デバイスに特殊ルーティングが必要かどうかの判断基準

新しいデバイス種別を追加する際、`resolveTriple()`の通常経路
(`voicePatchType`→`HwBankRegistry`検索→`findDeviceIndexByVoicePatchType`)
にそのまま乗せられるか、`0x70`(ビルトインリズム)のような専用バイパス
や、`isSampleBasedVoicePatchType`のような個別分岐が必要になるかは、
「リズム専用かどうか」ではなく、**そのデバイスの音色データが、標準の
`HwPatch`という形状でソフトウェア側に実在するかどうか**で決まる
(2026年7月、`COPLRhythm`実装時の議論から)。

| デバイス | 音色データの実体 | 特殊扱いの要否 |
|---|---|---|
| `COPNARhythm`/`COPLLRhythm` | チップROM内蔵の固定音色(ソフトウェアが管理する音色データが実質存在しない) | 必要 (`VOICE_PATCH_NONE`+`0x70`専用バイパス、または`findDeviceIndexByDeviceType`) |
| AWM/ADPCM系 | ソフトウェア管理下にはあるが、キーゾーン+ベロシティレイヤー+波形インデックスという、`HwPatch`(FMオペレータ型)とは全く異なる形状 | 必要 (`isSampleBasedVoicePatchType`による`SampleZoneBankRegistry`への分岐) |
| `COPLRhythm` | 他のFMチップと全く同じ`HwPatch`形状(オペレータ数が1〜2と狭いだけ) | **不要** (`resolveTriple`は無改造、最初から本物の`VoicePatchType`を持つ) |

音色データが`HwPatch`の形状に収まるデバイスは、たとえチャンネル数や
用途が特殊(リズム専用等)であっても、`VOICE_PATCH_NONE`のような
「識別子なし」の扱いにする必要はなく、専用の`VoicePatchType`を与えて
「音色データがデバイスを選択する」という通常の原則にそのまま乗せる
方が、コードの見通しが良くなる。逆に、音色データの形状自体が
`HwPatch`と根本的に異なる場合(AWM等)や、ソフトウェア側に音色データが
実在しない場合(ROM固定音源)は、専用の分岐が構造的に避けられない。

---

## 6. 既知の制限・未実装機能

| 項目 | チップ | 状態 |
|---|---|---|
| OPZ 2系統LFOリソース | COPZ | 旧FITOMも未完成のため現状維持 |
| CAdPcmZ280の正式な旧FITOM比較 | CAdPcmZ280 | 旧FITOMにPCMD8.cppは存在するが、部分的にしか突き合わせていない |

VoicePatchType完全一致以外へのフォールバックは実装済み
(`DeviceFactory::acceptsFallback`、各チップの`*AcceptsFallback`関数群)。
旧FITOMの単純な互換リスト方式とは異なり、パッチの実際の内容
(`hwOp[].WS`等、実際にそのチップ固有の拡張機能を使っているか)を見て
可否を判定する、内容駆動型の設計になっている。

**推奨されるバンク設計方針**: 同一チップファミリー内では、パッチの
`voicePatchType`は常に「そのファミリーの最上位(最も機能が豊富な)チップ」
として宣言することを推奨する(例: OPN系は常に`VOICE_PATCH_OPN2`、OPL系は
常に`VOICE_PATCH_OPL3_2`、OPZ系は常に`VOICE_PATCH_OPZ2`)。`acceptsFallback`
は「上位型で宣言されたパッチが、実際にはそのファミリー内の下位チップの
機能だけで表現できる内容か」を判定し、可能なら自動的に下位チップへ
フォールバックする設計になっているため、この運用に従えば:

- 下位チップしか無い環境でも(パッチが実際に上位専用機能を使っていない
  限り)正しく発音できる
- 同じパッチバンクファイルを、下位/上位どちらの環境でも共有できる
  (「下位チップ用バンク」「上位チップ用バンク」を重複して用意する必要がない)
- パッチ作成者は、実際に上位専用機能(非サイン波WS等)を使う場合にのみ、
  その効果が失われることを許容している(=下位環境では鳴らない)と
  自覚的に判断すればよい

2026年7月時点で全チップファミリーの`acceptsFallback`実装を監査し、
OPL系(`coplAcceptsFallback`)が`VOICE_PATCH_OPL3_2`方向を扱っていない
欠落を発見・修正済み。他のファミリー(OPN/OPN2、OPM/OPZ/OPZ2、SSG/EPSG、
OPLLファミリー)は監査の結果、既に全方向を正しくカバーしていることを
確認した。

## 8. CSoundDevice::chState_ の固定長配列オーバーフロー (2026年7月修正)

**症状**: `OPL4`を含むプロファイル(`emu_opl.profile.json`等)を`fitom_gui`で
読み込むと、デバイス初期化完了直後に無言でクラッシュする(ログ出力なし、
例外ダイアログも出ない`0xc0000005`アクセス違反)。Releaseビルドでのみ
安定再現し、`RelWithDebInfo`ビルドでは再現しないなど、ビルド構成に
よって発生有無が変わる不定動作だった。

**根本原因**: `CSoundDevice`は各チャンネルの状態を`ChState
chState_[MAX_CHS]`という固定長配列で保持しており、`MAX_CHS`は全チップ
共通の定数(旧値16)だった。一方`OPL4`のAWM(PCM/波形メモリ)部ドライバ
`COPL4AWM`(`core/src/OPL4.cpp`)は`CSoundDevice(DEVICE_OPL4AWM, 24, ...)`
で**24ch**として構築される。コンストラクタは`maxChs_`に渡された値を
クランプせずそのまま格納するため、`onMasterPitchChanged()`等の
`for (ch=0; ch<maxChs_; ++ch)`ループが、実体16要素しかない
`chState_`配列を範囲外(16〜23番目)まで読み書きしてしまうヒープ
バッファオーバーフローだった。

**調査の経緯**: Windowsイベントログ(Application Error)から
`fitom_gui.exe`が複数ビルド・複数日にわたり同じ`0xc0000005`で繰り返し
落ちていたことを確認。Releaseと同一の最適化フラグ(`/O2`)を保ったまま
`/Zi /DEBUG`を追加して再ビルドし、`%LOCALAPPDATA%\CrashDumps`の自動
ダンプと`dbghelp.dll`(P/Invoke経由)でクラッシュアドレスをシンボル化した
結果、`fitom::CSoundDevice::onMasterPitchChanged`
(`SoundDevImpl.cpp`の`chState_[ch].isActive()`)を指していた。

**修正**: 応急処置として`MAX_CHS`を24に拡大する案もあったが、「今
判明している最大値に追従するだけ」で将来より多chなチップが追加されれば
同じ問題を再発する場当たり的な対処のため採用しなかった。代わりに
`chState_`を`std::vector<ChState>`化し、コンストラクタで`maxChs_`と
**ちょうど同じ数**だけ`resize`する設計に変更した(`MAX_CHS`定数自体を
廃止)。配列サイズと`maxChs_`が食い違うという不変条件違反が構造的に
起こり得なくなる。

**同種パターンの横展開**: `MAX_CHS`を根拠にサイズを決めていた箇所を
全面調査し、以下も同時に修正した(チップファミリー間の一貫性の原則に
基づく):

- `core/src/PSG_new.cpp`の`CPSGBase::lfoTL_`/`envelopes_`
  (ソフトLFO基準TL・ソフトウェアADSR) — 同じ`MAX_CHS`依存の固定長配列
  だったため、`chState_`と同様に`vector`化
- `core/include/fitom/MultiDevice.h`の`CLinearPanDevice::masterVolume_`/
  `masterPan_` — `CSoundDevice::MAX_CHS`とは別に独自定義していた
  `kMaxChs_=16`という決め打ちサイズだった。現状16ch超のチップが
  `CLinearPanDevice`で束ねられることはないが、潜在的に同種の脆弱性を
  抱えていたため、束ねる2チップの`getChCount()`に基づいて動的にサイズを
  決める`vector`に変更した
- `core/src/OPLL_new.cpp`の`COPLL`(ch6-8をリズム専用に無効化する
  ループ)は、固定長配列の余裕に暗黙に依存して`maxChs_`に対する境界
  チェックを一切していなかった(コメントに「maxChs=6の場合は既に
  disable済みのため範囲外アクセスにならない」と明記されていた)。
  現状VRC7(maxChs=6)は常にリズムモード無効で呼ばれるため実害はない
  防御的な修正だが、`i < maxChs_`のガードを追加した

**教訓**: 「チップ固有の値を、全チップ共通の固定長配列サイズより
小さく保つ」という暗黙の不変条件は、チップドライバの実装者に伝わり
にくく、実際に3箇所で同種の脆弱性が見つかった。可能な限り「サイズと
実際のチャンネル数を構造的に一致させる」設計(今回のvector化)を優先し、
共有定数への依存は避けること。
