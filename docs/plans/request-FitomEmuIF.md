# 依頼: FitomEmuIF に HWPlugin_SetMasterVolume とオフライン描画を実装し、IHWPlugin.h の写しを同期する

FITOM_X のセッションが書いた、FitomEmuIF で作業するセッション向けの依頼文。
作業が終わったら、FITOM_X のセッションがこの文書を消す。

## 背景

FITOM_X は HW プラグインの契約に、任意の関数 `HWPlugin_SetMasterVolume` を足した
(FITOM_X のコミット `d214ff9`)。FITOM はマスターボリュームをチップのレジスタには
反映しないので、音声出力を持つプラグインがミキサーで掛ける。FitomEmuIF が実装するまで、
FitomEmuIF で鳴らす FM/PSG にはマスターボリュームが効かない。

## やること

- [ ] 作業の前に fetch / pull して最新にする
- [ ] `src/fitom/IHWPlugin.h` を、FITOM_X の `d214ff9` の
      `plugin_sdk/include/fitom/IHWPlugin.h` で置き換える
      (`git -C <FITOM_X> show d214ff9:plugin_sdk/include/fitom/IHWPlugin.h` で取り出せる)。
      手元の写しは、その1つ前のコミットの FITOM_X と同一だった(差分は
      `HWPlugin_SetMasterVolume` の節と、コメントの旧名 FitomIFTest の修正だけ)
- [ ] `HWPlugin_SetMasterVolume(HWHandle handle, uint8_t volume)` をエクスポートする。
      要件は FITOM_X の `docs/interfaces/plugin-hwif.md`「マスターボリューム（オプション）」節
      (`d214ff9` の時点)。要点:
  - 値は 0〜127。128 以上と `handle == nullptr` は `HW_ERR_INVALID_ARG`
  - 一度も呼ばれていないデバイスは 127(減衰なし)
  - 部位ごとのゲイン・pan とは別に掛かる(積)
  - 音声出力の動作中に呼ばれる。FITOM 側で `HWPlugin_Write` とは直列化されている
- [ ] 0〜127 から音量への換算を決める。決めるのはプラグイン側(FITOM は値をそのまま渡す)
- [ ] 他の設定と同じく、`HWPlugin_Close` で既定値(127)へ戻し、次に Open する側へ持ち越さない
- [ ] FitomEmuIF の文書にエクスポート関数の一覧があれば追記する
- [ ] テストを足す(範囲外の値、nullptr、Close で戻ること)

## 手がかり(FitomEmuIF `87d0a6c` を FITOM_X のセッションが読んだもの。確かめてから使うこと)

- チップごとのゲインは `FmEngine_SetGain(engine, chip_id, l, r)` で、pan の上書き
  (`panpot_override`)の実装に使われている。マスターボリュームの倍率をこの L/R に
  掛け合わせる方法が考えられる。pan を変えたときにもマスターボリュームが残るよう、
  両方から同じ計算で求めること
- `HWPlugin_Close` は、pan の上書きと部位ゲインを既定へ戻してからハンドルを消している
- ミックスは `PluginRegistry::audio_callback` でエンジンごとに加算し、最後に [-1, 1] で
  クランプしている
- 換算の参考: FITOM は CC#7(チャンネルボリューム)を GM2 のカーブ
  `40·log10(v/127)` dB で扱っている(v=0 は -96 dB。FITOM_X の `core/src/VolumeUtils.cpp`
  の `kGM2dB`)。同じカーブにすると、マスターボリュームとチャンネルボリュームの効き方が揃う

## 追加の依頼: オフライン描画(`HWPlugin_InitOffline` / `HWPlugin_Render`)を実装する

FITOM_X は、SMF を WAV へ変換するヘッドレスの実行ファイル `fitom_render` を足し、
HW プラグインの契約に任意の関数の組を足した。音声デバイスを開かず、FITOM が求めた
分だけ音を描画して返すモード。FitomEmuIF が実装するまで、FitomEmuIF を使うプロファイルは
`fitom_render` で変換できない(`fitom_render` は組を持たないプラグインがあると変換を拒否する)。

### やること

- [ ] `src/fitom/IHWPlugin.h` を、上の同期の代わりに、この節を追加した FITOM_X のコミットの
      `plugin_sdk/include/fitom/IHWPlugin.h` で置き換える(マスターボリュームの節も含む)
- [ ] `HWPlugin_InitOffline(const char* profile_path, uint32_t sample_rate)` と
      `HWPlugin_Render(float* out, uint32_t frames)` を組でエクスポートする。
      要件は FITOM_X の `docs/interfaces/plugin-hwif.md`「オフライン描画（オプション）」節。要点:
  - `HWPlugin_InitOffline` は `HWPlugin_Init` の代わりに呼ばれる。RtAudio のストリームを開かない。
    以後は `HWPlugin_Init` の後と同じく他の全関数が使えること
  - `sample_rate` はプラグイン設定ファイルの `sample_rate` より優先する
  - オフラインのときは `HWPlugin_GetLatencySamples` が 0 を返す
  - `HWPlugin_Render` は L/R インターリーブの float を `frames` フレーム分書く。**クリップしない**
    (FITOM が全プラグインの和を取った後で行う)
  - 呼び出しより前の `HWPlugin_Write` は、描画する最初のフレームから反映する
  - オフラインで初期化していなければ `HW_ERR_INVALID_ARG`
  - 部位ごとのゲイン・pan(・実装済みならマスターボリューム)は、音声デバイスへ出すときと同じく掛ける
- [ ] `HWPlugin_Shutdown` が、ストリームを開いていない状態でも安全に終わることを確かめる
- [ ] テストを足す(オフラインで初期化して書き込み→描画で音が出る、`HWPlugin_Init` 前の `Render` が失敗する等)

### 手がかり(FitomEmuIF `87d0a6c` を FITOM_X のセッションが読んだもの。確かめてから使うこと)

- RtAudio のストリームは `PluginRegistry::init()` の中の `start_audio()` で起動している。
  オフラインではこれを呼ばない分岐にすればよい
- `PluginRegistry::audio_callback` の中身(エンジンごとの `FmEngine_Generate` → 加算 → クランプ)が
  そのまま `HWPlugin_Render` になる。最後の [-1, 1] のクランプだけ外す
- 一時バッファ(`tmp_l`/`tmp_r`)は `buffer_frames` に合わせて確保されている。FITOM は1回に
  最大 4096 フレームを求める(通常は1ms分、44.1kHz で 44〜45 フレーム)ので、足りなければ
  分割するか確保し直す
- 各エンジンは `FmEngine_Create(sample_rate)` で作っている。オフラインでは引数の `sample_rate` を渡す

## 守ること

- FITOM_X は参照だけにして、編集しない
- FitomEmuIF の CLAUDE.md と規約に従う

## 終わったら

コミットのハッシュと、決めた換算をユーザーに報告する。FITOM_X のセッションが、
`docs/STATUS.md` の5節(周辺リポジトリの追従状況)を更新し、この文書を消す。
