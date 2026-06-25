# FmEngine プラグイン要件定義

**対象ヘッダ**: `plugin_sdk/include/fitom/IFmEnginePlugin.h`  
**ベース仕様**: `FmEngineApi.h` (FMEngineTest プロジェクト)  
**プラグイン種別**: FM音源エミュレーター（複数 DLL の同時ロード可）

---

## 概要

FM音源チップのソフトウェアエミュレーターを FITOM コアに接続するためのプラグイン。  
既存 `FmEngineApi.h` に準拠した DLL に、識別用の `FmEngine_GetEngineName()` を 1 関数追加することで対応できる。  
複数の FmEngine 互換 DLL（例: YM 系と PSG 系）を同時にロードして異なるチップに割り当てることができる。

---

## エクスポート関数一覧

### 既存 FmEngineApi.h から継承（変更なし）

| 関数 | 説明 |
|---|---|
| `FmEngine_Create(sample_rate)` | エンジンインスタンスを生成する |
| `FmEngine_Destroy(engine)` | エンジンインスタンスを破棄する |
| `FmEngine_Inquiry(engine)` | 対応チップ総数を返す |
| `FmEngine_GetSupportedChip(engine, index)` | index 番目のチップ名を返す（例: `"OPNA"`, `"OPL2"`） |
| `FmEngine_AddChip(engine, name, clock, out_id)` | チップを追加する。未知の名前は `FM_ERR_UNKNOWN_CHIP` |
| `FmEngine_GetChipName(engine, chip_id)` | 追加済みチップ名を返す |
| `FmEngine_GetNativeRate(engine, chip_id)` | チップのネイティブサンプルレートを返す |
| `FmEngine_GetSampleRate(engine)` | エンジン全体のサンプルレートを返す |
| `FmEngine_Write(engine, chip_id, reg, value, port)` | レジスタ書き込み（スレッドセーフ要件あり） |
| `FmEngine_SetGain(engine, chip_id, gain_l, gain_r)` | ゲイン設定（スレッドセーフ要件あり） |
| `FmEngine_GetGain(engine, chip_id, out_l, out_r)` | ゲイン取得 |
| `FmEngine_SetMemory(engine, chip_id, type, data, size)` | ADPCM/PCM ROM/RAM 設定 |
| `FmEngine_GetMemorySize(engine, chip_id, type)` | メモリサイズ取得 |
| `FmEngine_Generate(engine, out_l, out_r, samples)` | 波形生成（オーディオスレッドから呼ぶ） |

### FITOM が追加で要求する関数（新規）

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `FmEngine_GetEngineName` | `const char* ()` | エンジン識別名を返す（例: `"YMEngine"`, `"AYEngine"`）。config の `name` フィールドと一致させること。 |

---

## スレッドセーフ要件

| 関数グループ | 要件 |
|---|---|
| `FmEngine_Write`, `FmEngine_SetGain`, `FmEngine_GetGain` | **スレッドセーフ必須**。FITOM コアの MIDI 処理スレッドとオーディオコールバックスレッドが並行して呼ぶ場合がある。 |
| `FmEngine_Generate` | **オーディオコールバックスレッドのみから呼ぶ**。他のスレッドとの同時呼び出し禁止。 |
| `FmEngine_SetMemory` | **オーディオストリーム開始前に呼ぶこと**。ストリーム実行中の呼び出しは未定義動作。 |
| `FmEngine_AddChip`, `FmEngine_Create`, `FmEngine_Destroy` | 初期化・終了フェーズのみ。スレッドセーフ不要。 |

---

## エクスポート属性

プラットフォームごとに以下の属性でエクスポートすること。

```c
// Windows
#define FMENGINE_API  __declspec(dllexport)
#define FMENGINE_CALL __cdecl

// Linux / macOS
#define FMENGINE_API  __attribute__((visibility("default")))
#define FMENGINE_CALL
```

---

## アドレス変換規則

FITOM コアは `IPort::write(addr, data)` のアドレスを以下のように分解して渡す。

```
FmEngine_Write(engine, chip_id,
    reg   = addr & 0xFF,      // レジスタアドレス
    value = data & 0xFF,
    port  = (addr >> 8) & 0xFF  // ポート番号 (OPN 系 port0/1 等)
)
```

チップドライバの既存の慣習（アドレス上位バイト = ポート番号）に合わせた規則なので、実装側もこれに準拠すること。

---

## チップ名文字列の規則

- **大文字小文字を区別する**
- FITOM コアが認識するチップ名は `FITOMdefine.h` の `DEVICE_*` 定数に対応する文字列
- 推奨チップ名（例）:

| 文字列 | チップ | DEVICE_ID |
|---|---|---|
| `"OPN"` | YM2203 | DEVICE_OPN |
| `"OPN2"` | YM2612 | DEVICE_OPN2 |
| `"OPNA"` | YM2608 | DEVICE_OPNA |
| `"OPM"` | YM2151 | DEVICE_OPM |
| `"OPLL"` | YM2413 | DEVICE_OPLL |
| `"OPL"` | YM3526 | DEVICE_OPL |
| `"OPL2"` | YM3812 | DEVICE_OPL2 |
| `"OPL3"` | YMF262 | DEVICE_OPL3 |
| `"SSG"` | YM2149 | DEVICE_SSG |
| `"PSG"` | AY-3-891x | DEVICE_PSG |
| `"DCSG"` | SN76489 | DEVICE_DCSG |
| `"SCC"` | YM2212 | DEVICE_SCC |

---

## ビルド要件

- 言語: C++17 以上
- 依存: ymfm 等の内部実装ライブラリ（コアとの共有不要）
- DLL 名: 任意（config の `dll` フィールドで指定する）
- エクスポート: `extern "C"` で C リンケージを保つこと
- 出力: 実行ファイルと同ディレクトリ、または config 指定パスに配置

---

## config 記述例

```json
"fm_engines": [
  {
    "name":   "YMEngine",
    "dll":    "YMEngine.dll",
    "sample_rate": 48000
  },
  {
    "name":   "AYEngine",
    "dll":    "AYEngine.so",
    "sample_rate": 48000
  }
]
```

`name` は `FmEngine_GetEngineName()` の戻り値と一致する必要がある。  
一致しない場合は起動時にエラーログを出力してロードを中断する。
