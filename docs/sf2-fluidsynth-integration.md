# FluidSynth (SF2) 統合 技術検討

**ステータス**: 技術検討のみ。設計決定・実装は未着手。
**検討日**: 2026年7月
**検討の目的**: FITOM_Xに`fluidsynth`を組み込み、SF2サウンドフォントをシームレスに扱えるようにする(例: CC#0の特定値でSF2音源へ切り替え、FITOM_X固有バンクと動的に共存させる)ことが、現在の構造のまま可能かどうかを検証する。

---

## 1. 前提となる問い

現在のFITOM_Xは、音声出力を完全に`IHWPlugin`(HW I/Fプラグイン、実装は別リポジトリのFitomEmuIF等)へ移譲している。一方、MIDIメッセージの解析(CC/RPN/NRPN/プログラムチェンジ等の意味づけ)はFITOM_X本体(`CInstCh`/`CRhythmCh`/`MidiProcessor`)の責務である。

`fluidsynth`はMIDI解析・音声合成・音声出力を一体で行うライブラリだが、この責務分担とどう整合させられるか、またFluidSynth自体の改造を避けたい(可能な限り無改造でリンクしたい)、というのが検討の出発点。

---

## 2. FluidSynthの内部構造

FluidSynthは公開APIレベルで既に3層に分離されている。

| 層 | 実体 | 役割 |
|---|---|---|
| 合成エンジン | `fluid_synth_t` | `fluid_synth_noteon()`/`fluid_synth_cc()`/`fluid_synth_program_change()`/`fluid_synth_pitch_bend()`/`fluid_synth_sfload()`等。**生MIDIバイト列ではなく、解析済みイベントを受け取るAPI** |
| MIDI受信・SMF再生 | `fluid_midi_driver_t` / `fluid_player_t` | 生MIDIバイト列やSMFファイルをパースして合成エンジン層のAPIへ変換する、完全に独立したオプション層 |
| 音声デバイス | `fluid_audio_driver_t` | PortAudio/ALSA/WASAPI等で自前のオーディオストリームを持つ、これもオプション層 |

音声デバイス層を使わず、`fluid_synth_process()`/`fluid_synth_write_float()`を呼べば、任意のタイミングでPCMサンプルを引き出すだけの「プル型」レンダリングとして使える。

**結論**: MIDI受信層・音声デバイス層を一切使わず、合成エンジン層(`fluid_synth_t`)だけをリンクすれば、**FluidSynthのソースに一切手を入れずに**、「解析済みMIDIイベントを受け取り、PCMを吐き出すだけのライブラリ」として組み込める。これはFluidSynth本来の設計(組み込み用途を想定したライブラリ構成)に沿った使い方であり、無理な分割ではない。

---

## 3. 核心的な設計上の緊張関係

現状の`ISoundDevice`(core/include/fitom/ISoundDevice.h)は、**FMチップのハードウェア制約(少数の物理チャンネルを、FITOM_X本体のDVA = `allocCh`/`queryCh`が奪い合って割り当てる、1物理チャンネル1発音)**を前提にした抽象化になっている。

一方FluidSynthは、**GM準拠の16chそれぞれが内部で独自にポリフォニーを管理する**、逆方向のモデルである。1chに何音重ねるかはFITOM_X側ではなくFluidSynth自身が決める。

このため、**FluidSynthを無理に`ISoundDevice`の1実装として組み込むのは筋が悪い**と考える。`noteOn(ch, vel)`のような、「どの物理chを使うか」を事前にFITOM_X側のDVAが決め切っている前提のシグネチャと、FluidSynthの「chとノート番号を渡せば内部で勝手にポリ管理する」モデルは、インターフェースの意味論のレベルで噛み合わない。

---

## 4. 提案する分割方針

### ① `IHWPlugin.h`にソフトシンス向けイベントAPIを任意実装として追加

既存の`HWPlugin_Write`(チップのレジスタ書き込みを模した契約)とは別に、MIDIイベントレベルの関数群を新設する。

```c
// 案(未確定、シグネチャは要検討)
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SoftSynthNoteOn(uint8_t ch, uint8_t note, uint8_t vel);
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SoftSynthNoteOff(uint8_t ch, uint8_t note);
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SoftSynthCC(uint8_t ch, uint8_t cc, uint8_t val);
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SoftSynthProgramChange(uint8_t ch, uint8_t prog);
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SoftSynthSysEx(const uint8_t* data, size_t len);
```

`HWPlugin_Shutdown`(2026年7月新設)と同じく、未実装プラグインとの後方互換のため任意実装(`GetProcAddress`/`dlsym`で見つからなければ単に使わない)とする。

### ② HWプラグイン側(FitomEmuIF、またはSF2専用の新規プラグイン)がこれを実装

内部に`fluid_synth_t`を1つ持ち、①の各関数呼び出しを対応する`fluid_synth_*`関数へ**ほぼ1:1で転送するだけ**(FluidSynth本体の改変は不要)。

オーディオ出力は、既存のRtAudioコールバック内でFM系エンジンの出力を合算している箇所に、`fluid_synth_process()`の出力も同じバッファへ加算するだけで済む。同一クロック・同一ストリームでミックスされるため、複数ストリーム間のクロックずれ等の問題は生じない。

### ③ FITOM_X本体側:`CInstCh`に「SF2直行パス」を新設

既存の「直接デバイス選択モード」(CC#0の特定値でOPN/OPM等を直接指定する仕組み、[MIDIメッセージリファレンスマニュアルの「2.2 バンクセレクト / プログラムチェンジ」](manuals/midi-message-reference.md#22-バンクセレクト--プログラムチェンジ)参照)と同じ枠組みに、SF2用の予約値を1つ追加する。

この値が選択されているチャンネルは、`PatchManager`によるHwPatch解決・`ISoundDevice`経由のDVAを一切通さず、Note On/Off・CC・プログラムチェンジをそのまま①のAPIへ素通しするだけの経路にする。CC#32(バンクセレクトLSB)は、この場合そのままSF2側のバンク番号として使う形になり、既存の「直接モード」の意味論(CC#0=種別選択、CC#32=サブバンク、プログラムチェンジ=番号)と自然に整合する。

これにより、既存の「メロディチャンネル」「リズムチャンネル」に続く、**第三の経路(SF2直行パス)**として、既存2経路とは独立に実装できる。

---

## 5. 残る検討事項(実装時に詰める必要がある点)

- **モード切替時のノートオフ漏れ対策**:SF2直行パス⇔通常のFM/PSGチップ間でCC#0の値を切り替えた際、切替前に鳴っていたSF2側のノートが取り残されないようにする必要がある(既存のGM2リズム⇔メロディ切替時のクリーンアップと同様の考慮が必要)。
- **RPN/NRPNの引き継ぎ範囲**:ピッチベンドレンジ(RPN#0)等、FITOM_X側で既に独自拡張している一部のRPN/NRPN(96,1の物理チャンネル固定、96,2/3のパフォーマンスバンク切替、97番台のToneLayerオーバーライド等)を、SF2直行パスでもそのまま解釈するのか、無視するのか、別の意味を持たせるのかを決める必要がある。
- **複数MIDIポート(MPU)との関係**:SF2エンジン(`fluid_synth_t`)のインスタンスを全MPUで共有するか、MPUごとに持つかを決める必要がある(共有する場合、`fluid_synth_t`自体が既に16ch分のポリフォニー管理を内包しているため、MPUをまたいだチャンネル番号の衝突をどう避けるかも合わせて検討)。
- **GUIでの見え方**:`apps/fitom_gui`の「デバイス一覧」(`FITOMBridge::getDevices()`)は`ISoundDevice`ベースの列挙のため、SF2直行パスはこの一覧には現れない。MIDIモニターのバンド上での表示(Device/Fnumber列に何を出すか)も含め、別枠の扱いが必要になる。
- **SF2ファイルの管理**:`fluid_synth_sfload()`で読み込むSF2ファイルのパスをどう設定・切り替えるか(プロファイルJSONでの指定方法、複数SF2の切替可否)は未検討。

---

## 6. 結論

現在の構造のまま、FluidSynthのソースに一切手を入れずに組み込むことは**可能**。ただし「既存の`ISoundDevice`にもう1つ実装を足す」のではなく、「`IHWPlugin`の契約を拡張し、FITOM_X本体側に新しい第三の経路(SF2直行パス)を設ける」という、既存の2経路(メロディ/リズム)とは別枠の設計にするのが筋が良いと考える。

この文書は技術検討の記録であり、実装に着手する際は、上記5節の未決事項を先に詰めること。
