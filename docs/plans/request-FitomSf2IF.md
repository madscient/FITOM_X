# 依頼: FitomSf2IF の IHWPlugin.h の写しを同期し、オフライン描画を実装する

FITOM_X のセッションが書いた、FitomSf2IF で作業するセッション向けの依頼文。
作業が終わったら、FITOM_X のセッションがこの文書を消す。

## 背景

FITOM_X の HW プラグインの契約(`plugin_sdk/include/fitom/IHWPlugin.h`)には、
FitomSf2IF の写しより後に、任意の関数の組が2つ足されている。

- 部位ごとのゲイン(`HWPlugin_GetPartCount` / `GetPartName` / `SetPartGain` / `GetPartGain`)
- マスターボリューム(`HWPlugin_SetMasterVolume`、FITOM_X のコミット `d214ff9`)

どちらも任意で、FitomSf2IF は実装しなくてよい。FITOM は SF2 のデバイス(`chip: "SF2"`)に
`HWPlugin_SetMasterVolume` を呼ばず、マスターボリュームはこれまでどおり SysEx
(`F0 7F 7F 04 01 00 <vol> F7`)で送る。写しを同期しても、FitomSf2IF の動作は変わらない。

## やること

- [ ] 作業の前に fetch / pull して最新にする
- [ ] `plugin_sdk/include/fitom/IHWPlugin.h` を、FITOM_X の `d214ff9` の
      `plugin_sdk/include/fitom/IHWPlugin.h` で置き換える
      (`git -C <FITOM_X> show d214ff9:plugin_sdk/include/fitom/IHWPlugin.h` で取り出せる)
- [ ] 部位ゲインの組と `HWPlugin_SetMasterVolume` はエクスポートしない。ビルドが通ることを確かめる
- [ ] マスターボリュームの SysEx の扱いが、FITOM_X の `docs/interfaces/plugin-hwif.md`
      「SoundFont プラグインへの追加の約束」節と食い違っていないかを見る(変える必要は無いはず)

## 追加の依頼: オフライン描画(`HWPlugin_InitOffline` / `HWPlugin_Render`)を実装する

FITOM_X は、SMF を WAV へ変換するヘッドレスの実行ファイル `fitom_render` を足し、
HW プラグインの契約に任意の関数の組を足した。音声デバイスを開かず、FITOM が求めた
分だけ音を描画して返すモード。`fitom_render` は、プロファイルの `hw_plugins[]` に組を
持たないプラグインが1つでもあると変換を拒否する。FITOM_staging のプロファイルはすべて
FitomSf2IF を載せているので、FitomSf2IF が実装するまでどれも変換できない。
上の「部位ゲイン・マスターボリュームは実装しなくてよい」はそのままで、この組だけを実装する。

### やること

- [ ] `plugin_sdk/include/fitom/IHWPlugin.h` を、上の同期の代わりに、この節を追加した FITOM_X の
      コミットの版で置き換える
- [ ] `HWPlugin_InitOffline(const char* profile_path, uint32_t sample_rate)` と
      `HWPlugin_Render(float* out, uint32_t frames)` を組でエクスポートする。
      要件は FITOM_X の `docs/interfaces/plugin-hwif.md`「オフライン描画（オプション）」節。要点:
  - `HWPlugin_InitOffline` は `HWPlugin_Init` の代わりに呼ばれる。音声デバイスを開かない。
    以後は `HWPlugin_Init` の後と同じく他の全関数が使えること
  - `sample_rate` はプラグイン設定ファイルの `sample_rate` より優先する
  - オフラインのときは `HWPlugin_GetLatencySamples` が 0 を返す
  - `HWPlugin_Render` は L/R インターリーブの float を `frames` フレーム分書く。**クリップしない**
  - 呼び出しより前の `HWPlugin_Write` / `HWPlugin_WriteBlock`(MIDI バイト列)は、描画する最初の
    フレームから反映する
  - オフラインで初期化していなければ `HW_ERR_INVALID_ARG`
- [ ] `HWPlugin_Shutdown` が、オーディオドライバを作っていない状態でも安全に終わることを確かめる
- [ ] テストを足す

### 手がかり(FitomSf2IF `ae15e91` を FITOM_X のセッションが読んだもの。確かめてから使うこと)

- 音声デバイスは `FluidSynthEngine` の初期化で `new_fluid_audio_driver()` を作って鳴らしている。
  オフラインではこれを作らず、`HWPlugin_Render` で `fluid_synth_write_float()` を呼べばよい
  (L/R を別バッファに書くか、オフセット 0/1・間隔 2 でインターリーブのバッファへ直接書ける)
- サンプルレートは `synth.sample-rate` の設定に入る(プラグイン設定ファイルの `sample_rate` から)。
  オフラインでは引数の値で上書きする
- MIDI バイト列は `MidiEventParser` が解釈して `fluid_synth_noteon()` 等を直接呼んでいるので、
  書き込みは次の `fluid_synth_write_float()` から反映されるはず(未検証)

## 守ること

- FITOM_X は参照だけにして、編集しない
- FitomSf2IF の CLAUDE.md と規約に従う

## 終わったら

コミットのハッシュをユーザーに報告する。FITOM_X のセッションが、`docs/STATUS.md` の
5節(周辺リポジトリの追従状況)を更新し、この文書を消す。
