# FmVoice パラメータ分離設計書

## 問題: 旧 FMVOICE の混在

旧 `FMOP` / `FMVOICE` には異なる性質のパラメータが同一構造体に混在していた。

| フィールド | 旧位置 | 処理タイミング | 使用箇所 |
|---|---|---|---|
| AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/DT1/DT2/AM/VIB/EGT/WS | FMOP | UpdateVoice (即時) | チップドライバ |
| FB/AL/AMS/PMS/NFQ | FMVOICE | UpdateVoice (即時) | チップドライバ |
| SLF/SLW/SLD/SLY/SLR | FMOP | TimerCallBack (1ms) | SoundDev::UpdateOpLFO |
| VTL/VAR/VDR/VSL/VSR/VRR/VLD/VLR | FMOP | NoteOn 時 | SoundDev::SetVelocity |
| LFO/LWF/LFS/LFD/LFR/LDM/LDL | FMVOICE | TimerCallBack (1ms) | SoundDev::UpdateFnumber |
| REV/EGS/DM0/DT3 | FMOP | UpdateVoice (OPZ のみ) | OPZ ドライバのみ |

---

## 新設計: 4層分離

```
FmVoice
├── FmHwVoice   hw          チャンネル HW パラメータ (FB/ALG/AMS/PMS/NFQ)
├── FmHwOp      hwOp[4]     オペレータ HW パラメータ (AR〜WS)
├── FmSwVoice   sw          チャンネルソフト LFO (ピッチ変調)
├── FmSwOp      swOp[4]     オペレータソフトパラメータ (ベロシティ感度 + トレモロLFO)
└── FmChipExt   ext         チップ固有拡張 (OPZ: REV/EGS/DM0/DT3, PSG: HWEP)
```

---

## 各層の責務

### FmHwVoice / FmHwOp — チップドライバが直接参照

チップドライバの `UpdateVoice()` は `FmVoice::hw` / `FmVoice::hwOp[]` のみを参照する。
ソフトパラメータ・拡張パラメータは参照しない。

```cpp
// OPN::UpdateVoice (移行後)
void COPN::UpdateVoice(uint8_t ch)
{
    const FmVoice& voice = *GetChAttribute(ch)->GetVoice();
    SetReg(0xb0 + ch, (voice.hw.FB << 3) | (voice.hw.ALG & 7));
    for (int i = 0; i < 4; ++i) {
        SetReg(0x30 + map[i] + ch,
            ((voice.hwOp[i].DT1 & 7) << 4) | (voice.hwOp[i].MUL & 0xf));
        SetReg(0x40 + map[i] + ch,
            proc_->effectiveTL(i));   // ← VoiceProcessor が算出した値を参照
        // ... AR/DR/SR/RR/SL/EGT
    }
}
```

**`DT2`のチップ依存の意味の転用**：`DT2`はOPM/OPZ(HW)では2bitのDetune値だが、
OPN/OPL3(4OPモード)では未使用のため「疑似デチューン値」（符号付き8bit、
`static_cast<int8_t>(DT2)`、単位=100/64セント）として転用している。フィールド
幅・型は変更せず解釈のみで対応する。OPL3(`COPL3`)は`hwOp[0]`/`hwOp[2]`
（前半/後半2opペアの先頭オペレータ）の値を使う。

### FmSwOp (ベロシティ感度 + ソフトLFO) — VoiceProcessor が処理

```
NoteOn 時:
  VoiceProcessor::onNoteOn(vol, exp, vel, voice)
    → baseTL[i] = calcLinearLevel(evol, hwOp[i].TL) + ベロシティ補正(VTL)
    → 各 opLfo[i].start(swOp[i].SLY, swOp[i].SLR)

TimerCallBack (1ms) 時:
  VoiceProcessor::onTick(voice)
    → opLfo[i].tick() → recalcOpLfo(i, voice)
      → effectiveTL[i] = getLfoWave(SLW, SLF) * level * SLD + baseTL[i]
    → チップドライバ UpdateTL(ch, i, proc.effectiveTL(i)) を呼ぶ
```

### FmSwVoice (チャンネルLFO) — VoiceProcessor が処理

```
NoteOn 時:
  chLfo.start(sw.LFD, sw.LFR)

TimerCallBack (1ms) 時:
  chLfo.tick() → recalcChLfo(voice)
    → chLfoValue = getLfoWave(LWF, LFO) * level * depth(LDM/LDL)
  → チップドライバ UpdateFnumber(ch) を呼ぶ（周波数 offset に chLfoValue を加算）
```

### FmChipExt — 特定チップドライバのみ参照

OPZ ドライバは `FmVoice::ext` の `REV/EGS/DM0/DT3` を参照する。
PSG系ドライバ (CPSGBase) は `ext.HWEP` (HW Envelope Period, 16bit) を参照する
（AY-3-8910/YM2149 のレジスタ0x0B+0x0C、HW EG使用時のみ）。
OPN の `COPN` (ch2のみ) は `ext.DM0` を「FXモード選択」(0=通常/1=疑似デチューン/
2=非整数倍率/3=固定周波数) として流用する（OPZの用途とは無関係、`DM0`の
フィールド幅8bitの範囲で自由に解釈してよいという合意による）。
OPN/OPM/OPL 等、上記以外の組み合わせでは `ext` を参照しない。

---

## チップドライバの変更量

`UpdateVoice` で `FMVOICE*` を `FmVoice*` に変更し、フィールドアクセスを置き換える。

| 旧 | 新 |
|---|---|
| `voice->FB` | `voice->hw.FB` |
| `voice->AL & 7` | `voice->hw.ALG` |
| `voice->AMS` | `voice->hw.AMS` |
| `voice->PMS` | `voice->hw.PMS` |
| `voice->NFQ` | `voice->hw.NFQ` |
| `voice->op[i].AR` | `voice->hwOp[i].AR` |
| `voice->op[i].TL` | `proc->effectiveTL(i)` ← VoiceProcessor |
| `voice->op[i].SLF` | 参照不要（VoiceProcessor が処理） |
| `voice->op[i].VTL` | 参照不要（VoiceProcessor が処理） |
| `voice->op[i].REV` | `voice->ext.REV` (OPZ のみ) |

---

## 後方互換

### バンクファイル (.ini / 既存 JSON)

旧 `FMVOICE` 形式のバンクファイルは `FITOMCfg` の読み込み時に
`fitom::legacy::fromLegacy()` で変換する。書き出し時は `toLegacy()` を使う。

### GUI

旧 `FMVOICE*` を返す `GetCurrentVoice()` は引き続き動作させるため、
`CSoundDevice` 内で `FmVoice` ↔ `legacy::FMVOICE` の変換を挟む。
GUI の声音エディタが `FmVoice` を直接扱うように移行できたら、この変換層を除去する。

---

## ベロシティ感度パラメータの意味

すべて `VoiceProcessor::onNoteOn()` 内で NoteOn 時に計算され、
`velAR(op)`〜`velRR(op)` / `effectiveTL(op)` としてチップドライバから参照される。

| パラメータ | 効果 | 補正式 |
|---|---|---|
| `VTL` | ベロシティ低下時に TL を増加（音量を下げる） | `delta = (127-vel) * VTL / 127`（hwTLに加算） |
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

**全チップドライバ（OPN/OPN2/OPM/OPL/OPLL）で実装済み**。
ALGに基づくキャリアOP判定（`isCarrier()`）でキャリアのみに適用し、
モジュレータOPには適用しない。PSG系（AY-3-8910/YM2149）はHW EG使用時、
すべてのベロシティ感度が無効になる（チップ側の固定エンベロープ形状のため）。

---

## ソフト LFO パラメータの意味（全面再設計版）

旧実装の rate 未使用バグ・フェードイン/周期の混在・triangle波形の誤りを
修正し、`LfoControl` クラスとして全面再実装した。詳細設計は省略するが、
主な変更点は以下の通り。

### オペレータ単位 (FmSwOp) — トレモロ効果

| パラメータ | 意味 |
|---|---|
| `SLW` | 波形種別 (0=上昇のこぎり/1=矩形/2=三角/3=S&H/4=下降のこぎり/5=デルタ/6=サイン) |
| `SLS` | NoteOn でリセットするか (0=しない/1=リセット) |
| `SLM` | モード (0=repeat/1=one-shot hold/2=one-shot zero) |
| `SLD` | 深さ (0〜63 / 64〜127 → -64〜-1 の符号付き) |
| `SLY` | 遅延 [20ms 単位] |
| `SLR` | レート (0=LFO 無効, kSpeedStep参照) |
| `SLI` | フェードイン速度 (0=即フルデプス) |

旧 `SLF`（波形速度インデックス）は廃止し `SLR` に統合。

### チャンネル単位 (FmSwVoice) — ビブラート効果

| パラメータ | 意味 |
|---|---|
| `LWF` | 波形種別（オペレータLFOと同じ） |
| `LFS` | NoteOn でリセットするか |
| `LFM` | モード (0=repeat/1=one-shot hold/2=one-shot zero) |
| `LFD` | 遅延 [20ms 単位] |
| `LFR` | レート (0=LFO 無効) |
| `LFI` | フェードイン速度 |
| `LDM/LDL` | 深さ (16bit 符号付き: -8192〜+8191, 0=無変調) |

旧 `LFO`（HW LFO周波数として使われる想定だったフィールド）は廃止。
HW LFO（OPM/OPN2内蔵のLFOレジスタ）はマルチティンバー・ダイナミックボイス
アサインと相性が悪いため、ボイスパラメータからは切り離し、
CC#1 (Modulation) によるパフォーマンスパラメータとして別途実装した
（後述「CC#1 Modulation」参照）。

### LfoControl の主な改善点

| 旧の問題 | 新実装 |
|---|---|
| `rate_` が速度制御に未使用（常に1msで更新） | `period_ = kSpeedStep[rate%21]` で周期を直接管理 |
| フェードインと周期が混在 | `phase_tick_`（位相）と`fade_tick_`（フェードイン）を分離 |
| triangle と sine が同じ波形 | triangleは線形往復の独立計算 |
| S&H seed がグローバル static | インスタンスメンバーに変更（チャンネル間非干渉） |
| one-shot（1周期のみ適用）機能なし | `LfoMode` enumで実装 |
| NoteOff後に急に音が途切れる | `fadeout()`で自然にフェードアウト、`ChState::Releasing`(2000ms保持)と連動 |

### CC#1 Modulation との関係

`LFR>0`（音色固有LFOが設定されている）音色では CC#1 は無視される。
`LFR==0` の音色でのみ CC#1 が有効になり、sine波形固定・6.25Hz固定・
デプスは RPN#5 (Modulation Depth Range) で設定可能な範囲で CC#1 値に比例する。
CC#1 の初期値は 0（GM2仕様通り、ビブラートなしがデフォルト）。

---

## PSG系のソフトウェアエンベロープ (SoftEnvelope)

`CPSGBase`（`CSSG`/`CDCSG`/`CSCC`共通基底）は、AY-3-8910/YM2149のような
限定的なHWエンベロープしか持たないPSG系チップのため、独自のソフトウェア
ADSRエンベロープジェネレータ（`SoftEnvelope`クラス）を実装している。

- パラメータ範囲はFM音源(OPN)と同一 (AR/DR/SR: 5bit 0-31、SL/RR: 4bit 0-15)
- ATTACKフェーズは乗算的減衰（実機FM音源特有の凹型カーブ）
- DECAY/SUSTAIN/RELEASEフェーズは加算的増加（dB空間で線形＝指数的減衰）
- ベロシティ感度（VAR〜VRR）は他のFMチップと同じ仕組みをそのまま適用
- HW EG使用時（`EGT & 0x08`）はソフトウェアエンベロープを使わず、
  チップのハードウェアエンベロープに完全に委ねる

### AY-3-8910 HW EGレジスタの仕様修正

実機データシート確認の結果、レジスタ`0x0B`(Fine)+`0x0C`(Coarse)は
**分割不可の単一16bit「Envelope Period」値**であり、
4分割されたADSRパラメータではないことが判明した。
`FmChipExt.HWEP`（新設、16bit）にこの値を直接指定する形に修正した。

```cpp
setReg(0x0B, ext.HWEP & 0xFF);        // Fine
setReg(0x0C, (ext.HWEP >> 8) & 0xFF); // Coarse
setReg(0x0D, hwOp[0].EGT & 0xF);      // Shape (CONT/ATT/ALT/HOLDのビット組み合わせ)
```

---

## OPN FXモード (3rd channel special mode)

`COPN`のch2専用機能。実機OPNの「4オペレータそれぞれに独立したF-numberを
指定できるモード」（3rd channel special mode）を実装している。

### 新規フィールド: `FmHwOp::FXV` (int16_t、オペレータ単位)

```cpp
int16_t FXV;
```

`ext.DM0`（チャンネル単位、ch2全体で1モードを共有）で選択したモードに応じて
解釈が変わる：

| `ext.DM0` | モード | `FXV`の意味 |
|---|---|---|
| 0 | 通常 (FXモード無効) | 未使用 |
| 1 | 疑似デチューン | 100/64セント単位の符号付きオフセット (`getFnumber(ch, FXV)`) |
| 2 | 非整数倍率 | 100/64セント単位オフセット (倍率→セント換算。`1200×log2(倍率)`) |
| 3 | 固定周波数 | 0.1Hz単位の絶対周波数 (`getFnumberFromHz(FXV/10.0)`) |

対数周波数空間では「セント単位の加算」＝「周波数の倍率」と等価なため、
疑似デチューン・非整数倍率は同じ`FXV`型・同じ`getFnumber(ch,offset)`計算経路で
統一的に扱える。固定周波数モードのみ、ノート番号を無視して直接Hz値を指定する
別経路 (`getFnumberFromHz`、新設) を使う。

### `getFnumberFromHz()` — 直接Hz指定のFnum/Block変換

`CSoundDevice`に新設。`FnumUtils.h`のテーブル生成式（`val = freq×(2^17/master)×divide`）
をそのまま任意のHz値に適用し、11bit(0-2047)に収まるまでオクターブ正規化する。

### `fxCapable_`フラグ（旧FITOM`fxena`相当）

実機OPNのFXモードは「チップのch2」にのみ存在する物理的制約があるため、
`COPN`コンストラクタに`fxCapable`パラメータを追加した。単体`COPN`(YM2203)は
`true`固定。`COPNA`/`COPN2`（内部に`COPN`を2つ保持するCSpanDevice構成）は
前半サブチップ(port1、ch0-2)のみ`true`、後半サブチップ(port2、ch3-5)は
`false`（実機に該当レジスタが存在しないため）。`queryCh`もFXモード要求時に
ch2固定を強制する（`fxCapable_`なチップのみ、`updateVoice`/`updateFreq`と統一）。

---

## IPort / チップドライバ側の変更手順

フェーズ6（チップドライバ移行）の具体的な手順:

1. `CSoundDevice::CHATTR` の `FMVOICE voice` → `FmVoice voice` に変更
2. `CSoundDevice` に `VoiceProcessor proc_[MAX_CHS]` を追加
3. `SetVoice()` で `proc_[ch].onNoteOn(...)` を呼ぶように変更
4. `TimerCallBack()` で `proc_[ch].onTick(voice)` の結果を使う
5. 各チップドライバの `UpdateVoice()` でフィールドアクセスを変換
6. `UpdateVolExp()` を削除し、`proc_[ch].effectiveTL(op)` を参照するように変更
7. `UpdateOpLFO()` を削除し、`TimerCallBack` の `tlUpdateMask` を使うように変更
