# チップドライバ移行ガイド

## 移行パターン

`OPN_new.cpp` が示すパターンをすべてのチップドライバに適用する。

### 必要な変更箇所

#### 1. クラス継承の変更

```cpp
// 旧
class COPN : public CSoundDevice { ... };

// 新
class COPN : public fitom::CSoundDevice { ... };
// コンストラクタ引数が変わる:
//   旧: CSoundDevice(devid, maxchs, fmas, devide, offset, ftype, pt, regsize)
//   新: CSoundDevice(deviceType, maxChs, port, fnumMaster, fnumDivide, noteOffset, fnumType, regSize)
```

#### 2. UpdateVoice: FMVOICE → HwPatch

```cpp
// 旧
void UpdateVoice(uint8_t ch) override {
    FMVOICE* voice = GetChAttribute(ch)->GetVoice();
    SetReg(0xB0 + ch, (voice->FB << 3) | (voice->AL & 7));
    for (int op = 0; op < ops; op++) {
        SetReg(0x40 + opSlot(ch, op), voice->op[op].TL); // ← TL を直接使用
    }
}

// 新
void updateVoice(uint8_t ch) override {
    const HwPatch& p = chState_[ch].hwPatch;
    setReg(0xB0 + ch, (p.hw.FB << 3) | (p.hw.ALG & 7));
    for (int op = 0; op < 4; op++) {
        uint8_t tl = isCarrier(ch, op)
            ? chState_[ch].proc.effectiveTL(op)  // ← VoiceProcessor が算出
            : p.hwOp[op].TL;
        setReg(0x40 + opSlot(ch, op), tl & 0x7F);
    }
}
```

#### 3. UpdateVolExp → updateVolExp

```cpp
// 旧: UpdateVolExp が CalcLinearLevel + UpdateTL を直接行う
void UpdateVolExp(uint8_t ch) override {
    CHATTR* attr = GetChAttribute(ch);
    FMVOICE* voice = attr->GetVoice();
    for (int op = 0; op < ops; op++) {
        if (IsCarrier(ch, op)) {
            uint8_t tl = CalcLinearLevel(evol, voice->op[op].TL);
            UpdateTL(ch, op, tl);
        }
    }
}

// 新: VoiceProcessor が effectiveTL を持っているため読むだけ
void updateVolExp(uint8_t ch) override {
    for (int op = 0; op < 4; op++) {
        if (!isCarrier(ch, op)) continue;
        setReg(0x40 + opSlot(ch, op),
               chState_[ch].proc.effectiveTL(op) & 0x7F);
    }
}
```

#### 4. UpdateOpLFO → 削除

```cpp
// 旧: TimerCallBack → UpdateOpLFO → UpdateTL
// 新: CSoundDevice::timerCallback が proc.onTick() → tlUpdateMask → updateTL を呼ぶ
// チップドライバ側の UpdateOpLFO は不要になる
```

#### 5. TCHAR/LPCTSTR/GetDescriptor

```cpp
// 旧
void GetDescriptor(TCHAR* str, size_t len) override {
    StringCchPrintf(str, len, _T("%02X:OPN (%ich)"), device, chs);
}

// 新
std::string getDescriptor() const override {
    return "OPN (YM2203) 3ch";
}
```

#### 6. NoteOffset/MasterTune

旧の `NoteOffset` は `CSoundDevice` 内の `noteOffset_` として引き継がれる。
チップ固有の値はコンストラクタで指定する。

```cpp
// 旧: NoteOffset = -576 がデフォルト (CSoundDevice コンストラクタで設定)
// OPN2 など別のオフセットを使うチップ:
COPN2::COPN2(IPort* port)
    : CSoundDevice(DEVICE_OPN2, 6, port,
                   7670454, 144,
                   -576,    // ← ここで指定
                   ...) {}
```

---

## チップ別の移行チェックリスト

### FM 系 (4OP)

| ファイル | 優先度 | 特記事項 |
|---|---|---|
| `OPN_new.cpp` | 完了 | 移行パターンのリファレンス |
| `OPM.cpp` | 高 | DT2 / AMS / PMS あり / KeyCode テーブル |
| `OPN2.cpp` | 高 | 6ch / CH3拡張モード |
| `OPL.cpp` | 中 | 2OP / ALG 1bit / KSR 1bit / KSL / VIB |
| `OPL3.cpp` | 中 | 4OP / OPL3 拡張レジスタ |
| `OPLL.cpp` | 中 | 2OP / 内蔵音色 |
| `OPK.cpp` | 低 | OPK 固有 |
| `MAx.cpp` | 低 | MA-3/MA-5 固有 |

### PSG / SSG 系

| ファイル | 特記事項 |
|---|---|
| `SSG.cpp` | FnumTableType::SSG / 3音源 |
| `PSGBase.cpp` | SSG 基底クラス |
| `DCSG.cpp` | SN76489 / PSG base |
| `SCC.cpp` | SCC 波形テーブルの扱い |
| `SAA.cpp` | SAA1099 固有 |
| `EPSG.cpp` | AY-3-8910 拡張 |

### ADPCM / PCM 系

| ファイル | 特記事項 |
|---|---|
| `ADPCM.cpp` | YM2608 ADPCM-A/B |
| `YMDeltaT.cpp` | ADPCM-B (Delta-T) |
| `PCMD8.cpp` | PCM ドライバ |

### マルチデバイス

| ファイル | 特記事項 |
|---|---|
| `MultiDev.cpp` | `CMultiDevice::ISoundDevice` 移行 |
| `SpanDev.cpp` | `CSpanDevice` — `AllocCh` の委譲ロジック |
| `Unison.cpp` | `CUnison` — 全 ch 同時発音 |
| `Stereo.cpp` | ステレオ L/R デバイス |

---

## 実際の移行手順 (1ファイルずつ)

```
1. ファイルを core/src/ にコピー済み (完了)
2. #include "STDAFX.h" → #include "fitom/fitom_core.h" に変更
3. クラス宣言を fitom:: 名前空間に入れる
4. CSoundDevice の継承変更 + コンストラクタ引数修正
5. UpdateVoice → updateVoice (小文字) + HwPatch アクセスに変更
6. UpdateVolExp → updateVolExp に変更 (effectiveTL 参照)
7. UpdateTL → updateTL に変更
8. UpdateFreq → updateFreq に変更
9. UpdatePanpot → updatePanpot に変更
10. UpdateSustain → updateSustain に変更
11. UpdateKey → updateKey に変更
12. GetDescriptor(TCHAR*, size_t) → getDescriptor() const (std::string)
13. UpdateOpLFO を削除 (CSoundDevice::timerCallback が代行)
14. core/CMakeLists.txt の FITOM_CORE_SOURCES に追加
15. ビルド & 動作確認
```

---

## `CPort*` → `fitom::IPort*` の移行

チップドライバのコンストラクタに渡す `port` は、
旧 `CPort*` から `fitom::IPort*` に変わる。

`FmEnginePort` / `HWPort` はどちらも `fitom::IPort` を実装しており、
チップドライバ側は型を変えるだけで動く。

```cpp
// 旧
COPNA opna(new CFT825Port(), DEVICE_OPNA);

// 新 (FitomIFTest HW バックエンド)
auto hwPort = std::make_unique<fitom::HWPort>(hwPlugin, paramsJson);
COPNA opna(hwPort.get(), DEVICE_OPNA);

// 新 (FmEngine エミュレーターバックエンド)
auto emuPort = std::make_unique<fitom::FmEnginePort>(engineInst, "OPNA", 0);
COPNA opna(emuPort.get(), DEVICE_OPNA);
```
