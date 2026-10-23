# MIDI プラグイン要件定義

**対象読者**: MIDI バックエンド DLL を実装する開発者  
**契約の正本**: `plugin_sdk/include/fitom/IMidiPlugin.h`  
**同梱の実装**:
- RtMidi (`fitom_midi_rtmidi.dll` / `.so` / `.dylib`、Windows/Linux/macOS共通)。プロファイルの`midi_backend`で指定する
- 内部用MIDIパイプ (`fitom_midi_pipe`)。プロファイルとは無関係に、DLLがあれば追加でロードされる。仕様は[plugin-midi-pipe.md](plugin-midi-pipe.md)

---

## 概要

WinMM・ALSA・CoreMIDI などの MIDI 実装の差異を  
FITOM コアから完全に隠蔽するプラグイン。  
コアは MIDI 1.0 バイト列のみを扱う。同梱の実装は [RtMidi](https://github.com/thestk/rtmidi)
(`third_party/rtmidi`、git submodule)に委譲し、Windows/Linux/macOSを1つの
DLLソースでカバーする。

---

## エクスポート関数一覧

すべて必須。コアはロード時に全関数の存在を確かめ、1つでも欠けていればロードに失敗する。

### プラグイン情報

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `MidiPlugin_GetName` | `const char* ()` | プラグイン名を返す。例: `"RtMidi"` |

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

現在のコアは MIDI Out を使わない(ロード時に関数の存在を確かめるだけで、呼び出さない)。

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
- FITOM コアはコールバックのスレッド上でロックを取り、受け取ったバイト列をその場で処理してから戻る。処理中はタイマー処理や他のポートからの入力と直列化されるため、コールバックの呼び出しが詰まるとMIDI処理全体が止まる。プラグイン側でコールバックを呼ぶスレッドに、他の重い処理を載せないこと
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
| `MIDI_ERR_UNAVAILABLE` | -4 | 対応するMIDI APIが利用不可（未対応OS等） |

---

## RtMidi 実装固有の要件と制約

**実装ファイル**: `backends/midi_rtmidi/src/MidiRtMidi.cpp`

### ビルド環境
- [RtMidi](https://github.com/thestk/rtmidi) 本体を `third_party/rtmidi` に git submodule として配置する
  (`git submodule update --init --recursive`)
- RtMidi自身が `cmake_minimum_required(VERSION 3.24)` を要求するため、
  ビルド環境側も CMake 3.24 以上が必要
- RtMidiは静的リンクする(`fitom_midi_rtmidi.dll`/`.so`/`.dylib` 単体で完結し、
  追加のランタイムDLLインストールは不要)
- プラットフォームAPI選択はRtMidi側が自動判定する: Windows→WinMM、
  Linux→ALSA、macOS→CoreMIDI（いずれもコンパイル時に決まり、実行時の
  切り替えはない）

### デバイス名フォーマット
- `RtMidiIn::getPortName()` / `RtMidiOut::getPortName()` が返す文字列を
  そのまま使う。フォーマットはプラットフォームのMIDI API依存
  (例: Windows/macOSはデバイス名そのもの、LinuxのALSAシーケンサは
  `"クライアント名:ポート名"`)

### タイムスタンプ
- `MidiPlugin_OpenIn` のコールバックに渡される `timestamp_ns` は常に `0`
  (不明)を返す。RtMidiのコールバックは「前回イベントからの相対デルタ秒」
  しか提供せず、`IMidiPlugin.h` が要求する絶対受信タイムスタンプには
  変換できないため
- `MidiPlugin_Send` の `timestamp_ns` はスケジュール送信機構が無いため無視し、
  常に即時送信として扱う

### SysEx
- `RtMidiIn::ignoreTypes(false, ...)` / `RtMidiOut::sendMessage()` により、
  通常メッセージと同じAPIでSysExを一塊のまま送受信できる

---

## config 記述例

プロファイル(`*.profile.json`)の `midi_backend.dll` で指定する:

```json
"midi_backend": {
  "dll": "fitom_midi_rtmidi.dll"
}
```

開く MIDI In のデバイス名は、プロファイルの `midi_inputs` に書く。
