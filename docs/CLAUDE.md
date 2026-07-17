# CLAUDE.md

このファイルは、Claude Code(または他のセッション)がこのリポジトリで
作業する際に自動的に読み込む規約ファイルです。**複数のマシンで作業する
ことを前提にしています**。会話履歴はマシンごとにローカルなため、
このファイルと`docs/`配下のドキュメントだけを頼りに、どのマシンから
始めても同じ前提で作業を再開できるようにしてください。

作業を始める前に、必ず`docs/STATUS.md`(進捗・完成状況)を先に読んでください。

---

## プロジェクト概要

FITOM_Xは、複数のFM/PSGサウンドチップ(OPN/OPNA/OPM/OPZ/OPL系/OPLL系/
PSG各種/ADPCM/AWM等)を横断的に扱う、クロスプラットフォームなマルチチップ
FM音源エンジン + MIDIアプリケーションです(C++17)。

## ディレクトリ構成

```
apps/
  fitom_cli/     CLIアプリケーション
  fitom_gui/     Dear ImGui GUIアプリケーション(gui/bridge経由でコアに接続)
gui/
  bridge/        UIフレームワーク非依存のコアブリッジ(FITOMBridge)。
                 apps/fitom_gui等はこれ経由でのみコアに触れる。
core/
  include/fitom/ 公開ヘッダ
  src/           実装
legacy/          旧実装(ビルド対象外、参照用に保管。core/には含めない)
backends/        差し替え可能なプラグインDLL(HW I/F、MIDIバックエンド各種)
plugin_sdk/      DLLプラグインのC API契約(IHWPlugin.h、IMidiPlugin.h)
docs/            設計ドキュメント(下記参照)
tests/           Catch2テスト
```

## ビルド

```bash
cmake --preset <preset名>   # CMakePresets.json参照。windows-vs2022-x64 /
                             # windows-vs2026-x64 / linux-ninja 等
cmake --build --preset <preset名>
```

GUI(Dear ImGui)をビルドする場合は`-DFITOM_GUI_IMGUI=ON`が必要です。
`third_party/imgui`・`third_party/glfw`はgit submoduleとして別途取得して
ください(`git submodule update --init --recursive`)。

## 作業時の必須ルール

- **変更のたびに必ずビルド・既存テスト(`tests/fitom_tests`)を実行して確認する。** 警告が増えていないかも確認すること
- **最小限の原則的な変更を優先する。** その場しのぎの回避策より、既存のアーキテクチャに沿った一貫性のある実装を選ぶ
- **チップファミリー間の一貫性は必須要件。** ある制約を1つのチップに適用したら、類似する他のチップにも同じ設計を適用する
- **ドキュメントと実装の設計意図を一致させ続ける。** 挙動を変えたら、対応する`docs/`配下の記述も同じコミット/セッション内で更新する
- 新しいgetter/setterを追加する前に、既存のインターフェース(`IMidiCh`・`ISoundDevice`等)に既に同等のものが無いか確認する
- JSONスキーマ(`config_schema/`)を変更したら、対応するリファレンスドキュメント(下記)も更新する

## 主要ドキュメント

| ドキュメント | 内容 |
|---|---|
| `docs/STATUS.md` | 完成状況の一覧(**作業開始前に必読**) |
| `docs/manuals/midi-message-reference.md` | エンドユーザー向けMIDIメッセージ仕様(CC/RPN/NRPN/SysEx全般) |
| `docs/manuals/midi-implementation-chart.md` | 上記の対応状況一覧表 |
| `docs/midi-implementation-status.md` | 開発者向けMIDI実装状況(内部実装詳細つき) |
| `docs/manuals/hwpatch-reference.md` | HwPatch(音色合成パラメータ)リファレンス |
| `docs/manuals/swpatch-reference.md` | SwPatch(演奏特性パラメータ)リファレンス |
| `docs/manuals/native-patch-reference.md` | ネイティブパッチ(ToneLayer)リファレンス |
| `docs/plugin-hwif.md` | HW I/Fプラグイン要件定義(実装リポジトリ: FitomEmuIF) |
| `docs/plugin-midi.md` | MIDIバックエンドプラグイン要件定義 |
| `docs/plugin-midi-pipe.md` | 内部用MIDIパイプ(名前付きパイプ)インターフェース仕様。**別プロジェクトのパッチエディタ向け**。このリポジトリ側はインターフェース(`backends/midi_pipe/`)とドキュメントの整備のみがスコープで、パッチエディタ本体は実装しない |

## 別プロジェクトとの境界

- **パッチエディタ**:別プロジェクトとして実装される。このリポジトリの
  スコープは`backends/midi_pipe/`(インターフェース)と
  `docs/plugin-midi-pipe.md`(仕様書)の整備のみ
- **FitomEmuIF / YMEngine**:HW I/Fの実装は別リポジトリ。このリポジトリは
  `plugin_sdk/include/fitom/IHWPlugin.h`(契約)と
  `docs/plugin-hwif.md`(要件定義)を整備する側

## GUI(apps/fitom_gui)の現状

Dear ImGui + GLFW + OpenGL3。ルート画面はMIDIモニターのバンド(CH毎の
Bank/Program/Volume/Note/Device/Fnumber表示 + 128ノートのキーボードビュー
+ ベロシティ連動の発光エフェクト)のみを表示する設計。他のビュー
(デバイス一覧・パッチエディタ等)への導線は未実装(該当する描画関数は
`[[maybe_unused]]`で温存済み)。詳細な現状は`docs/STATUS.md`を参照。

## デバッグ・検証の作法

- スクリーンショットでの確認が必要な場合、Linux環境では`Xvfb`+
  `ImageMagick(import)`で仮想ディスプレイ上のGUIを撮影できる
- 実際のオーディオデバイスが無い環境(CI・サンドボックス等)では、
  `HWPlugin_Init`はRtAudio等のストリームオープンに失敗するのが正常。
  これは環境側の制約であり、コードの不具合ではない
- 変更後は必ずフルビルド+`tests/fitom_tests`を実行し、警告が新たに
  増えていないか確認してから完了とする
