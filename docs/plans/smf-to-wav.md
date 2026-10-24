# 計画: SMF→WAV 変換器(`fitom_render`)の残り

FITOM_X側の実装は済んだ(決めたことと見送った案は`docs/worklog/2026-10.md`の
「SMFをWAVへ変換するヘッドレスの実行ファイルfitom_renderを足した」)。
この文書には、まだ終わっていない作業だけを置く。全部終わったら、決定事項を
`docs/worklog/`へ移してこの文書を消す。

## 他のリポジトリの実装待ち

- [ ] FitomEmuIFがオフライン描画の組を実装する(依頼文: `request-FitomEmuIF.md`)
- [ ] FitomSf2IFがオフライン描画の組を実装する(依頼文: `request-FitomSf2IF.md`)
- [ ] 両方が済んだら、stagingのプロファイルで変換して音を確かめてもらう(`docs/STATUS.md`の3節)

## SMFのポート指定

今は`FF 09`(Device Name)・`FF 21`(MIDI Port)を読まず、全トラックをMPU0へ送る。
ユーザーがDAWで書き出したSMFを用意し、ポート指定がどう入っているかを見てから決め直す。
変える費用はSMFの読み込み部(`apps/fitom_render/SmfReader.cpp`)と`main.cpp`の数行と、
利用者向けマニュアル(`docs/manuals/render-reference.md`)の「SMFの扱い」節。

規格上の位置づけ(どちらも**推測**):
- `FF 09`はMMAのRP-019で定められた公式のメタイベント(midi.orgのRP-019の紹介ページの記述による。RP-019の原文は読んでいない)
- `FF 21 01 pp`は規格外で、シーケンサが慣習的に使ってきたもの(midi.orgのフォーラムの利用者の発言による。MMAの公式な記述は見つけていない)

## VGM出力

SMFからのVGM出力は、`fitom_render`に足す(ユーザーの方針)。`fitom_render`はサンプル単位の
時計を持つので、`docs/proposals/vgm-recording.md`が前提にしている壁時計からの換算は、
SMFから作る場合には要らない。GUIから実時間で記録する機能は、引き続きその設計検討の範囲。
