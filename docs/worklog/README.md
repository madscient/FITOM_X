# 作業記録(worklog)

FITOM_Xの変更・修正・調査の経緯を、月ごとに1ファイルで置く。現在の実装状況は
`docs/STATUS.md`に、現在の設計は各設計文書に書き、そこに至った経緯はここに書く。

- ファイル名は`YYYY-MM.md`。1件を1つの`##`見出しにし、見出しには何をしたかを1文で書く
- 本文は1文1行で書く。**確認済み**・**未検証**・**前提**・**見送った案**などのラベルの前で段落を分ける
- 本文の後に`- 状態:`と`- 関連ドキュメント:`の2行を置く
- 件をどの月に置くかは、その件に書かれた最初の日付で決める
- 新しい記録は、該当する月のファイルの末尾に足す
- 他の件を指すときは、位置(「上記」「下記」)ではなく見出しの文言で指す

## 2026年7月〜10月の既存の件について

7月・8月の件と10月の一部は、`docs/STATUS.md`の表から文言を変えずに移し、その後
表の1行を見出しと本文に機械的に分けたもの。並びは移す前のSTATUS.mdでの並びのまま
(日付順ではない)。本文中の「上記」「下記」「上の表」「次の行」は移す前の
STATUS.md内の位置関係を指すため、別の件や別の月のファイル、`docs/STATUS.md`に
残った記述を指していることがある。移す前の版は、このREADME.mdを追加したコミットの
親で読める(`git log --diff-filter=A -- docs/worklog/README.md`)。

## 文書のファイル名の対応

記録の中の文書名は、書いた時点の名前のまま。現在の場所は次のとおり。

| 記録の中の名前 | 現在の場所 |
|---|---|
| `chip-driver-architecture.md` | `docs/design/chip-drivers.md`(共通の仕組み)と`docs/design/chip-notes.md`(チップ別) |
| `patch-structure-design.md` | `docs/design/patch-resolution.md` |
| `voice-data-design.md` | `docs/design/voice-processor.md` |
| `midi-implementation-status.md` | `docs/design/midi-processing.md` |
| `config-design.md` | `docs/design/config-loading.md` |
| `sf2-fluidsynth-integration.md` | `docs/design/sf2-direct-path.md` |
| `terminology.md` | `docs/design/terminology.md` |
| `plugin-hwif.md` / `plugin-midi.md` / `plugin-midi-pipe.md` | `docs/interfaces/`の同名ファイル |
| `vgm-recording.md` / `opm-opz-hw-lfo.md` | `docs/proposals/`の同名ファイル |
| `voice-parameter-reference.md` | 削除。利用者向けの内容は`docs/manuals/hwpatch-reference.md`、チップごとのレジスタと内部処理は`docs/design/chip-drivers.md` |
| `DESIGN.md` / `chip-driver-migration.md` | 削除(2026年10月の「`docs/DESIGN.md`と`docs/chip-driver-migration.md`を削除した」の件) |
