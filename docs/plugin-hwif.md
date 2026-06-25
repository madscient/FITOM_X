# HW I/F プラグイン要件定義

**対象ヘッダ**: `plugin_sdk/include/fitom/IHWPlugin.h`  
**実装リポジトリ**: FitomIFTest  
**プラグイン種別**: ハードウェアインターフェース（同時ロードは 1 DLL のみ）

---

## 概要

RE1MB / RE4 (FT245R/FT2232H) や SPFM FM の塔 / SPFM Light などのハードウェアを  
FITOM コアに接続するためのプラグイン。  
FitomIFTest の `hw::HWControllerBase` クラス階層をそのまま使用し、  
C API (`IHWPlugin.h`) としてエクスポートした共有ライブラリをビルドすることで対応する。

---

## エクスポート関数一覧

### プラグイン情報

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetName` | `const char* ()` | プラグイン名を返す（例: `"FitomIFTest"`） |

### デバイス列挙

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Enumerate` | `const char* ()` | 接続済みデバイスを JSON 配列で返す。呼び出し元が `HWPlugin_FreeString` で解放する。 |
| `HWPlugin_FreeString` | `void (const char*)` | `HWPlugin_Enumerate` が返した文字列を解放する |

**`HWPlugin_Enumerate` の返却フォーマット:**

```json
[
  { "type": "RE1",        "serial": "ABCD1234", "index": 0 },
  { "type": "RE4",        "serial": "EFGH5678", "index": 0 },
  { "type": "SPFM_TOWER", "port":   "COM3",     "index": 0 },
  { "type": "SPFM_LIGHT", "port":   "COM4",     "index": 0 }
]
```

| フィールド | 型 | 説明 |
|---|---|---|
| `type` | string | デバイス種別。RE1 / RE4 / SPFM_TOWER / SPFM_LIGHT |
| `serial` | string | FT245R / FT2232H のシリアル番号（RE1/RE4 のみ） |
| `port` | string | COMポート名または `/dev/ttyUSB0` 等（SPFM 系のみ） |
| `index` | number | 同種デバイスが複数台ある場合のインデックス |

### デバイス開閉

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Open` | `HWResult (const char* params_json, HWHandle* out)` | デバイスを開く。params_json は後述のフォーマット。 |
| `HWPlugin_Close` | `void (HWHandle)` | デバイスを閉じてリソースを解放する |

**`params_json` フォーマット:**

```json
{
  "type":   "RE1",
  "serial": "ABCD1234",
  "slot":   0,
  "clock":  3579545,
  "pan":    0
}
```

| フィールド | 型 | 省略 | 説明 |
|---|---|---|---|
| `type` | string | 必須 | RE1 / RE4 / SPFM_TOWER / SPFM_LIGHT |
| `serial` | string | 省略時 index=0 | RE1/RE4: シリアル番号 |
| `port` | string | 省略時 自動選択 | SPFM 系: COMポート |
| `index` | number | 省略時 0 | インデックスで識別する場合に使用 |
| `slot` | number | 省略時 0 | SPFM 系のスロット番号 |
| `clock` | number | 省略時 0 | チップのマスタークロック [Hz]。0 は呼び出し元が別途管理 |
| `pan` | number | 省略時 0 | パノラマ。0=Stereo, 1=L only, 2=R only, 3=LR mono |

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
| `HWPlugin_IsOpen` | `bool (HWHandle)` | デバイスが使用可能かを返す |

---

## アドレス変換規則

`HWPlugin_Write(handle, addr, data)` の `addr` は以下のように解釈する。

```
addr 上位 8bit (addr >> 8)  → a_high（SPFM 拡張アドレス用）
addr 下位 8bit (addr & 0xFF) → チップレジスタアドレス（下位 4bit が有効）
```

これは `hw::HWControllerBase::write(slot, addr, data, a_high)` の呼び出しに対応する。  
`slot` は `HWPlugin_Open` 時の `params_json` の `slot` フィールドで固定される。

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
| `HWPlugin_Write`, `HWPlugin_WriteBlock` | **スレッドセーフ必須**。MIDI 処理スレッドから並行して呼ばれる場合がある。FT245R/FT2232H の USB バス排他は DLL 内部で行うこと。 |
| `HWPlugin_Open`, `HWPlugin_Close`, `HWPlugin_Reset` | 初期化・終了フェーズのみ。スレッドセーフ不要。 |

---

## FitomIFTest 側のビルド追加作業

既存のヘッダオンリーターゲット（RE1Controller 等）はそのまま残し、  
`IHWPlugin.h` を実装した共有ライブラリターゲットを追加する。

**追加するファイル:** `FitomIFTest/src/HWPluginImpl.cpp`  
**追加する CMake ターゲット:**

```cmake
add_library(fitom_hw SHARED src/HWPluginImpl.cpp)
target_compile_definitions(fitom_hw PRIVATE FITOM_HW_PLUGIN_EXPORTS)
target_include_directories(fitom_hw PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # HWControllerBase.hpp 等
    path/to/fitom/plugin_sdk/include     # IHWPlugin.h
)
target_link_libraries(fitom_hw PRIVATE
    RE1Controller RE4Controller SPFMController FTDIDeviceInfo
    Boost::system nlohmann_json::nlohmann_json)
```

---

## ビルド要件

- 言語: C++17 以上
- 依存: libftdi1（RE1/RE4）、Boost.Asio（SPFM 系）
- DLL 名: 任意（config の `dll` フィールドで指定する）
- プラットフォーム: Windows / Linux / macOS（RE1/RE4 は libftdi1 が必要）
- エクスポート: `extern "C"` で C リンケージを保つこと

---

## config 記述例

```json
"hw_plugin": {
  "dll": "fitom_hw.dll"
}
```

デバイスの具体的な接続設定はプロファイルの `devices` セクションに記述する（後述）。
