# HW I/F プラグイン要件定義

**対象ヘッダ**: `plugin_sdk/include/fitom/IHWPlugin.h`  
**実装リポジトリ**: FitomIFTest (物理HW) / fitom_fmhwif (FMエンジン内蔵)  
**プラグイン種別**: ハードウェアインターフェース

---

## 概要

FITOM コアは `IHWPlugin` C API を通じてすべてのオーディオデバイスを制御する。
物理チップ（SPFM / RE1MB 等）と FM エンジンエミュレーターの両方が
この同一インターフェースを実装するため、FITOM コアからは区別なく扱える。

```
FITOM core
  └── HWPort (IPort アダプター)
        └── IHWPlugin C API
              ├── fitom_hw.dll       ← 物理チップ (FitomIFTest)
              └── fitom_fmhwif.dll   ← FMエンジン内蔵 + RtAudio (将来実装)
```

### fitom_fmhwif について

`fitom_fmhwif.dll` は複数の FM エンジン DLL を内部で束ね、
PCM ミックスと RtAudio によるオーディオ出力まで完結させる hwif 互換 DLL。
FITOM コアは RtAudio に依存せず、`HWPlugin_Write` を呼ぶだけでよい。

**注**: fitom_fmhwif は現在設計段階。旧来の `FmEnginePort` + `AudioEngine` は廃止。

---

## エクスポート関数一覧

### プラグイン情報

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetName` | `const char* ()` | プラグイン名を返す（例: `"FitomIFTest"`, `"fitom_fmhwif"`） |

### デバイス列挙

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Enumerate` | `const char* ()` | 接続済みデバイスを JSON 配列で返す。呼び出し元が `HWPlugin_FreeString` で解放する |
| `HWPlugin_FreeString` | `void (const char*)` | `HWPlugin_Enumerate` が返した文字列を解放する |

**`HWPlugin_Enumerate` の返却フォーマット（物理 HW）:**

```json
[
  { "type": "RE1",        "serial": "ABCD1234", "index": 0 },
  { "type": "RE4",        "serial": "EFGH5678", "index": 0 },
  { "type": "SPFM_TOWER", "port":   "COM3",     "index": 0 },
  { "type": "SPFM_LIGHT", "port":   "COM4",     "index": 0 }
]
```

**`HWPlugin_Enumerate` の返却フォーマット（fitom_fmhwif）:**

```json
[
  { "type": "FMHWIF", "engine": "YMEngine", "chip": "OPM",  "index": 0 },
  { "type": "FMHWIF", "engine": "YMEngine", "chip": "OPNA", "index": 1 }
]
```

| フィールド | 型 | 説明 |
|---|---|---|
| `type` | string | `FMHWIF` 固定 |
| `engine` | string | FMエンジン DLL 名（`YMEngine`, `AYEngine` 等） |
| `chip` | string | チップ種別（`OPM`, `OPNA`, `OPN2`, `OPL3` 等） |
| `index` | number | 同種チップが複数ある場合のインデックス |

### デバイス開閉

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Open` | `HWResult (const char* params_json, HWHandle* out)` | デバイスを開く |
| `HWPlugin_Close` | `void (HWHandle)` | デバイスを閉じてリソースを解放する |

**`params_json` フォーマット（物理 HW）:**

```json
{
  "type":   "SPFM_TOWER",
  "port":   "COM3",
  "slot":   0,
  "clock":  7987200,
  "pan":    0
}
```

**`params_json` フォーマット（fitom_fmhwif）:**

```json
{
  "type":         "FMHWIF",
  "engine":       "YMEngine",
  "chip":         "OPM",
  "clock":        3579545,
  "sample_rate":  44100,
  "buffer_frames": 512,
  "audio_api":    "auto"
}
```

| フィールド | 型 | 省略 | 説明 |
|---|---|---|---|
| `type` | string | 必須 | RE1 / RE4 / SPFM_TOWER / SPFM_LIGHT / FMHWIF |
| `serial` | string | 省略時 index=0 | RE1/RE4: シリアル番号 |
| `port` | string | 省略時 自動選択 | SPFM 系: COMポート |
| `slot` | number | 省略時 0 | SPFM 系のスロット番号 |
| `clock` | number | 省略時 0 | チップのマスタークロック [Hz] |
| `pan` | number | 省略時 0 | パノラマ。0=Stereo, 1=L only, 2=R only |
| `engine` | string | FMHWIF必須 | FMエンジン DLL 名 |
| `chip` | string | FMHWIF必須 | チップ種別 |
| `sample_rate` | number | 省略時 44100 | サンプルレート [Hz]（FMHWIF のみ） |
| `buffer_frames` | number | 省略時 512 | バッファサイズ [samples]（FMHWIF のみ） |
| `audio_api` | string | 省略時 "auto" | RtAudio API 名（FMHWIF のみ）|

### I/O

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Write` | `HWResult (HWHandle, uint16_t addr, uint8_t data)` | 1バイト書き込み |
| `HWPlugin_WriteBlock` | `HWResult (HWHandle, uint8_t startAddr, const uint8_t* data, size_t len)` | 連続書き込み |
| `HWPlugin_Reset` | `HWResult (HWHandle, unsigned int pulse_us)` | ハードウェアリセット |

### メタ情報

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetClock` | `int (HWHandle)` | マスタークロック [Hz] を返す |
| `HWPlugin_GetPanpot` | `int (HWHandle)` | パノラマ設定値を返す |
| `HWPlugin_IsOpen` | `bool (HWHandle)` | デバイスが使用可能か返す |

### レイテンシ同期（オプション）

物理チップと FM エンジン内蔵 hwif を混在使用する場合、
発音タイミングを揃えるためにレイテンシ同期が必要になる。
以下の2関数は**実装任意**。未実装の場合 FITOM は旧 DLL と互換動作する。

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetLatencySamples` | `uint32_t (HWHandle)` | `write()` から実際の発音までのレイテンシ [samples] を返す |
| `HWPlugin_SetDelaySamples` | `void (HWHandle, uint32_t)` | FITOM が算出した基準遅延量 [samples] を設定する |

**実装ガイド:**

```c
// fitom_fmhwif: バッファサイズ = 自身のレイテンシ
uint32_t HWPlugin_GetLatencySamples(HWHandle h) {
    return h->buffer_frames;   // 例: 512
}
// fitom_fmhwif: 自身のレイテンシと一致するため通常は何もしなくてよい
void HWPlugin_SetDelaySamples(HWHandle h, uint32_t delay) {
    (void)delay;  // buffer_frames == delay になるよう設定済みのため no-op
}

// 物理チップ (fitom_hw): 即時デバイスなので 0 を返す
uint32_t HWPlugin_GetLatencySamples(HWHandle h) {
    return 0;
}
// 物理チップ: FITOM から基準遅延を受け取り、write() をキューイングして遅らせる
void HWPlugin_SetDelaySamples(HWHandle h, uint32_t delay) {
    h->delay_samples = delay;
    // 以降の HWPlugin_Write() はタイムスタンプと共にキューに積み、
    // 別スレッドで delay_samples 後に実際の USB 転送を行う
}
```

**FITOM コア側の同期処理:**

`CFITOM::initDevices()` が全デバイスに対して以下を実行する。

```
1. 全 HWPort の GetLatencySamples() を収集 → max_latency を決定
2. 全 HWPort に SetDelaySamples(max_latency) を設定
```

これにより「物理チップ + FMエンジン内蔵hwif」混在構成でも
ノート ON/OFF のタイミングが一致する。

---

## アドレス変換規則

`HWPlugin_Write(handle, addr, data)` の `addr` は以下のように解釈する。

```
addr 上位 8bit (addr >> 8)   → a_high（SPFM 拡張アドレス / OPL3 Array1 等）
addr 下位 8bit (addr & 0xFF) → チップレジスタアドレス
```

`SplitPort` が2ポートチップ（OPNA / OPN2 / OPL3）の
`addr >= 0x100` を自動的に a_high=1 に変換するため、
DLL は addr の解釈のみ実装すればよい。

---

## エラーコード

| コード | 値 | 説明 |
|---|---|---|
| `HW_OK` | 0 | 成功 |
| `HW_ERR_NOT_FOUND` | -1 | 指定デバイスが見つからない |
| `HW_ERR_OPEN_FAILED` | -2 | デバイスのオープンに失敗 |
| `HW_ERR_IO` | -3 | I/O エラー |
| `HW_ERR_INVALID_ARG` | -4 | 引数不正（type 不明、JSON 解析失敗等） |

---

## スレッドセーフ要件

| 関数 | 要件 |
|---|---|
| `HWPlugin_Write`, `HWPlugin_WriteBlock` | **スレッドセーフ必須**。FITOM の MIDI 処理スレッドから並行呼び出しが発生する |
| `HWPlugin_Open`, `HWPlugin_Close`, `HWPlugin_Reset` | 初期化・終了フェーズのみ。スレッドセーフ不要 |
| `HWPlugin_GetLatencySamples` | 初期化フェーズのみ呼ばれる。スレッドセーフ不要 |
| `HWPlugin_SetDelaySamples` | 初期化フェーズのみ呼ばれる。スレッドセーフ不要 |

---

## fitom_hw.dll のビルド（FitomIFTest）

既存のヘッダオンリーターゲット（RE1Controller 等）はそのまま残し、
`IHWPlugin.h` を実装した共有ライブラリターゲットを追加する。

**追加ファイル:** `FitomIFTest/src/HWPluginImpl.cpp`

```cmake
add_library(fitom_hw SHARED src/HWPluginImpl.cpp)
target_compile_definitions(fitom_hw PRIVATE FITOM_HW_PLUGIN_EXPORTS)
target_include_directories(fitom_hw PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    path/to/fitom/plugin_sdk/include
)
target_link_libraries(fitom_hw PRIVATE
    RE1Controller RE4Controller SPFMController FTDIDeviceInfo
    Boost::system nlohmann_json::nlohmann_json
)
```

**実装する関数:**
- `HWPlugin_GetName / Enumerate / FreeString / Open / Close`
- `HWPlugin_Write / WriteBlock / Reset`
- `HWPlugin_GetClock / GetPanpot / IsOpen`
- `HWPlugin_GetLatencySamples` — `return 0;`（物理チップは即時）
- `HWPlugin_SetDelaySamples` — キューイング遅延の実装

---

## fitom_fmhwif.dll のビルド（将来実装）

FM エンジン DLL（YMEngine 等）を複数ロードし、PCM ミックスと
RtAudio によるオーディオ出力を内部で完結させる hwif 互換 DLL。

```cmake
add_library(fitom_fmhwif SHARED src/FmHwIfImpl.cpp)
target_compile_definitions(fitom_fmhwif PRIVATE FITOM_HW_PLUGIN_EXPORTS)
target_link_libraries(fitom_fmhwif PRIVATE
    rtaudio
    nlohmann_json::nlohmann_json
)
```

**実装する関数（物理HWと同じ IHWPlugin.h を実装）:**
- `HWPlugin_GetLatencySamples` — `return buffer_frames;`
- `HWPlugin_SetDelaySamples` — 通常 no-op（自身のバッファ=基準レイテンシ）
- `HWPlugin_Write` — 内部の FM エンジンにレジスタ書き込みを転送

---

## ビルド要件

| 項目 | 要件 |
|---|---|
| 言語 | C++17 以上 |
| 依存（物理HW） | libftdi1（RE1/RE4）、Boost.Asio（SPFM 系） |
| 依存（fmhwif） | RtAudio、FMエンジン DLL |
| DLL 名 | 任意（`fitom.conf.json` の `hw_plugin.dll` で指定） |
| エクスポート | `extern "C"` で C リンケージを保つこと |
| プラットフォーム | Windows / Linux / macOS |

---

## config 記述例

```json
"hw_plugin": {
  "dll": "fitom_hw.dll"
}
```

```json
"hw_plugin": {
  "dll": "fitom_fmhwif.dll"
}
```

デバイスの具体的な接続設定はプロファイルの `devices` セクションに記述する。
