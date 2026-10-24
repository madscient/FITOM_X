# HW I/F プラグイン要件定義

**対象読者**: HW I/F プラグイン DLL を実装する開発者  
**契約の正本**: `plugin_sdk/include/fitom/IHWPlugin.h`  
**実装リポジトリ**:
- [FitomHwIF](https://github.com/madscient/FitomHwIF) (`fitom_hw.dll`) — 物理チップ (SPFM / RE1 / RE4)
- [FitomEmuIF](https://github.com/madscient/FitomEmuIF) (`FitomEmuIF.dll`) — FM エンジン内蔵(エミュレータ)
- [FitomSf2IF](https://github.com/madscient/FitomSf2IF) (`FitomSf2IF.dll`) — SoundFont 音源(fluidsynth)

---

## 概要

FITOM コアは `IHWPlugin` C API を通じてすべてのオーディオデバイスを制御する。
物理チップと FM エンジンエミュレーターの両方が
この同一インターフェースを実装するため、FITOM コアからは区別なく扱える。

```
FITOM core
  └── HWPort (IPort アダプター)
        └── IHWPlugin C API
              ├── fitom_hw.dll     ← 物理チップ (FitomHwIF)
              ├── FitomEmuIF.dll   ← FMエンジン内蔵 + 音声出力 (FitomEmuIF)
              └── FitomSf2IF.dll   ← SoundFont 音源 + 音声出力 (FitomSf2IF)
```

FM エンジン内蔵のプラグインは、複数の FM エンジン DLL を内部で束ね、
PCM ミックスと音声出力まで完結させる。FITOM コアは音声出力に依存せず、
`HWPlugin_Write` を呼ぶだけでよい。使うチップの構成やサンプルレートなど、
プラグイン自身の設定は、プラグイン固有の設定ファイルに持つ。そのパスは
FITOM のプロファイルの `hw_plugins[].profile` に書き、`HWPlugin_Init` に渡される。

---

## エクスポート関数一覧

| 区分 | 関数 | エクスポートしない場合 |
|---|---|---|
| 必須 | `GetName` / `Init` / `Enumerate` / `FreeString` / `Open` / `Close` / `Write` / `WriteBlock` / `Reset` / `GetClock` / `GetPanpot` / `IsOpen` | ロードに失敗する |
| 任意 | `GetLatencySamples` / `SetDelaySamples` | `GetLatencySamples` が無ければレイテンシ 0 とみなす。`SetDelaySamples` が無ければ呼ばない |
| 任意(強く推奨) | `Shutdown` | 呼ばない |
| 任意(4関数の組) | `GetPartCount` / `GetPartName` / `SetPartGain` / `GetPartGain` | 「部位ごとのゲイン」節を参照 |
| 任意 | `SetMasterVolume` | 呼ばない(マスターボリュームはそのデバイスに掛からない)。「マスターボリューム」節を参照 |

関数名はいずれも `HWPlugin_` で始まる。

### プラグイン情報

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetName` | `const char* ()` | プラグイン名を返す |

### 初期化

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Init` | `HWResult (const char* profile_path)` | プラグインを初期化する |

- FITOM は DLL をロードした直後、他のどの関数よりも先に一度だけ呼ぶ
- `profile_path` はプロファイルの `hw_plugins[].profile` の値。省略されていれば `nullptr` を渡すので、プラグイン自身の既定の探索規則で設定ファイルを探すこと。FITOM はこのファイルの中身を解釈しない
- `HW_OK` 以外を返すと、FITOM はそのプラグインを登録せず、以降どの関数も呼ばない
- 初期化前に他の関数が呼ばれた場合は失敗を返すこと

### デバイス列挙

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Enumerate` | `const char* ()` | 開けるデバイスを JSON 配列で返す。呼び出し元が `HWPlugin_FreeString` で解放する。失敗時は `nullptr` |
| `HWPlugin_FreeString` | `void (const char*)` | `HWPlugin_Enumerate` が返した文字列を解放する |

プロファイルの `hw_plugins[].auto_devices` が `true` のとき、FITOM は `HWPlugin_Enumerate`
が返した配列の各要素を、そのままプロファイルの `devices[]` の1件として扱う。つまり各要素は、
次節の `params_json` としてそのまま `HWPlugin_Open` に渡せる形で、かつ FITOM が読む `chip`
を含んでいる必要がある。物理チップは挿さっているチップを自動判別できないので、物理チップの
プラグインは `chip` を返さなくてよい(その場合、利用者は `devices[]` を手で書く)。

返却例(FitomHwIF):

```json
[
  { "type": "RE1",        "serial": "ABCD1234", "index": 0 },
  { "type": "SPFM_TOWER", "port":   "COM3",     "index": 0 }
]
```

返却例(FitomEmuIF。`auto_devices` で使う):

```json
[
  { "type": "FMHWIF", "engine": "YMEngine", "chip": "OPM",  "index": 0 },
  { "type": "FMHWIF", "engine": "YMEngine", "chip": "OPNA", "index": 1 }
]
```

### デバイス開閉

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Open` | `HWResult (const char* params_json, HWHandle* out)` | デバイスを開く |
| `HWPlugin_Close` | `void (HWHandle)` | デバイスを閉じてリソースを解放する |

**FITOM が渡す `params_json`**: プロファイルの `devices[]` の1件から、FITOM 自身が使う
キー(`if` / `label` / `plugin` / `extra_slot` / `rhythm_mode` / `stereo_pair`)を
取り除いた残りすべて。それ以外のキーの意味はプラグインが決める。

FITOM が `params_json` に手を加えるのは次の2つだけ。

- `extra_slot` が書かれたデバイスは、2ポート目として `slot` を `extra_slot` の値に置き換えた `params_json` でもう一度 `HWPlugin_Open` を呼ぶ
- `chip` が `"SF2"` のデバイスには、`soundfonts` を足す(「SoundFont プラグインへの追加の約束」節を参照)

`chip` は FITOM がチップの種類を決めるために読む。プラグインに渡す `params_json` にも残る。

現在の実装が読むキー(参考。正本は各リポジトリ):

| プラグイン | キー |
|---|---|
| FitomHwIF | `type`(RE1 / RE4 / SPFM_TOWER / SPFM_LIGHT) / `serial` / `port` / `slot` / `index` / `pan` / `sample_rate` |
| FitomEmuIF | `type`(`"FMHWIF"`固定) / `engine` / `chip` / `index` / `pan` |
| FitomSf2IF | `soundfonts` |

`pan` の値は 0=ステレオ、1=L のみ、2=R のみ。FITOM は `HWPlugin_GetPanpot` でこの値を読み、
L/R の組の判定に使う。

### プラグイン全体のシャットダウン(任意実装、強く推奨)

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Shutdown` | `void ()` | プラグイン全体を安全に停止する。プロセス終了前にFITOM_X側が一度だけ呼ぶ |

**背景・なぜ必要か**

`HWPlugin_Close`はハンドル単位(`HWPlugin_Open`で開いた1つのデバイス)の解放のみを行う契約です。しかし、`RtAudio`のストリームのように**プラグイン全体で1つだけ共有するリソース**(バックグラウンドのオーディオコールバックスレッドを持つ)は、個々の`HWPlugin_Close`呼び出しでは止まりません。

この関数が実装されていない場合、ストリームの停止はC++の静的デストラクタ(グローバル/staticオブジェクトの破棄)任せになります。この破棄は、プロセス終了時にDLLのアンロード(`FreeLibrary`/`dlclose`)と前後する、暗黙的なタイミングで走ります。**この状態でオーディオスレッドの停止・join処理を行うと、Windowsではローダーロックに起因するデッドロック(アプリがフリーズし、Ctrl+Cも効かなくなる)を引き起こすことがあります。**

**実装要件**

- `HWPlugin_Init`が成功した後、FITOM_Xがプロセスを終了する前に**一度だけ**呼ばれます(`HWPlugin_Open`/`HWPlugin_Close`の呼び出し回数・順序とは独立)。
- この関数の中で、オーディオストリームを**同期的に**(呼び出しから戻った時点で完全に停止・スレッドがjoin済みの状態になるまで)停止してください。`RtAudio`であれば`stopStream()`→`closeStream()`が該当します。
- 呼び出しは通常のメインスレッドのコンテキストで行われます(`DllMain`/`DLL_PROCESS_DETACH`の中ではありません)。ロック取得やスレッドjoin等、`DllMain`内では避けるべき操作をこの関数の中では安全に行えます。
- 冪等性は不要です(FITOM_X側で二重呼び出しをガードしています)。
- **未実装でも動作します**(FITOM_X側は`GetProcAddress`/`dlsym`でこの関数を探し、見つからなければ何もせずスキップします)。ただし未実装のままだと、上記のフリーズ問題が解消されません。
- この関数が呼ばれないまま終了する場合(この関数を呼ばない古いFITOM_X等)にも耐えられるよう、静的デストラクタでの停止処理は残しておくことを推奨します。

**実装例(RtAudioを使う場合)**

```cpp
// グローバル/staticなRtAudioインスタンスを想定
static RtAudio* g_audio = nullptr;

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_Shutdown()
{
    if (g_audio && g_audio->isStreamOpen()) {
        if (g_audio->isStreamRunning()) {
            g_audio->stopStream();
        }
        g_audio->closeStream();
    }
}
```

**エクスポート**:他の`HWPlugin_*`関数と同様に、`.def`ファイル(または`__declspec(dllexport)`)でエクスポートしてください。

### I/O

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_Write` | `HWResult (HWHandle, uint16_t addr, uint8_t data)` | 1バイト書き込み |
| `HWPlugin_WriteBlock` | `HWResult (HWHandle, uint8_t startAddr, const uint8_t* data, size_t len)` | 連続書き込み |
| `HWPlugin_Reset` | `HWResult (HWHandle, unsigned int pulse_us)` | ハードウェアリセット |

### メタ情報

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetClock` | `int (HWHandle)` | チップのマスタークロック [Hz] を返す。FITOM は F-number・トーン周期・ADPCM の再生レートをこの値から計算するので、チップを実際に動かしているクロックを返すこと。OPNA の SSG 部や OPL4 の FM 部のような内部ブロックの分周は FITOM 側で行うため、チップ自体のクロックを返す。分からないときは 0 を返す(FM 系はチップの標準クロック、それ以外は音声出力のサンプルレートで代用する) |
| `HWPlugin_GetPanpot` | `int (HWHandle)` | パノラマ設定値を返す(`pan` と同じ値域) |
| `HWPlugin_IsOpen` | `bool (HWHandle)` | デバイスが使用可能か返す |

### レイテンシ同期（オプション）

物理チップと FM エンジン内蔵 hwif を混在使用する場合、
発音タイミングを揃えるためにレイテンシ同期が必要になる。
以下の2関数は**実装任意**。`HWPlugin_GetLatencySamples` が無いデバイスはレイテンシ 0(即時)とみなし、`HWPlugin_SetDelaySamples` が無いデバイスには遅延を設定しない。

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetLatencySamples` | `uint32_t (HWHandle)` | `write()` から実際の発音までのレイテンシ [samples] を返す |
| `HWPlugin_SetDelaySamples` | `void (HWHandle, uint32_t)` | FITOM が算出した基準遅延量 [samples] を設定する |

**実装ガイド:**

```c
// FMエンジン内蔵: バッファサイズ = 自身のレイテンシ
uint32_t HWPlugin_GetLatencySamples(HWHandle h) {
    return h->buffer_frames;   // 例: 512
}
// FMエンジン内蔵: 自身のレイテンシと一致するため通常は何もしなくてよい
void HWPlugin_SetDelaySamples(HWHandle h, uint32_t delay) {
    (void)delay;  // buffer_frames == delay になるよう設定済みのため no-op
}

// 物理チップ: 即時デバイスなので 0 を返す
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

デバイスをすべて開いた後、FITOM は全デバイスに対して以下を実行する。

```
1. 全 HWPort の GetLatencySamples() を収集 → max_latency を決定
2. 全 HWPort に SetDelaySamples(max_latency) を設定
```

これにより「物理チップ + FMエンジン内蔵hwif」混在構成でも
ノート ON/OFF のタイミングが一致する。

### 部位ごとのゲイン（オプション）

チップによっては、音を複数の端子から別々に出す（OPNA の FM と SSG など）。
この出力のひとつひとつを**部位**と呼ぶ。実機ではボード上の回路でミックスするので、
音量のバランスは機種で違う。FM エンジン内蔵 hwif が、そのバランスを FITOM から
調整できるようにするための関数。物理チップの hwif は実装しなくてよい
(実装して、部位を 0 個返してもよい)。

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_GetPartCount` | `uint32_t (HWHandle)` | デバイスが持つ部位の数を返す。部位を持たないデバイスは 0 |
| `HWPlugin_GetPartName` | `const char* (HWHandle, uint32_t index)` | index 番目の部位の名前を返す。範囲外は `nullptr` |
| `HWPlugin_SetPartGain` | `HWResult (HWHandle, const char* part, float gain_l, float gain_r)` | 部位のゲインを設定する |
| `HWPlugin_GetPartGain` | `HWResult (HWHandle, const char* part, float* out_gain_l, float* out_gain_r)` | 部位のゲインを取得する |

**4 関数は組でエクスポートする。** FITOM は `HWPlugin_GetPartCount` の有無だけで
組の有無を判定する。

| エクスポートの状態 | FITOM の扱い |
|---|---|
| 4 関数とも無い | どの関数も呼ばず、どのデバイスも部位を持たないものとして扱う |
| 4 関数ともある | 部位ごとのゲインに対応するプラグインとして扱う |
| `HWPlugin_GetPartCount` があり、残りのどれかが欠ける | プラグインのロードに失敗させる（必須関数の欠落と同じ扱い） |

**実装要件**

- **部位の名前**：プラグインが決める文字列で、大文字小文字を区別する（`"FM"`、`"SSG"` 等）。
  並ぶ順序は定めない。FITOM は部位を index ではなく名前で識別する。
- **文字列の寿命**：`HWPlugin_GetPartName` が返す文字列は `HWPlugin_Close(handle)` まで
  有効であること。FITOM は解放しない。
- **ゲインの値**：L/R 独立で、1.0 = 0 dB。`pan` による L/R の振り分けとは別に掛かる
  （実際に掛かるのは両者の積）。
- **既定値と寿命**：設定したゲインは `HWPlugin_Close(handle)` で既定値に戻すこと。
  既定値はプラグインが決める値で、1.0 とは限らない。`HWPlugin_Open` の直後に
  `HWPlugin_GetPartGain` で取得できること。
- **部位を持たないデバイス**：`HWPlugin_GetPartCount` は 0 を返す。
- **音声出力の動作中に呼び出せること。**

| 戻り値 | 条件 |
|---|---|
| `HW_OK` | 成功 |
| `HW_ERR_INVALID_ARG` | `handle` / `part` / 出力先が `nullptr`、またはデバイスが持たない部位の名前 |
| `HW_ERR_IO` | それ以外の理由による失敗 |

**FITOM コア側の使い方:**

```
1. HWPlugin_Open の直後に GetPartCount / GetPartName / GetPartGain を呼び、
   部位の一覧と既定値を控える（既定値を読めなかった部位は調整対象にしない）
2. 続けて、プロファイルの part_gains に保存されている値を SetPartGain で適用する
3. 演奏中は、GUI のシステム設定ダイアログの操作に応じて SetPartGain を呼ぶ
```

- FITOM が保存するのは、既定値から変えた部位の値だけ。変えていない部位には
  `HWPlugin_SetPartGain` を呼ばず、プラグインの既定値のまま鳴らす
  （プロファイル側の書式は `docs/manuals/profile-reference.md`「チップの出力ごとの音量」参照）。
- 組をエクスポートしないプラグインのデバイスには、4 関数のどれも呼ばない。

### マスターボリューム（オプション）

FITOM のマスターボリューム(GM2 Universal Realtime のマスターボリューム、GUI の
システム設定ダイアログ、プロファイルの `master_volume`)を、デバイスの音声出力に
掛けるための関数。FITOM はマスターボリュームをチップのレジスタには反映しないので、
音声出力を持つプラグイン(FM エンジン内蔵 hwif 等)がミキサーで掛ける。物理チップの
hwif は実装しなくてよい。

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `HWPlugin_SetMasterVolume` | `HWResult (HWHandle, uint8_t volume)` | デバイスの出力にマスターボリュームを設定する |

**実装要件**

- **値**：0〜127。GM2 のマスターボリュームの上位 7bit と同じ値。音量への換算は
  プラグインが決める。128 以上は `HW_ERR_INVALID_ARG`。
- **既定値**：一度も呼ばれていないデバイスは 127(減衰なし)として扱うこと。
- **他のゲインとの関係**：部位ごとのゲイン・`pan` とは別に掛かる(実際に掛かるのはこれらの積)。
- **音声出力の動作中に呼び出せること。**

| 戻り値 | 条件 |
|---|---|
| `HW_OK` | 成功 |
| `HW_ERR_INVALID_ARG` | `handle` が `nullptr`、または `volume` が 128 以上 |
| `HW_ERR_IO` | それ以外の理由による失敗 |

**FITOM コア側の使い方:**

```
1. 全デバイスを開いた後、プロファイルの master_volume(省略時 100)で
   開いたデバイスごとに SetMasterVolume を呼ぶ
2. 演奏中は、マスターボリュームの SysEx の受信と、GUI のシステム設定ダイアログの
   操作に応じて、開いたデバイスごとに同じ値で SetMasterVolume を呼ぶ
```

- `chip: "SF2"` のデバイスには呼ばない(SoundFont プラグインには SysEx で送る。
  「SoundFont プラグインへの追加の約束」節参照)。
- エクスポートしないプラグインのデバイスには呼ばない。

---

## アドレス変換規則

`HWPlugin_Write(handle, addr, data)` の `addr` は以下のように解釈する。

```
addr 上位 8bit (addr >> 8)   → a_high（SPFM 拡張アドレス / OPL3 Array1 等）
addr 下位 8bit (addr & 0xFF) → チップレジスタアドレス
```

2ポートチップ（OPNA / OPN2 / OPL3）の `addr >= 0x100` は FITOM 側で a_high=1 に
変換して渡すため、DLL は addr の解釈のみ実装すればよい。

### レジスタアドレスは常に実チップのもの（重要）

FITOM が渡す `addr` は**実チップのレジスタ配置**である。エミュレータ
ライブラリが独自のレジスタ番号体系を持つ場合、その変換は**プラグイン/
エンジン側の責務**であり、FITOM 側がライブラリの都合に合わせることはしない。

具体例（SCC）:

| | 波形 | 周波数 | 音量 | ch有効 | deform |
|---|---|---|---|---|---|
| **FITOM が書くアドレス** (無印SCC / K051649) | 0x00-0x7F | 0x80 | 0x8A | 0x8F | 0xC0 |
| **FITOM が書くアドレス** (SCC+ / K052539) | 0x00-0x9F | 0xA0 | 0xAA | 0xAF | 0xC0 |
| (参考) emu2212 の `SCC_writeReg` 番号体系 | 0x00-0x9F | 0xA0 | 0xAA | 0xAF | 0xC0 |

無印SCCはch3/ch4が波形メモリを共有して4ブロックしか持たないため、実チップ
では以降の制御レジスタが前へずれる。emu2212 は両チップを単一のフラットな
番号体系で扱うため無印でも 0xA0 系になるが、これはライブラリ内部の都合で
あり実チップの配置ではない。emu2212 系のエンジンは、受け取った実チップ
アドレスを自身の番号体系へ変換してから `SCC_writeReg()` を呼ぶこと。

同様に、実チップに存在しないレジスタ（emu2212 の 0xE0 = SCC/SCC+ モード
切替など）へ FITOM が書き込むことはない。エンジンが必要とする場合は
エンジン側で設定する。

アドレスの基準はチップ（SCC なら Z80 メモリ空間の 0x9800）である。Y8960 の
SCC 部（`chip: "SCCEX"`）は無印 SCC と同じ配置に、ch ごとの定位レジスタ（実機の
0x9900-0x9904）を足したもので、FITOM はこれを **0x100-0x104** として渡す。

### Y8960 の拡張レジスタ

Y8960 カートリッジの拡張ブロックが FITOM から書かれる番地は、すべて Y8960 の
仕様書（Y8960_Cartridge リポジトリ `doc/spec/Y8960_Specifications.xlsx`）の
レジスタ番号そのものである。定位はいずれも 8bit（1=左端 / 128=中央 / 255=右端、
リセット値 128）で、0 は書かない。

| `chip` | 拡張レジスタ |
|---|---|
| `OPLLEX` | 0x40-0x48 ch ごとの ROM プリセットバンク、0x50-0x58 ch ごとの定位 |
| `OPL2EX` | 0xF6-0xFE ch ごとの定位、0xFF ADPCM の定位 |
| `SSGEX` | 0x00-0x0D SSG-1、0x10-0x1D SSG-2（各 YM2149 と同一配置）、0x20-0x22 / 0x24-0x26 ch ごとの音量（0-128）、0x28-0x2A / 0x2C-0x2E ch ごとの定位 |
| `SCCEX` | 0x100-0x104 ch ごとの定位（上記） |

`SSGEX` は YMZ705 の `SSGS` とはユニットの間隔・定位の位置と分解能が異なる別の
チップである。エンジンが Y8960 の SSG 部を列挙するときのチップ名は `SSGEX` とすること
（`SSGS` と列挙すると、FITOM は YMZ705 の配置で書き込む）。

---

## SoundFont プラグインへの追加の約束

プロファイルで `chip: "SF2"` と書いたデバイスは、レジスタではなく MIDI バイト列を受け取る。
SoundFont プラグインは、新しい関数を足さずに既存の関数だけでこれを受け取る。

- **`HWPlugin_Open`**: `params_json` に、FITOM が `soundfonts`(SF2 ファイルの絶対パスの配列)を足して渡す。FITOM はプロファイルの `banks.sf2_banks[].file` から重複を除いてこの配列を作る。配列の0起算の位置を `soundfont_index` と呼ぶ。同じファイルは配列に1回しか現れないので、ファイルのロードは各要素につき1回でよい
- **`HWPlugin_WriteBlock`**: `startAddr` は常に 0 で、`data` に MIDI バイト列が1メッセージずつ入る。FITOM は `HWPlugin_Write` を使わない
- **チャンネル**: FITOM は、プロファイルの `sf2_channel_windows` で SF2 に割り当てた MIDI チャンネルのメッセージだけを、割り当て先の fluidsynth のチャンネル番号に書き換えて送る

FITOM が送るメッセージ:

| メッセージ | 内容 |
|---|---|
| ノートオン/オフ、CC、ピッチベンド、チャンネルプレッシャー等 | チャンネル番号だけを書き換えて、そのまま送る。CC#0 と CC#32 は送らない |
| `F0 00 48 01 05 <chan> <soundfont_index> <bank_msb> <bank_lsb> <prog> F7` | プログラムチェンジの代わり。FITOM が CC#32 から SF2 ファイルとファイル内のバンク番号を解決したうえで送る。`<chan>` は fluidsynth のチャンネル、バンク番号は14bitを7bitずつに分けたもの。CC#32 が一度も解決されていないチャンネルでは送らない |
| `F0 7F 7F 04 01 00 <vol> F7` | マスターボリュームの変更(GM2 Universal Realtime、7bit 精度) |
| `F0 7F 7F 04 03 <ll> <mm> F7` | マスターピッチの変更(GM2 マスターファインチューニング。440Hz からのずれを ±100 セントの範囲で表す) |

音声出力は他のプラグインと同じくプラグイン側で完結させる。FITOM はプラグイン間の音声を
ミックスしない。

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
| `HWPlugin_Init` | ロード直後に一度だけ呼ばれる。スレッドセーフ不要 |
| `HWPlugin_Write`, `HWPlugin_WriteBlock` | **スレッドセーフ必須**。FITOM は MIDI 入力・タイマー・GUI 操作のそれぞれのスレッドから呼ぶ。現在の FITOM はこれらを1つのロックで直列化しているが、契約としては並行呼び出しを許容すること |
| `HWPlugin_Open`, `HWPlugin_Close`, `HWPlugin_Reset` | 初期化・終了フェーズのみ。スレッドセーフ不要 |
| `HWPlugin_GetLatencySamples` | 初期化フェーズのみ呼ばれる。スレッドセーフ不要 |
| `HWPlugin_SetDelaySamples` | 初期化フェーズのみ呼ばれる。スレッドセーフ不要 |
| `HWPlugin_GetPartCount`, `HWPlugin_GetPartName` | `HWPlugin_Open` の直後に、同じスレッドから呼ばれる。スレッドセーフ不要 |
| `HWPlugin_SetPartGain`, `HWPlugin_GetPartGain` | **音声出力（オーディオコールバック）と並行して呼び出せること**。`HWPlugin_Open` の直後（初期化フェーズ）と、演奏中の GUI 操作から呼ばれる。演奏中の呼び出しは、MIDI 処理・タイマーと同じロックで直列化されるので、それらが行う `HWPlugin_Write` / `HWPlugin_WriteBlock` とは並行しない。`HWPlugin_Open` / `HWPlugin_Close` とも並行しない |
| `HWPlugin_SetMasterVolume` | **音声出力（オーディオコールバック）と並行して呼び出せること**。初期化フェーズと、演奏中の SysEx の受信・GUI 操作から呼ばれる。演奏中の呼び出しは、MIDI 処理・タイマーと同じロックで直列化されるので、それらが行う `HWPlugin_Write` / `HWPlugin_WriteBlock` とは並行しない。`HWPlugin_Open` / `HWPlugin_Close` とも並行しない |
| `HWPlugin_Shutdown` | 終了フェーズに一度だけ、メインスレッドから呼ばれる。**呼び出しから戻るまでに、内部のバックグラウンドスレッド(オーディオコールバック等)を完全に停止・joinし終えていること**(詳細は「プラグイン全体のシャットダウン」節参照) |

---

## ビルド要件

| 項目 | 要件 |
|---|---|
| 言語 | C++17 以上 |
| DLL 名 | 任意（プロファイルの `hw_plugins[].dll` で指定） |
| エクスポート | `extern "C"` で C リンケージを保つこと。`IHWPlugin.h` を取り込み、`FITOM_HW_PLUGIN_EXPORTS` を定義してビルドする |
| プラットフォーム | Windows / Linux / macOS |

---

## config 記述例

プロファイル(`*.profile.json`)の `hw_plugins` に登録し、`devices` の `plugin` で名前を指す。

```json
"hw_plugins": [
  { "name": "hw",  "dll": "fitom_hw.dll" },
  { "name": "emu", "dll": "FitomEmuIF.dll", "profile": "fmhwif_profile.json", "auto_devices": true }
],
"devices": [
  { "if": "HW", "plugin": "hw", "label": "OPNA", "chip": "OPNA",
    "type": "SPFM_LIGHT", "port": "COM4", "slot": 0 }
]
```

プロファイルの書き方の詳細は `docs/manuals/profile-reference.md` を参照。
