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
config_schema/   設定ファイル・バンクファイルのJSONスキーマ
docs/            ドキュメント(下記「ドキュメントの地図」参照)
spec/            外部ツール(パッチエディタ等)向けの機械可読仕様データ。
                 コアは読まない(spec/README.md参照)
tests/           Catch2テスト
banks/           バンクファイルのフォーマット例示のみ(下記「バンク・
                 プロファイルの扱い」参照)
config/          プロファイル・fitom.confのフォーマット例示のみ(同上)
third_party/     git submodule(nlohmann-json / rtmidi / imgui / glfw)。編集しない
```

## バンク・プロファイルの扱い

**`banks/`と`config/profiles/`は、フォーマットと配線を示す例示専用。**
実際の演奏・聴感テストに使うバンクセットとプロファイルは
**ステージングリポジトリ側が正**であり、このリポジトリでは管理しない。

- 音色バンクの追加・更新をこのリポジトリへ持ち込まない。ここに置くのは
  チップ族ごとに1ファイルずつの代表例だけ(`banks/README.md`参照)
- スキーマ(`config_schema/`)を変更した際も、検証・追従が必要なのは
  この例示ファイルの範囲だけでよい
- 逆に、例示ファイルは参照切れを作らないこと(プロファイルが存在しない
  バンクを指したままにしない)

## ビルド

configureとbuildでプリセットの名前が違う(`CMakePresets.json`参照)。

```bash
cmake --preset windows-vs2026-x64            # configure。他に windows-vs2022-x64 / linux-ninja / macos-ninja 等
cmake --build --preset windows-vs2026-release
ctest --preset windows-vs2026-test
```

GUI(Dear ImGui)も含める場合は、configureプリセット
`windows-vs2026-x64-gui-imgui`(`FITOM_GUI_IMGUI=ON`)を使う。対応する
buildプリセットは無いので、`cmake --build build/windows-vs2026-x64-gui-imgui --config Release`
でビルドし、`ctest --test-dir build/windows-vs2026-x64-gui-imgui -C Release`
でテストする。`third_party/imgui`・`third_party/glfw`はgit submoduleとして
別途取得する(`git submodule update --init --recursive`)。

## 作業時の必須ルール

- **ユーザーへの応答(報告・説明・要約)は必ず日本語で行う。** コード中の識別子・コメントの言語は既存の慣習(日本語コメント)に従う
- **変更のたびに必ずビルド・既存テスト(`tests/fitom_tests`)を実行して確認する。** 警告が増えていないかも確認すること
- **最小限の原則的な変更を優先する。** その場しのぎの回避策より、既存のアーキテクチャに沿った一貫性のある実装を選ぶ
- **チップファミリー間の一貫性は必須要件。** ある制約を1つのチップに適用したら、類似する他のチップにも同じ設計を適用する
- **`docs/STATUS.md`には現在の状態だけを書く。** 修正の経緯や調査の記録は`docs/worklog/YYYY-MM.md`(その月のファイル)の末尾に足す
- **ドキュメントと実装の設計意図を一致させ続ける。** 挙動を変えたら、対応する`docs/`配下の記述も同じコミット/セッション内で更新する
- 新しいgetter/setterを追加する前に、既存のインターフェース(`IMidiCh`・`ISoundDevice`等)に既に同等のものが無いか確認する
- JSONスキーマ(`config_schema/`)を変更したら、対応する利用者向けマニュアル(`docs/manuals/`の`profile-reference.md`・`bank-reference.md`・`hwpatch-reference.md`等)も更新する
- **テストケース名(`TEST_CASE`の第1引数)はASCIIに限る。** Windowsのctestが非ASCIIの名前を見つけられず失敗するため(`tests/test_test_names.cpp`が検査する)。日本語の説明はSECTION名やコメントに書く
- チップドライバがHwPatchのどのフィールドを参照するか(または値の変換方法)を変えたら、`spec/chip-capabilities.json`も同じセッション内で更新する
- **コミットのauthor/committerは、このリポジトリの履歴で使われている単一のIDに揃える。** マシンのグローバル設定とは別なので、クローンごとに`git log -1 --format='%an <%ae>' origin/main`の値を`git config --local user.name`/`user.email`へ設定してからコミットする

## ドキュメントの地図

`docs/`は読者ごとにディレクトリを分けている。文書の一覧は`README.md`の
「ドキュメント」にある。

| 場所 | 読者 | 書くもの |
|---|---|---|
| `docs/STATUS.md` | AI・開発者 | 現在の実装状況・未確認事項・既知の未対応(**作業開始前に必読**)。GUIの実装済み画面もここ |
| `docs/worklog/` | AI・開発者 | 変更・修正・調査の経緯。経緯を書く唯一の場所(月ごとに1ファイル。書式は`docs/worklog/README.md`) |
| `docs/manuals/` | エンドユーザー | MIDIメッセージ・設定ファイル・バンクファイル・音色パラメータの仕様 |
| `docs/design/` | 本体の開発者・AI | 内部の設計と、その理由 |
| `docs/interfaces/` | 別リポジトリの開発者 | プラグイン・外部ツールとの契約 |
| `docs/proposals/` | 開発者・AI | 未実装の設計検討 |
| `docs/plans/` | AI・開発者 | 進行中の作業計画。終わったら決定事項と見送った案を`docs/worklog/`へ移して消す |
| `spec/chip-capabilities.json` | 外部ツール | `docs/manuals/hwpatch-reference.md`の機械可読版(チップ種別ごとの有効パラメータ・値域・実効解像度・排他条件) |

文書を書くときの規則:

- **`docs/manuals/`には、コード内のシンボル(クラス名・関数名・ソースファイル名)と経緯を書かない。** 現在の挙動を、利用者が書く値・送るメッセージの言葉で書く。開発用の文書への参照も貼らない
- **`docs/design/`・`docs/interfaces/`にも経緯を書かない。** 「なぜこうなっているか」は現在形の理由として書き、「以前は〜だった」「〜を修正した」は`docs/worklog/`に書く。見送った案は、文書の「見送った案と理由」の節に理由と一緒に残してよい
- `docs/proposals/`の文書を実装したら、`docs/design/`へ移し、現行の設計の説明に書き直す(検討の経緯は`docs/worklog/`へ)
- 他の文書・コードコメント・スキーマから文書の節を指すときは、節番号ではなく見出しの文言で指す(`docs/manuals/`のように見出しに番号を振っている文書の番号は可)。見出しの文言を変えるときは、その見出しを指す参照をgrepして一緒に直す
- コードと食い違う記述を見つけたら、文書を現在の挙動に合わせる。挙動の方を直すべきなら、`docs/STATUS.md`の4節に載せて利用者の判断を仰ぐ

## 別プロジェクトとの境界

- **パッチエディタ**:別プロジェクトとして実装される。このリポジトリの
  スコープは`backends/midi_pipe/`(インターフェース)と
  `docs/interfaces/plugin-midi-pipe.md`(仕様書)の整備のみ
- **FitomHwIF / FitomEmuIF / FitomSf2IF**:HW I/Fプラグインの実装は別リポジトリ。このリポジトリは
  `plugin_sdk/include/fitom/IHWPlugin.h`(契約)と
  `docs/interfaces/plugin-hwif.md`(要件定義)を整備する側

## デバッグ・検証の作法

- スクリーンショットでの確認が必要な場合、Linux環境では`Xvfb`+
  `ImageMagick(import)`で仮想ディスプレイ上のGUIを撮影できる
- 実際のオーディオデバイスが無い環境(CI・サンドボックス等)では、
  `HWPlugin_Init`はRtAudio等のストリームオープンに失敗するのが正常。
  これは環境側の制約であり、コードの不具合ではない
- 変更後は必ずフルビルド+`tests/fitom_tests`を実行し、警告が新たに
  増えていないか確認してから完了とする
- **Windows環境(ユーザーの実機)では、`fitom_gui`のスクリーンショットに
  よる目視確認を自分で行おうとしない。** ユーザーの実デスクトップ上での
  ウィンドウキャプチャは、フォーカス制御が意図通りに働かない場合に
  無関係なウィンドウ(メールクライアント等、機微な内容を含みうる)を
  誤って撮影するリスクがあり、ユーザーの他の作業とも競合する。
  ビルド・`tests/fitom_tests`等GUI以外の検証が完了した段階で、GUIの
  目視確認(該当機能の起動手順を添えて)をユーザーに依頼すること
