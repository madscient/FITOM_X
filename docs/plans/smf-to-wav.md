# 計画: SMF→WAV 変換器(`fitom_render`)の残り

FITOM_X側の実装は済んだ(決めたことと見送った案は`docs/worklog/2026-10.md`の
「SMFをWAVへ変換するヘッドレスの実行ファイルfitom_renderを足した」)。
この文書には、まだ終わっていない作業だけを置く。全部終わったら、決定事項を
`docs/worklog/`へ移してこの文書を消す。

## 実物のプラグインとの組み合わせ

FitomEmuIF(`3d16a0f`)・FitomSf2IF(`12803b9`)はオフライン描画の組を実装済み。

- [ ] stagingのプロファイルで変換して音を確かめてもらう(`docs/STATUS.md`の3節)

## VGM出力

SMFからのVGM出力は、`fitom_render`に足す(ユーザーの方針)。`fitom_render`はサンプル単位の
時計を持つので、`docs/proposals/vgm-recording.md`が前提にしている壁時計からの換算は、
SMFから作る場合には要らない。GUIから実時間で記録する機能は、引き続きその設計検討の範囲。
