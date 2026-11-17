# 計画: リポジトリの整理(2026年11月)

不要なファイルの削除、依存の取り込みのvcpkgへの一本化、文書の役割・配置・冗長の見直し。
候補は2026年11月18日のセッションで、HEAD `a12e1b3`を点検して作った(文書は3人の点検役が分担して読んだ)。
全部終わったら、決定事項と見送った案を`docs/worklog/`へ移してこの文書を消す。

確度の印: **確認済み(直接)** = この計画を書いたセッションがgrep・cmp・du・ghで確かめた / **確認済み(点検)** = 点検役がgrep・読み比べで確かめた / **コード読み** = コードを読んだ判断で、走らせていない

## 決めたこと(ユーザー、2026年11月18日)

- 削除: `extern/`(手元の空ディレクトリ)、`gui/mfc_shim/`、使われていないビルド設定、`docs/plans/smf-to-wav.md`、`legacy/include/fitom/`の7本
- 残す: `legacy/`の残り(OPKのドライバが未移植で、STATUSとコードのコメント3か所が参照している)、手元の`fitom_cli.log`と古いビルドディレクトリ
- vcpkgを全プラットフォームで必須にし、サブモジュールをすべてvcpkgへ移す。RtMidiは上流のvcpkgが6.0.0までなので、6.1.0をoverlay portで持つ
- 各マシンのvcpkgの作業ツリーをpullし、`builtin-baseline`を上流の最新へ上げる。GLFWは3.5.1のまま。ImGuiは`overrides`で1.92.8に固定する
- `-vcpkg`付きのプリセット2つは、素のプリセットと同じになるので消す
- OPLLのROM音色で存在しないROM(プログラム番号の下位4〜7)を選んだときの説明は、文書を挙動(直前の音色が残る)に合わせる
- `docs/interfaces/plugin-hwif.md`の非公開リポジトリFitomHwIFへのリンクはそのまま

## 作業の順

- [x] 1. 削除(下の「削除の一覧」。サブモジュールと`-vcpkg`のプリセットは2で消す)
- [x] 2. vcpkgへの移行(GUIの目視確認はSTATUSの3節)
- [x] 3. 文書: 食い違い(C-12とIHWPlugin.hのコメントは見送り。理由はworklog)
- [x] 4. 文書: 役割違い(C-29・C-32・B-D5・B-D6は見送り。理由はworklog)
- [x] 5. 文書: 冗長(C-39・A-R8・B-C5は見送り。理由はworklog)
- [ ] 6. 文書: 経緯の語・用語・参照
- [ ] 7. この文書を消し、決定事項をworklogへ移す

## 削除の一覧(合意したもの)

| 対象 | 理由 |
|---|---|
| `extern/`(git管理外) | 空 |
| `gui/mfc_shim/CMakeLists.txt`・`FITOMBridge.cpp`・`FITOMBridge.h` | どこからも`add_subdirectory`されない |
| `CMakePresets.json`の`FITOM_BUILD_BACKEND_FMENGINE`(4か所)・`FITOM_GUI_MFC`(2か所) | どこからも読まれない |
| `CMakeLists.txt`の`FITOM_GUI_QT`と`gui/qt`の分岐、READMEのビルドオプション表のQtの行 | `gui/qt/`が無い |
| `vcpkg.json`の`boost-interprocess` | コードが使っていない |
| `CMakePresets.json`の`windows-vs2022-x64-vcpkg`・`windows-vs2026-x64-vcpkg` | vcpkgを必須にすると素のプリセットと同じになる |
| `CMakeLists.txt`の`FITOM_USE_VCPKG_JSON` | 同上 |
| サブモジュール`third_party/nlohmann-json`・`third_party/rtmidi`・`third_party/imgui`・`third_party/glfw`と`third_party/imgui_cmake/CMakeLists.txt` | vcpkgへ移す |
| `docs/plans/smf-to-wav.md` | 終わった計画。VGMの方針をworklogへ1件足す |
| `legacy/include/fitom/`の`FITOM.h`・`FITOMCfg.h`・`MIDI.h`・`MIDIDEV.h`・`Port.h`・`SoundDev.h`・`TYPES.H` | `legacy/src/`の同名ファイルとバイト単位で同一(確認済み(直接)、cmp) |

## 文書の修正の候補

点検役の担当の頭文字: A=入口・AI向け、B=design、C=manuals・interfaces。

### 食い違い(読んだ人を誤らせる)

**design**
- B-A1 `midi-processing.md:83` マスターボリュームはHWプラグインへも渡す。`CSoundDevice::setMasterVolume()`は存在しない — 確認済み(直接)
- B-A2 `chip-drivers.md:235-237` `stereo_pair:"L"/"R"`の対応チップの列挙が、コードの判定(`getChipPanType() != Mono`)より狭い。同じ文書の:316-320とも食い違う。`profile-reference.md:176`も同じ — 確認済み(点検)
- B-A3 `chip-notes.md:90` OPZ固定周波数「確かめていない」が、STATUS 4節の測定結果と食い違う — 確認済み(点検)
- B-A4 `midi-processing.md:306` `stopAll()`は存在しない(今は`release()`/`choke()`) — 確認済み(直接)
- B-A5 `patch-resolution.md:133` `Config::loadBanks()`は存在しない — 確認済み(直接)
- B-A6 `voice-processor.md:83,88,112` LFOの擬似コードの引数順と式がコードと違う — 確認済み(点検)
- B-A7 `config-loading.md:42-49` SF2バンクはdevices[]より前に読む — 確認済み(直接)
- B-A8 `config-loading.md:21` `fitom.conf.json`を読むアプリにfitom_renderが漏れている — 確認済み(点検)

**manuals・interfaces**
- C-1 `swpatch-reference.md:109` CC#77(デプス)は発音中にも効く — コード読み
- C-2 `hwpatch-reference.md:258,292` OPLLの8番以降のROM音色を「メタバンクのbuiltinから選ぶ」は誤り。メタバンクは検索に登録されない。`spec/chip-capabilities.json`のOPLLのALGのnoteも同じ — コード読み
- C-3 `bank-reference.md:85-86` / `hwpatch-reference.md:287-288` / `design/patch-resolution.md:145` OPLL ROMの4-7を「無音」「未定義」と書いている。コードは「見つからない扱いで直前の音色が残る」 — コード読み
- C-4 `midi-message-reference.md:315-316` 2秒の起点はノートオフ — コード読み
- C-5 `midi-implementation-chart.md:59` タイミングクロックは受信しても無視される — コード読み
- C-6 `hwpatch-reference.md:92` OPN2/OPNAの節でCC#14/15が効くと書いているが、OPN2は`$22=0`固定で、CC#14/15を受けるのはOPMだけ — 確認済み(直接)
- C-8 `bank-reference.md:209` PCMバンクのchipに`OPL2EX`が漏れている — 確認済み(点検)
- C-9 `profile-reference.md:141` FitomHwIFのパラメータに`sample_rate`が漏れている — 確認済み(点検)
- C-10 `profile-reference.md:42-47` logの既定値が表と本文で違う。`fitom.conf.schema.json`の説明も古い — 確認済み(点検)
- C-12 `plugin-hwif.md:498,503-504` 並行呼び出しを「契約」とするか「今の実装」とするかが割れている — 確認済み(点検)
- C-13 `patch-editor-connect-sequence.pu` 引数の形とCC#0=0x79が文書と合っていない — 確認済み(点検)
- C-14 `plugin-midi.md:69` コールバックのtypedefに呼び出し規約のマクロが無い — 確認済み(点検)
- 担当外(C): `IHWPlugin.h:7`の`hw::HWControllerBase`はリポジトリに無い。`:72`に経緯。`profile.schema.json`の`hw_banks.group`が、読み込みで捨てるPSG系を許している
- 担当外(B): `hwpatch-reference.md:310` PSG系の音量を一律「4bit・1段3dB」と書いている。DCSG・SCC・SAAは違う(chip-notes)。STATUS 4節にOPN2/OPNAのAMS/PMSが効かないことが無い

**入口**
- A-R1 `README.md:18-21` アーキテクチャ図。fitom_cliとfitom_renderはbridgeを経由しない — 確認済み(直接)
- A-R2 READMEのdocsの木に`plans/`が無い。A-R5 ドキュメント一覧に`tools/render_measure/README.md`が無い — 確認済み(点検)
- A-R3 README・CLAUDE.mdの`third_party/`の説明に自前の`imgui_cmake/`が含まれない(vcpkgへの移行で解消) — 確認済み(点検)
- A-R6 `README.md:58` 対応チップの表に`SAA1099`の別名が無い — 確認済み(点検)
- A-C1 CLAUDE.mdの構成図の`apps/`に`fitom_render/`が無い — 確認済み(直接)
- A-L1 `legacy/README.md:13-15`(削除で解消) — 確認済み(直接)
- A-B1 `banks/README.md:35-43` 例のパスは、プロファイルを直下に置いたときしか解決しない — 確認済み(点検)
- A-SP1 `spec/README.md:41-53` `chips[]`の表に`builtin_presets`・`builtin_programs`・`sample_zone`が無い — 確認済み(点検)
- A-T1 `tools/render_measure/README.md:118-119` `epsg_expanded.py`の`--tolerance`が無い — 確認済み(点検)
- A-O1 `proposals/opm-opz-hw-lfo.md:4` 「実装は未着手」だが、土台は実装済み — 確認済み(点検)

### 役割違い

- A-P1 `docs/plans/smf-to-wav.md`は終わった計画(削除の一覧)。参照はSTATUS.md:252とvgm-recording.md:4
- A-S5 / A-S13 STATUSに経緯(「直した」「直す前は」「その後〜緩めた」) :191、:274-278
- A-S6 STATUS 3節「準備」の修正の列挙は半分経緯
- A-S14 STATUS 5節「EPSGemuEngine 手元が1件遅れ」は1台のマシンの状態
- A-S11 STATUS 4節の空の見出し「プラグイン SDK」
- A-C4 依頼文の置き場所の規則がSTATUS:270にある → CLAUDE.mdの地図へ
- B-D1 `chip-notes.md:420,822`がSTATUS 4節の未対応と重複
- B-D2 `chip-notes.md:699,739` 仕様書の追従状況(STATUSの役目)
- B-D3 `chip-notes.md:567-570` DSGの2回書き込みはプラグイン側の約束で、interfacesに無い
- B-D4 `chip-notes.md:239-240` FITOM_stagingのバンクの件数
- C-25 `hwpatch-reference.md:87-90`、C-34 `plugin-midi-pipe.md:32,147,175` 将来の計画
- C-28 `hwpatch-reference.md:260-262` 内部の解決の説明(designと重複)
- C-29 `hwpatch-reference.md:238` 確かめた範囲の記録
- C-30 `hwpatch-reference.md:290` 未対応の一覧
- C-31 `midi-message-reference.md:436` 設計の理由
- C-32 `plugin-midi.md:112-143` RtMidiバックエンドの内部実装。契約ではない
- C-33 `plugin-hwif.md:18-25,216-223` 内部のクラス名`HWPort`

### 冗長(片方が古くなる危険があるもの)

- B-C1 Y8960拡張レジスタの番地表: `chip-notes.md:701-715`と`plugin-hwif.md:437-453`
- C-35 OPLL ROM音色の番号付け: bank-reference / hwpatch-referenceの3か所(既にC-3で食い違っている)
- C-36 メタバンク: bank-referenceとhwpatch-reference
- C-37 PCMバンクからの音色の自動作成: bank-referenceとhwpatch-reference
- C-38 midi-message-referenceの中でSF2直行のRPN/NRPNが3か所、役割切替が2か所
- C-39 profile-referenceとスキーマのdescription
- B-C2 パスの解決基点: patch-resolutionとconfig-loading
- B-C3 ソフトウェアLFOのパラメータ表: voice-processorとswpatch-reference
- B-C4 プライベートSysExの電文: midi-processingとmidi-message-reference
- B-C5〜C9 stereo_pairの細目、DSGのpatch_no、クロック、内蔵リズムのGUI列挙、用語集の実装の要点
- A-S1 / A-S2 STATUSがchip-notesやコードコメントと同じ説明を持つ(:58、:62。:62は参照切れも)
- A-S4 Y8960仕様書の版がSTATUSに2か所、chip-notesに1か所
- A-S8 STATUS 4節「パッチの解決」の1項目は「ビルド」の呼び出し元の無いコードへ
- A-C2 CLAUDE.mdの「ビルドとテストを走らせる」が2か所
- A-C3 chip-capabilities.jsonの更新規則がCLAUDE.mdとspec/READMEで範囲が違う
- A-R8 READMEの相対パスの規則(マニュアル2本と重複)

### 経緯の語・用語・参照

- 経緯の語: designの7か所(chip-drivers:240、midi-processing:313、terminology:84、sf2-direct-path:39、chip-notes:226,761、config-loading:67)、C-23 `profile-reference.md:203`、C-24 `hwpatch-reference.md:260`、C-26 `plugin-midi-pipe.md:139,141`
- マニュアルのコードシンボル: C-18〜C-22(DVA・PatchManager・`Patch`・`voicePatchType`・ログの文言)
- 用語: B-F1〜F6(用語集の見出し語が使われていない、「ビルトインリズム」と「内蔵リズム」、DVAの3通りの表記、`Config::`と`FITOMConfig::`)
- 参照切れ: C-15 `swpatch-reference.md:5`のアンカー(確認済み(直接))、C-16、C-17、B-B1〜B3
- 古い日付: A-S12 STATUS 5節の見直し日。A-S7 FITOM_stagingを確かめた版が2つ(`acb733a`と`ef55367`)
- A-W1 worklog/READMEの名前の対応表に、消えた文書が7つ欠けている
