# 文書再編の作業計画

対象読者: この作業を続けるセッション(AI)と、レビューする開発者。
作業が終わったら、この文書は残すか消すかを利用者に確認する。経緯は
`docs/worklog/`に書き、ここには計画・決定事項・残作業だけを書く。

確度の印: **確認済み**(何で確かめたかを添える) / **未検証** / **推測**。
「監査」と書いたものは、2026年10月23日に4本の読み取り専用の監査で
見つけ、コードを grep/Read して照合したと報告されたもの。この文書の
筆者が自分で照合したものだけに**確認済み**を付ける。

---

## 1. 目的

文書の置き場所と書き方の規則(現在の状態と経緯を分ける、利用者向け
文書にコード内シンボルと経緯を書かない、読者ごとに書く)ができる前の
文書が、規則に合っていない。読者別に再配置し、経緯を worklog に移し、
コードと食い違う記述を直す。

## 2. 決定事項

| # | 決定 | 前提(これが変われば見直す) |
|---|---|---|
| D1 | `docs/`を読者別のディレクトリに分ける(3節の構成) | 読者の区分が「利用者 / 本体の開発者・AI / 別リポジトリの開発者 / 未実装の検討」の4つで足りること |
| D2 | 他のリポジトリからの参照は、各リポジトリのコードコメントと現役の文書だけを直す。過去ログに当たる文書(FITOM_patch_editor の`docs/history/`、各リポジトリの worklog 等)は直さない | 各リポジトリを触る許可をその都度得られること。触る前に`git status`を見る |
| D3 | worklog の既存の行を、文面を変えずに見出し形式へ機械変換する。行どうしの「上記」「下記」の参照は直せないので、`worklog/README.md`の注記は残す | — |
| D4 | コードのコメントと`config_schema/`の description にある経緯・古い記述も、今回の範囲に含める | — |
| D5 | 各文書の作業は「中身を変えない移動・分割」と「経緯の移動・誤りの修正」を別のコミットに分ける | レビューで移動と書き換えを見分けられるようにするため |
| D6 | P3 は1本ずつ順に、同じ判断基準で書き直す | — |
| D7 | 節は自由に並べ替える。コード・スキーマ・他の文書・他のリポジトリからの参照は、節番号ではなく見出しの文言で指す形に直す | 見出しの文言を変えるときは、その見出しを指す参照を grep して一緒に直すこと |
| D8 | 文書と食い違うコードの挙動は、今回はコードを直さない。照合したうえで STATUS 4節に載せ、利用者向けマニュアルには実際の挙動を書く | 直すかどうかは項目ごとに利用者が決める |
| D10 | `chip-drivers.md`を2つに分ける。共通の仕組み(継承構造、サブデバイスの自動生成、同種デバイスの束ね、ステレオ化、VoicePatchTypeの対応表、特殊ルーティングの判断基準)は`docs/design/chip-drivers.md`に、チップファミリーごとの実装の注意とレジスタは`docs/design/chip-notes.md`に置く | — |
| D9 | 設計文書から worklog へ移す経緯は、元の文書ごとに1件として`docs/worklog/2026-10.md`に置き、段落は文面を変えずに入れる。worklog に同じ内容が既にある段落は移さずに削り、そのことを同じ件に書く | — |

## 3. 再編後の構成

| 新しいパス | 読者 | 元の文書 |
|---|---|---|
| `README.md` | 初めて見る人・ビルドする人。文書の入口 | (据え置き。文書一覧をここに一本化) |
| `CLAUDE.md` | AI。規則と文書の地図だけ | (据え置き。GUIの現状は STATUS へ寄せる) |
| `docs/STATUS.md` | 現在の状態 | (据え置き) |
| `docs/worklog/` | 経緯の唯一の置き場 | (据え置き。形式だけ変える) |
| `docs/plans/` | AI・開発者。進行中の作業計画 | (新設。この文書) |
| `docs/manuals/midi-message-reference.md` | 利用者 | (据え置き) |
| `docs/manuals/midi-implementation-chart.md` | 利用者 | (据え置き) |
| `docs/manuals/hwpatch-reference.md` | 利用者 | + `voice-parameter-reference.md`の利用者向け部分 |
| `docs/manuals/swpatch-reference.md` | 利用者 | (据え置き) + `banks/sw/README.md` |
| `docs/manuals/layered-patch-reference.md` | 利用者 | (据え置き) |
| `docs/manuals/profile-reference.md` | 利用者(新設) | `config-design.md`の利用者向け部分 + `banks/README.md`の`banks`・`bank_overrides` |
| `docs/manuals/bank-reference.md` | 利用者(新設) | `patch-structure-design.md`の利用者向け部分(ドラムキット、PCMバンク、builtin、PSG共有バンク等) |
| `docs/design/chip-drivers.md` | 同上 | `chip-driver-architecture.md`のうち全チップに共通する仕組み |
| `docs/design/chip-notes.md` | 同上 | `chip-driver-architecture.md`のチップ別の節 + `voice-parameter-reference.md`の内部向け部分 + `voice-data-design.md`のPSGエンベロープとOPN FXモード |
| `docs/design/patch-resolution.md` | 同上 | `patch-structure-design.md`の内部向け部分 |
| `docs/design/voice-processor.md` | 同上 | `voice-data-design.md` |
| `docs/design/midi-processing.md` | 同上 | `midi-implementation-status.md` |
| `docs/design/config-loading.md` | 同上 | `config-design.md`の残り |
| `docs/design/sf2-direct-path.md` | 同上 | `sf2-fluidsynth-integration.md` |
| `docs/design/terminology.md` | 同上 | `terminology.md`(用語の定義だけに縮める) |
| `docs/interfaces/plugin-hwif.md` | 別リポジトリの開発者 | `plugin-hwif.md` |
| `docs/interfaces/plugin-midi.md` | 同上 | `plugin-midi.md` |
| `docs/interfaces/plugin-midi-pipe.md` | 同上 | `plugin-midi-pipe.md` |
| `docs/interfaces/patch-editor-connect-sequence.pu` | 同上 | `patch-editor-connect-sequence.pu` |
| `docs/proposals/vgm-recording.md` | 未実装の検討 | `vgm-recording.md` |
| `docs/proposals/opm-opz-hw-lfo.md` | 未実装の検討 | `opm-opz-hw-lfo.md` |
| (削除) | — | `DESIGN.md`、`chip-driver-migration.md`、`voice-parameter-reference.md`、`banks/sw/README.md` |

`DESIGN.md`の現役の部分は README のアーキテクチャ概要・ビルド要件と`docs/interfaces/`で覆われていたため、`architecture.md`は作らなかった。

`spec/`・`banks/README.md`・`legacy/README.md`は場所を変えない。

## 4. 作業の順序と進捗

- [x] P0 この計画書を置く
- [x] P1 worklog を見出し形式へ機械変換する(D3)
- [x] P2 ファイルを新しいパスへ移し、リポジトリ内の参照パスを付け替える(中身は変えない)
- [x] P3 文書ごとに分割・経緯の移動・誤りの修正(5節の一覧)
- [ ] P4 コードコメントとスキーマの description の経緯・古い記述(D4)
- [ ] P5 CLAUDE.md・README・STATUS の文書一覧を一本化し、規則を足す
- [ ] P6 他のリポジトリの参照を直す(D2)

## 5. 文書ごとの作業一覧

以下は監査の報告。各項目は P3 で直す前に照合する。

### 5.1 削除する文書

- `chip-driver-migration.md`: 移行は完了。削除の前に、OPK/OPK2 が未移植であることを STATUS 4節へ移す(監査: DeviceFactory に生成処理が無い)
- `DESIGN.md`: 移行計画で、全フェーズが終わっている。削除済み
- `voice-parameter-reference.md`: 利用者向け部分(レジスタイメージからの変換表、AWM の校正フィールド)を`hwpatch-reference.md`へ、内部向け部分を`chip-drivers.md`へ移してから削除。こちらの方が古い記述(ADPCM の`ops[0].WS`、OPLL の表、共通 ext)はマニュアル側を正とする
- `banks/sw/README.md`: `banks/README.md`の1行と`swpatch-reference.md`へのリンクに畳む。VTL の式がコードと食い違う(監査: `VoiceProcessor.cpp`522-524)

### 5.2 分割する文書

- `config-design.md`: 約8割が利用者向けで、`profile-reference.md`へ移す。内部シンボルを使った説明は書き直す。開発者向けに残すのは、プロファイル切替の再起動方式、読み込み順序、検証で弾いたときの扱い。`banks`・`bank_overrides`・`sf2_banks`・`sf2_channel_windows`・`psg_fallback_chip`が記載されていないので、マニュアルを新設するときに足す
- `patch-structure-design.md`: 利用者向け部分(推奨バンク方針、OPLL ROMのhwProg、builtinメタバンク、SampleZoneのsw_bank、PSG共有バンク、ドラムマップ・ドラムキット、choke_groups、ToneLayer例、pcm_banksのgroup、ディレクトリ構成、相対パス)を`bank-reference.md`へ。945-1068行(既存バグと呼び出し順序の欠陥)は worklog へ。ただし「assignCh は updateVoice の前に onNoteOn を呼ぶ」「レガートは assignCh を通らない」は設計の事実として残す。既知の技術的負債は STATUS 4節へ
- `terminology.md`: 用語の定義(kfs、各パッチの呼び名、ドラムバンク/キット、無音バンク)だけ残す。CC#0の表・無音バンク・GM2 は`midi-processing.md`に、0x70・OPL_RHY は`patch-resolution.md`に既に同じ内容があるので削る。Fnum 共有は`chip-drivers.md`4.4節へ
- `voice-data-design.md`: VoiceProcessor の仕様だけを残す。PSG エンベロープと FX モードは`chip-drivers.md`へ。旧 FMVOICE との比較・後方互換・フェーズ6の手順は削除

### 5.3 書き直す文書

- `midi-implementation-status.md`→`midi-processing.md`: 状況の表は manuals への参照に置き換え、内部の配線だけ残す。343-466行は worklog に同じ文面があるので削除(監査)。残す設計規則: noteOff は全レイヤーのエントリを解放する / Running ch を奪うときは必ず noteOff する / wasReleasing は assign() で確定する。worklog に無い経緯(CC#126 新設、NRPN 96,1 とドラム用 NRPN の修正、HW LFO の lfoOwner_ 撤廃、プライベートSysEx の新設)は移す。Bank Select の三重の重複をここに一本化
- `sf2-fluidsynth-integration.md`→`sf2-direct-path.md`: 実装済みの現行設計の説明に書き直す。①〜⑧の番号はコードコメントから参照されている(13ファイル26か所)。番号を変えるなら P4 で付け替える。不採用案は「見送った案と理由」の節に集約する。2節の FluidSynth 内部構造は FitomSf2IF 側の知識なので削る
- `chip-driver-architecture.md`→`chip-drivers.md`: §2 に紛れた OPL4 AWM の内容を §4 の独立した節へ。§7・§8(個別バグの記録)は要点だけ残して worklog へ。継承図の欠け(C3801、COPL2EX、COPLRhythm、CEPSG、CSAA1099、CY8960Delta、COPL4AWM、CLinearPanDevice)を足す。節番号はコード・テスト・スキーマ・他の文書から参照されている(OPN_new.cpp「セクション7」、OPLL_new.cpp「4.4節」、Config.h と test_config.cpp「3.1」、test_ssgs.cpp「3節」、PatchManager.cpp・hwbank.schema.json「5.」、STATUS「4.5.3節」)
- `plugin-hwif.md`: 関数一覧に必須の`HWPlugin_Init`を足す。params_json の節を Open の位置へ戻す。`fitom_fmhwif`を FitomEmuIF に直す。374-491行の「将来実装」のビルド手順と古い config 例を整理し、SF2 プラグインへの追加契約(WriteBlock で生MIDI、`soundfonts`、sub-cmd 0x05)は現役の契約として残す
- `plugin-midi.md`: 「同時ロードは1 DLLのみ」(内部パイプと同時に読む)、「コアはキューに積む」(実際はコールバックのスレッドで mutex を取って同期処理)、参照先の無い「後述」を直す
- `plugin-midi-pipe.md`: GUI からのキオスク起動の引数(`<profile.json> <kind> <bank-file> <prog>`)を足す。5.5の「CC#121 でオーバーライドも解除」は誤り(プログラムチェンジでだけ消える)。割り当て ch が 9 のときリズムチャンネルになり直接デバイス選択が効かないことを書く(コードで直すかは利用者の判断)
- `vgm-recording.md`・`opm-opz-hw-lfo.md`: 日付つきの記録を外し、古くなった行番号参照を関数名に直す。`opm-opz-hw-lfo.md`5節の既存の不具合は STATUS 4節にも載せる

### 5.4 マニュアルの誤り(利用者向けなので優先)

- `midi-message-reference.md`: ch10 の記述(起動時は ch9 がリズムチャンネル。監査: CFITOM.cpp:318)。マスターボリュームが「システム全体の音量」になっていない(**確認済み**: `CSoundDevice::masterVolume_`は代入だけで読まれていないことを grep で確認。音では確かめていない)。CC#76-78 が表では「上書き」、7節ではオフセット方式
- `midi-implementation-chart.md`: CC#76-78 の「上書き」
- `swpatch-reference.md`: 「ベロシティ感度はキャリアのみ」が VTL の説明と矛盾
- `layered-patch-reference.md`・`hwpatch-reference.md`15節: Patch 単位の`sw_bank`/`sw_prog`は存在しない(監査: PatchData.h、patchbank.schema)

### 5.5 STATUS・CLAUDE.md・README

- STATUS: 「設定スキーマ・ドキュメント」の表を削除(文書一覧は README に一本化)。ファイル一覧の漏れ(DrumData.h、PcmBankData.h、SccWaveData.h、FITOMdefine.h、Fnum.h/.cpp、Sf2BankRegistry.h)。`AudioEngine`の廃止行を削除。RPN 2/7F7F は実装済み(監査)。疑似デチューンは DT2 ではなく PDT(監査)。GUI の現状を CLAUDE.md から寄せる
- CLAUDE.md: ビルド手順のプリセット名(**確認済み**: `CMakePresets.json`を読み、build プリセットは`windows-release`等で configure プリセットと名前が違うことを確認)。ディレクトリ構成に`config_schema/`・`third_party/`が無い。「主要ドキュメント」表を地図に作り直す。利用者向け文書の規則・設計検討文書の扱いの規則を足す
- README: 文書一覧を再編後の構成に合わせる。ビルド要件の Boost の欄を直す(**確認済み**: コードが使うのは Boost.Log(と date_time)と format だけで、interprocess は使っていない。thread は CMake の必須コンポーネントに残っている)
- ルートの`CMakeLists.txt`63行目付近の Boost の用途のコメント(named_mutex)が古い(P4)

## 6. 他のリポジトリの参照(P6)

2026年10月23日に grep で数えた(**確認済み**)。過去ログは除く。

- FITOM_patch_editor: `apps/gui/`・`include/fpe/`・`src/`のコメント、`docs/ARCHITECTURE.md`、`CLAUDE.md`
- FITOM_staging: `README.md`、`CLAUDE.md`、`tools/voice_convert/`、`config_schema/`(写し)、`config/profiles/hw_plugins/fitom_sf2if_profile.json`。`docs/voice-parameter-reference.md`という写しを持っている
- FitomHwIF: `src/HWPluginImpl.cpp`
- FitomEmuIF: `doc/plan.md`

## 7. 見送った案

- パスを変えずに中身だけ整理する案。理由: 読者がファイル名でしか判別できない状態が残る。利用者が読者別のディレクトリを選んだ
- 移動元に案内だけのファイルを置く案、他のリポジトリの参照を放置する案。理由: 利用者が各リポジトリを直す案を選んだ
- worklog を凍結して新規分だけ新形式にする案。理由: 利用者が機械変換を選んだ
- コードコメントとスキーマを別作業にする案。理由: 利用者が今回の範囲に含めると決めた
- P3 を4グループに分けて並行で書かせる案。理由: 経緯とみなす範囲や重複の寄せ先の判断が文書ごとに揺れる。利用者が1本ずつ順に進める案を選んだ
- 節番号を保ち、新しい節は末尾に足す案。理由: 構成の整理に制約が残る
- マニュアルを正としてコードの挙動を直す案。理由: 文書の再編と挙動の変更を混ぜない。利用者が後で個別に決める案を選んだ
