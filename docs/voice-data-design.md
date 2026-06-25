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
└── FmChipExt   ext         チップ固有拡張 (OPZ: REV/EGS/DM0/DT3)
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
OPN/OPM/OPL 等は `ext` を参照しない。

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

| パラメータ | 効果 | 値の解釈 |
|---|---|---|
| `VTL` | ベロシティ低下時に TL を増加（音量を下げる） | 0=無効, 127=最大感度 |
| `VAR` | ベロシティ → AR の補正（現状未実装） | 将来拡張用 |
| `VDR` | ベロシティ → DR の補正（現状未実装） | 将来拡張用 |
| `VLD` | ベロシティ → ソフトLFO Depth の補正（現状未実装） | 将来拡張用 |
| `VLR` | ベロシティ → ソフトLFO Rate の補正（現状未実装） | 将来拡張用 |

現時点で実装されているのは `VTL` のみ（TL への感度）。
他のパラメータはボイスデータとして保持するが処理は将来実装。

---

## ソフト LFO パラメータの意味

### オペレータ単位 (FmSwOp) — トレモロ効果

| パラメータ | 意味 |
|---|---|
| `SLF` | 波形速度インデックス (0〜18: speedstep テーブル参照) |
| `SLW` | 波形種別 (0=上昇のこぎり/1=矩形/2=三角/3=S&H/4=下降のこぎり/5=デルタ/6=サイン) |
| `SLD` | 深さ (0〜63 / 64〜127 → -64〜-1 の符号付き) |
| `SLY` | 遅延 [20ms 単位] |
| `SLR` | レート (0=LFO 無効) |

`SLR=0` のとき LFO は動作しない（チェック後に `opLfo` を開始しない）。

### チャンネル単位 (FmSwVoice) — ビブラート効果

| パラメータ | 意味 |
|---|---|
| `LFO` | 波形速度インデックス |
| `LWF` | 波形種別 (オペレータLFO と同じ) |
| `LFS` | NoteOn 時に位相リセットするか (1=リセット) |
| `LFD` | 遅延 [20ms 単位] |
| `LFR` | レート (0=LFO 無効) |
| `LDM/LDL` | 深さ (16bit 符号付き: -8192〜+8191, 0=無変調) |

ピッチ変調量は `VoiceProcessor::channelLfoValue()` として取得し、
`CSoundDevice::UpdateFnumber()` の `offset` 引数に加算する。

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
