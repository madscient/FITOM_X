# MIDI プラグイン要件定義

**対象ヘッダ**: `plugin_sdk/include/fitom/IMidiPlugin.h`  
**実装バリアント**: Windows MIDI Services (`fitom_midi_wms.dll`) / ALSA (`fitom_midi_alsa.so`)  
**プラグイン種別**: MIDI 入出力（同時ロードは 1 DLL のみ）

---

## 概要

WinMM・Windows MIDI Services・ALSA・CoreMIDI などの MIDI 実装の差異を  
FITOM コアから完全に隠蔽するプラグイン。  
コアは MIDI 1.0 バイト列のみを扱い、プロトコル変換（UMP ↔ MIDI 1.0 等）は DLL 内部で行う。

---

## エクスポート関数一覧

### プラグイン情報

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `MidiPlugin_GetName` | `const char* ()` | プラグイン名を返す。例: `"WindowsMIDIServices"`, `"ALSA"` |

### デバイス列挙

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `MidiPlugin_EnumerateIn` | `const char* ()` | 利用可能な MIDI In デバイス名の JSON 配列を返す |
| `MidiPlugin_EnumerateOut` | `const char* ()` | 利用可能な MIDI Out デバイス名の JSON 配列を返す |
| `MidiPlugin_FreeString` | `void (const char*)` | Enumerate 系が返した文字列を解放する |

**返却フォーマット（JSON 配列）:**

```json
["MIDI キーボード", "Virtual MIDI Bus 1", "UM-ONE"]
```

デバイス名は `MidiPlugin_OpenIn` / `MidiPlugin_OpenOut` の `device_name` として使用する。

### MIDI In

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `MidiPlugin_OpenIn` | `MidiResult (const char* device_name, MidiInCallback callback, void* user_data, MidiInHandle* out)` | 指定デバイスを開いてコールバックを登録する |
| `MidiPlugin_CloseIn` | `void (MidiInHandle)` | MIDI In デバイスを閉じる |

### MIDI Out

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `MidiPlugin_OpenOut` | `MidiResult (const char* device_name, MidiOutHandle* out)` | 指定デバイスを開く |
| `MidiPlugin_CloseOut` | `void (MidiOutHandle)` | MIDI Out デバイスを閉じる |
| `MidiPlugin_Send` | `MidiResult (MidiOutHandle, const uint8_t* data, size_t len, uint64_t timestamp_ns)` | MIDI メッセージを送信する |

---

## コールバック仕様

```c
typedef void (*MidiInCallback)(
    const uint8_t* data,       // MIDI 1.0 バイト列 (status [+ data1 [+ data2]])
    size_t         len,        // バイト数 (1〜3、または SysEx の場合はそれ以上)
    uint64_t       timestamp_ns, // 受信タイムスタンプ [ナノ秒]。不明な場合は 0
    void*          user_data   // MidiPlugin_OpenIn に渡した値
);
```

- コールバックは DLL 内部のスレッドから呼ばれる
- コールバック内での重い処理・ブロッキング呼び出しは禁止
- FITOM コアはコールバック内で MIDI メッセージを内部キューに積む設計とする
- SysEx はひとまとまりで渡すこと（分割禁止）

---

## メッセージフォーマット

**MIDI 1.0 バイト列で統一する。**  
プラグイン内部で UMP など他のフォーマットが使われる場合は、  
コールバック呼び出し前・`MidiPlugin_Send` 後にプラグイン側で変換する。

| メッセージ種別 | len | フォーマット |
|---|---|---|
| ノートオン/オフ | 3 | `[9x/8x nn vv]` |
| コントロールチェンジ | 3 | `[Bx cc vv]` |
| プログラムチェンジ | 2 | `[Cx pp]` |
| ピッチベンド | 3 | `[Ex ll mm]` |
| SysEx | 可変 | `[F0 ... F7]` |
| タイムコード等 | 1〜2 | 実装依存（可能な範囲で対応） |

---

## エラーコード

| コード | 値 | 説明 |
|---|---|---|
| `MIDI_OK` | 0 | 成功 |
| `MIDI_ERR_NOT_FOUND` | -1 | 指定デバイスが見つからない |
| `MIDI_ERR_OPEN_FAILED` | -2 | デバイスのオープンに失敗 |
| `MIDI_ERR_IO` | -3 | 送受信エラー |
| `MIDI_ERR_UNAVAILABLE` | -4 | プラグイン自体が使用不可（WMS Runtime 未インストール等） |

---

## Windows MIDI Services 実装固有の要件と制約

### ビルド環境
- Visual Studio 2022 以上（C++/WinRT のプロジェクション生成に使用）
- Windows SDK 10.0.20348 以上
- NuGet パッケージ（ローカルフィード経由）:
  - `Microsoft.Windows.Devices.Midi2`
  - `Microsoft.Windows.CppWinRT`
- **C++20** で記述（コア本体の C++17 とは独立して問題ない）
- **64bit のみ**（x86/ARM32 は非対応）

### 実行時要件
- [Windows MIDI Services Runtime](https://github.com/microsoft/MIDI/releases) の別途インストールが必要
- 未インストール時は `MidiPlugin_OpenIn` / `MidiPlugin_OpenOut` が `MIDI_ERR_UNAVAILABLE` を返す
- この場合 FITOM コアは警告ログを出力し、MIDI 入力なしで起動を継続する

### UMP 変換
- WMS は全メッセージを UMP (Universal MIDI Packet) で処理する
- MIDI 1.0 デバイスからの UMP32 (Type 2) → MIDI 1.0 バイト列に DLL 内で変換する
- `MidiPlugin_Send` では MIDI 1.0 バイト列 → UMP32 に変換して送信する
- `timestamp_ns` が 0 以外の場合、WMS のスケジュール送信機能を使用する

### マルチクライアント
- WMS はネイティブにマルチクライアントをサポートする
- 複数アプリが同一デバイスに接続しても競合しない

---

## ALSA 実装固有の要件と制約

### ビルド環境
- Linux のみ
- `libasound2-dev`（`alsa/asoundlib.h`）

### デバイス名フォーマット
- ALSA シーケンサのクライアント名とポート名を `:` で連結した文字列を使用する
- 例: `"Midi Through:Midi Through Port-0"`, `"JUNO-DS:JUNO-DS MIDI 1"`

### タイムスタンプ
- ALSA シーケンサはイベントのタイムスタンプを提供するが精度はカーネルスケジューラに依存する
- `timestamp_ns` に設定できる場合はそうする。困難な場合は `0` を渡してよい

### `MidiPlugin_Send` の `timestamp_ns`
- ALSA シーケンサはスケジュール送信をサポートするが実装は任意
- `timestamp_ns = 0` 以外を受け取った場合、即時送信として扱っても構わない

---

## config 記述例

```json
"midi_plugin": {
  "dll": "fitom_midi_wms.dll"
}
```

使用するデバイス名はプロファイルの `midi_inputs` / `midi_outputs` セクションに記述する（後述）。
