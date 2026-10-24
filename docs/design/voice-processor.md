# 音色データと VoiceProcessor

**対象読者**: FITOM_X本体の開発者・AI  
**関連文書**:
- 利用者向けの各パラメータの意味: `docs/manuals/hwpatch-reference.md`、`docs/manuals/swpatch-reference.md`
- チップごとのレジスタへの書き方: `docs/design/chip-notes.md`

---

## 音色データの構成

音色のパラメータは、処理されるタイミングと処理する場所で2つに分かれる。

```
HwPatch(PatchData.h)            チップドライバがレジスタへ書く値
├── hw      FmHwVoice            チャンネル単位 (FB/ALG/AMS/PMS/NFQ/FB2)
├── hwOp[4] FmHwOp               オペレータ単位 (AR〜WS、PDT、OPZのREV/EGS/DT3)
├── ext     FmChipExt            特定のチップだけが使う値(下表)
├── builtin                      ROM/ビルトイン音色への参照(builtin専用バンクのみ)
└── swBank / swProg              パフォーマンスパッチ(SwPatch)への参照(-1=なし)

SwPatch(PatchData.h)            VoiceProcessor がノートオン時と毎tickに使う値
├── sw      FmSwVoice            チャンネルのソフトウェアLFO(ビブラート)
├── swOp[4] FmSwOp               オペレータ単位のベロシティ感度とソフトウェアLFO(トレモロ)
└── fineTranspose                固定トランスポーズ [セント]
```

- `HwPatch`のパラメータは、音色を設定したときにチップドライバがそのままレジスタへ書く。TLだけは例外で、`VoiceProcessor`が補正した値(`effectiveTL(op)`)を書く
- `SwPatch`のパラメータは、ノートオン時(ベロシティ感度)と1msごとのtick(ソフトウェアLFO)に`VoiceProcessor`が計算に使う。チップドライバはSwPatchを直接読まない
- `HwPatch`が`SwPatch`への参照を持つので、レイヤードパッチ・直接モード・内蔵リズム等、HwPatchが発音されるすべての経路で同じSwPatchが効く(解決の規則は`docs/design/patch-resolution.md`)

### `FmHwOp::PDT` と `FmChipExt`

チップごとに意味が違うフィールド。

| フィールド | 使うチップ | 意味 |
|---|---|---|
| `hwOp[].PDT` | OPN(ch2のFXモード) | `ext.FIX`のモードに応じた疑似デチューン・倍率・固定周波数(`docs/design/chip-notes.md`の「OPN FXモード」) |
| `hwOp[].PDT` | OPL3(4OPモード) | 前半/後半の2opペアの疑似デチューン。`hwOp[0]`/`hwOp[2]`の値を使う。単位はkfs |
| `hwOp[].REV`/`EGS`/`DT3` | OPZ | オペレータ単位。実機OPZは各オペレータに個別に設定できる |
| `ext.FIX` | OPZ | Osc fixed freqフラグ |
| `ext.FIX` | OPN(ch2) | FXモードの種別(0=通常/1=疑似デチューン/2=非整数倍率/3=固定周波数) |
| `ext.ALG_EXT` | OPM / OPLL / OPLLEX | OPM: ノイズ(ch7)。OPLL: プリセット選択。OPLLEX: bit0はプリセット選択、bit2-1はチャンネル別のROMプリセットバンク |
| `ext.HWEP` | SSG(AY-3-8910/YM2149) | ハードウェアエンベロープの周期(レジスタ0x0B+0x0Cの16bit値) |
| `ext.targetVoicePatchType` | PSG系共有バンク | パッチが実際に対象とするチップ(`docs/design/patch-resolution.md`の「PSG系共有バンク」) |
| `ext.rhythmCh` | OPL内蔵リズム(`VOICE_PATCH_OPL_RHY`) | 対象の楽器(0=HH/1=CYM/2=TOM/3=SD/4=BD、0xFF=未設定) |

これ以外の組み合わせでは、チップドライバは`ext`を読まない。

## VoiceProcessor

デバイスチャンネルごとに1つあり(`ChState::proc`)、ノートオン時に`onNoteOn()`、1msごとに`onTick()`が呼ばれる。

### 呼び出しの順序

`onNoteOn()`は`CSoundDevice::assignCh()`の中で、`updateVoice()`より前に呼ぶ。`updateVoice()`はキャリアのベロシティ補正値(`velAR`〜`velRR`)とTLをレジスタへ書くので、その前に計算が済んでいなければならない。`CSoundDevice::noteOn()`では`onNoteOn()`を呼ばない(呼ぶとLFOのリセット等が二重に走る)。

### キャリアの判定

`VoiceProcessor`はチップ非依存の設計のため、どのopがキャリア(音を出す
オペレータ)かを自力では判定できない(`hw.ALG`の意味・ビット幅・キャリア
規則はチップごとに異なる: OPN/OPMは3bit8アルゴリズム、OPL(2op)は1bit、
OPL3(4op)は独自の8アルゴリズム、OPLLは常にop1固定 等)。呼び出し元
(`CSoundDevice::computeCarrierMask()`、内部で仮想関数`isCarrierOp(ch,op)`
を呼ぶ)がチップ固有の規則で計算した`carrierMask`を渡す設計になっている。
`isCarrierOp()`のデフォルト実装はOPN/OPM互換のテーブルで、キャリア規則が
異なるチップ(OPL系、OPLL等)はオーバーライドする。

`carrierMask`が効くのは**vol(マスターボリューム)の適用範囲のみ**である。
vel/expによる**VTL補正はキャリア/モジュレータを問わず全opに適用**する。
モジュレータのTLはFMの変調指数そのものであり、ここをvel/expで動かすことが
FM音源のベロシティ表現(強く弾くほど明るい音)の本体だからである。逆にvolを
モジュレータに掛けると、マスターボリュームを絞っただけで変調指数が下がり
音色そのものが変わってしまうため、volはキャリアに限定する。

### TLの計算

```
NoteOn 時:
  VoiceProcessor::onNoteOn(vol, exp, vel, voice, carrierMask)
    → baseTL[i] = ベロシティ/エクスプレッション補正(VTL) を全opに適用したTL
                  → さらに carrierMask の bit[i]=1 の op のみ calcLinearLevel(evol, TL)
    → 各 opLfo[i].start(swOp[i].SLY, swOp[i].SLR, swOp[i].SLI, swOp[i].SLM)

tick (1ms) 時:
  VoiceProcessor::onTick(voice)
    → opLfo[i].tick() → recalcOpLfo(i, voice)
      → effectiveTL[i] = getLfoWave(SLW, SLR) * level * SLD + baseTL[i]
    → チップドライバが proc.effectiveTL(i) をTLレジスタへ書く
```

`baseTL`/`effectiveTL`はキャリア・モジュレータを問わず**ラウドネス空間**
(0=無音, 127=最大音量)で保持する。実機のTLレジスタは減衰量空間(0=最大音量)
なので、チップドライバは書き込み前に必ず`linear2dB()`(各ドライバの
`effTLToReg()`)で逆変換する。

チップドライバ側も、モジュレータのTLレジスタを音色データの生値ではなく
`proc.effectiveTL(op)`から書くこと。生値を書くとVTLもオペレータLFO
(トレモロ)もモジュレータに届かない。同じ理由で`updateVolExp()`は全opを
書き直す(expressionはVTL経由でモジュレータの変調指数を動かすため)。

OPLLだけは制約がある。モジュレータTLはユーザー音色レジスタ`$02`にあり、
このレジスタ群(`$00-$07`)は実機OPLLでは全ch共有のため、ある chのVTL補正が
他chのユーザー音色にも及ぶ。これはベロシティ補正後のEGレートが元から
同じ共有レジスタへ書かれているのと同じ構造上の制約である。プリセット音色は
音色パラメータ自体を持たないため対象外。

### チャンネルのソフトウェアLFO(ビブラート)

```
NoteOn 時:
  chLfo.start(sw.LFD, sw.LFR, sw.LFI, sw.LFM)

tick (1ms) 時:
  chLfo.tick() → recalcChLfo(voice)
    → depthKfs = sw.depthCents * 64 / 100
    → chLfoValue = getLfoWave(LWF, LFR) * depthKfs / 120
  → チップドライバが周波数のオフセットに chLfoValue を加える
```

### ベロシティ感度

すべて `VoiceProcessor::onNoteOn()` 内で NoteOn 時に計算され、
`velAR(op)`〜`velRR(op)` / `effectiveTL(op)` としてチップドライバから参照される。

| パラメータ | 効果 | 補正式 |
|---|---|---|
| `VTL` | vel/exp 低下時に TL を増加（キャリアなら音量減、モジュレータなら変調指数減=音色が暗く） | `delta = -kGM2dB[min(vel,exp)] × VTL/254 ÷ 0.75`（hwTLに加算、全op対象） |
| `VAR` | ベロシティ → AR（アタックレート）の補正 | 最大補正幅 8 (=31÷4)、下限1（発音保証） |
| `VDR` | ベロシティ → DR（ディケイレート）の補正 | 最大補正幅 8 |
| `VSL` | ベロシティ → SL（サステインレベル）の補正 | 最大補正幅 4 (=15÷4)、増加方向 |
| `VSR` | ベロシティ → SR（サステインレート）の補正 | 最大補正幅 8 |
| `VRR` | ベロシティ → RR（リリースレート）の補正 | 最大補正幅 4 |
| `VLD` | ベロシティ → ソフトLFO Depth の補正 | 未実装（予約） |
| `VLR` | ベロシティ → ソフトLFO FadeIn の補正 | 未実装（予約） |

補正式（VAR〜VRR共通）:
```
sens_vel = (127 - vel)
delta    = maxDelta × sensitivity/127 × sens_vel/127
velParam = clamp(hwParam ± delta, min, max)
```

`maxDelta` は各パラメータのレンジの1/4（AR/DR/SR=8、SL/RR=4）。
AR の下限は1に固定（0はチップによって「発音しない」を意味するため）。

**適用範囲**: `VTL`は`VoiceProcessor`が全opに掛ける。`VAR`〜`VRR`は`VoiceProcessor`が全opについて補正値を計算し、どのopに使うかはチップドライバが決める。OPN/OPM/OPL/OPLLのドライバはキャリアにだけ使い、モジュレータには音色データの値を書く。PSG系はソフトウェアエンベロープに使う。PSG系(AY-3-8910/YM2149)でハードウェアエンベロープを使うときは、エンベロープの形がチップ側で決まるので、`VAR`〜`VRR`は効かない。

## ソフトウェアLFO

`LfoControl`クラスが1つのLFOを表し、オペレータ単位(トレモロ)とチャンネル単位(ビブラート)の両方で使う。

- 周期は`rateToTicks(rate)`で決め、位相(`phase_tick_`)とフェードイン(`fade_tick_`)を別々に進める
- 波形は上昇のこぎり/矩形/三角/S&H/下降のこぎり/デルタ/サイン。S&Hの乱数の種はインスタンスごとに持ち、チャンネル間で干渉しない
- モード(`LfoMode`)は repeat / one-shot hold / one-shot zero
- NoteOff後は`fadeout()`で自然に消え、`ChState::Releasing`(2000ms保持)と連動する
- `LFS=0`/`SLS=0`(またはCC#1駆動のLFOが動いている)で、同じデバイスチャンネルの次のNoteOnがLFOを引き継ぐときは、`cancelFadeout()`で前のノートのフェードアウトを取り消し、`fadeout()`を呼ぶ前の段階とフェード係数へ戻す。位相はフェードアウト中に進んだ分を引き継ぐ。取り消さないと、リリース中のチャンネルを再利用したノートやモノの弾き直しで、LFOが2000ms後に止まる。始め直す(`start()`)のでなく戻すのは、`LFS=0`が「ノートをまたいで位相を続ける」指定であり、始め直すとディレイ(`LFD`)も毎回かかり直すため

### オペレータ単位 (FmSwOp) — トレモロ

| パラメータ | 意味 |
|---|---|
| `SLW` | 波形種別 (0=上昇のこぎり/1=矩形/2=三角/3=S&H/4=下降のこぎり/5=デルタ/6=サイン) |
| `SLS` | NoteOn でリセットするか (0=しない/1=リセット) |
| `SLM` | モード (0=repeat/1=one-shot hold/2=one-shot zero) |
| `SLD` | 深さ (0〜63 / 64〜127 → -64〜-1 の符号付き) |
| `SLY` | 遅延 [20ms 単位] |
| `SLR` | レート (0=LFO無効。1-127は対数カーブで0.5Hz〜50Hzに変換、rateToTicks()参照) |
| `SLI` | フェードイン速度 (0=即フルデプス) |

### チャンネル単位 (FmSwVoice) — ビブラート

| パラメータ | 意味 |
|---|---|
| `LWF` | 波形種別（オペレータLFOと同じ） |
| `LFS` | NoteOn でリセットするか |
| `LFM` | モード (0=repeat/1=one-shot hold/2=one-shot zero) |
| `LFD` | 遅延 [20ms 単位] |
| `LFR` | レート (0=LFO 無効) |
| `LFI` | フェードイン速度 |
| `depthCents` | 深さ [セント、-1200〜+1200、0=無変調] |

`depthCents`はセント単位で指定し、内部でkfs単位(`docs/design/terminology.md`参照)に変換して適用する(SLD(トレモロ深さ)と同じ計算パターン)。範囲を±1200セント(±1オクターブ)まで取るのは、ピッチLFOをドラム音の装飾(大きなピッチベンド効果等)にも使うため。

### のこぎり波の位相とモードの関係

上昇/下降のこぎり波(`SLW`/`LWF` = 0/4)は、いきなり最大振幅から始まって
発音開始時にピッチ/音量が飛ぶのを避けるため、いずれのモードでも t=0 で
変位0から始まる。ただし、その実現方法は `SLM`/`LFM` によって異なる
(`LfoControl::wave()`)。

| モード | 1周期の走査 (下降のこぎり波の場合) |
|---|---|
| repeat | 半周期分位相をずらす。`0 → -120 →(不連続)→ +120 → 0`。振幅は±120 |
| one-shot hold/zero | 片側傾斜。`0 → -120` を単調に走査(不連続点なし)。振幅は0〜-120 |

repeat では波形が回り続けるため、不連続点が周期境界にあっても中間に
あっても聞こえ方は変わらない(位相オフセットの違いでしかない)。一方
one-shot の1周期はそのままピッチ/音量エンベロープになるため、周期の
途中に不連続点があってはならない。

one-shot hold のホールド値(周期完了後に保持する値)は、位相リセットで
変位0(=無変調)に戻ってしまわないよう、傾斜が向かっていた側の最大振幅
(上昇=+120 / 下降=-120)とする。片側傾斜の周期末尾の値と連続するため、
ホールドへの移行でも変位は飛ばない。

### レートから周期への変換

`SLR`/`LFR`(レート、0=LFO無効、1〜127)は、`VoiceProcessor.cpp`の
`rateToTicks()`関数で、対数(指数)カーブの数式により実際のLFO周期
(tick、1tick=1ms)に変換される。

```cpp
static uint16_t rateToTicks(uint8_t rate) noexcept
{
    if (rate == 0) return 0;
    constexpr double kFreqMin = 0.5;   // Hz (rate=1)
    constexpr double kFreqMax = 50.0;  // Hz (rate=127)
    const double t = static_cast<double>(std::min<uint8_t>(rate, 127) - 1) / 126.0;
    const double freq = kFreqMin * std::pow(kFreqMax / kFreqMin, t);
    return static_cast<uint16_t>(std::lround(1000.0 / freq));
}
```

rate値(1〜127)を1増やすごとに、周波数が一定比率(約3.7%)で変化する
ため、低域・高域を問わず知覚的に均等なステップになる。周波数に比例する
値のテーブルにすると、ビブラートで最もよく使う低い帯域ほど隣どうしの
比率が大きくなり、分解能が粗くなる。

| rate | 周期[tick=ms] | 周波数[Hz] | 備考 |
|---|---|---|---|
| 1 | 2000 | 0.50 | 最遅 |
| 16 | 1156 | 0.87 | |
| 32 | 644 | 1.55 | |
| 48 | 359 | 2.79 | |
| 64 | 200 | 5.00 | |
| 70 | 161 | 6.23 | CC#1のビブラートの周期 |
| 80 | 111 | 8.97 | |
| 96 | 62 | 16.10 | |
| 112 | 35 | 28.90 | |
| 127 | 20 | 50.00 | 最速 |

レート0はLFO無効(関数呼び出し自体が行われない)。

### CC#1 との関係

`LFR>0`（音色固有LFOが設定されている）音色では CC#1 は無視される。
`LFR==0` の音色でのみ CC#1 が有効になり、sine波形・rate=70(約6.23Hz)固定で、
デプスは RPN#5 (Modulation Depth Range) で設定可能な範囲で CC#1 値に比例する。
CC#1 の初期値は 0（GM2仕様通り、ビブラートなしがデフォルト）。

### ハードウェアLFOを音色に持たない理由

チップ内蔵のLFO(OPM等)の周波数・深さは、ボイスパラメータに持たない。チップ内蔵LFOはチップ全体で1系統の共有資源で、マルチティンバー・ダイナミックボイスアサインと相性が悪いため。音色が持つのはチャンネルごとの感度(`hw.AMS`/`hw.PMS`)とオペレータごとの`AM`だけで、周波数と深さはCC#14/15で演奏中に送る(`docs/design/midi-processing.md`の「ハードウェアLFO」)。
