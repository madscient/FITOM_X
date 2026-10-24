# 依頼: FitomHwIF の IHWPlugin.h の写しを同期する

FITOM_X のセッションが書いた、FitomHwIF で作業するセッション向けの依頼文。
作業が終わったら、FITOM_X のセッションがこの文書を消す。

## 背景

FITOM_X は HW プラグインの契約に、任意の関数 `HWPlugin_SetMasterVolume` を足した
(FITOM_X のコミット `d214ff9`)。音声出力を持つプラグインがミキサーでマスターボリュームを
掛けるための関数で、実機のチップにはミキサーが無いので、FitomHwIF は実装しなくてよい
(エクスポートしなければ、FITOM は呼ばない)。

## やること

- [ ] 作業の前に fetch / pull して最新にする
- [ ] `plugin_sdk/include/fitom/IHWPlugin.h` を、FITOM_X の `d214ff9` の
      `plugin_sdk/include/fitom/IHWPlugin.h` で置き換える
      (`git -C <FITOM_X> show d214ff9:plugin_sdk/include/fitom/IHWPlugin.h` で取り出せる)。
      手元の写しは、その1つ前のコミットの FITOM_X と同一だった(差分は
      `HWPlugin_SetMasterVolume` の節と、コメントの旧名 FitomIFTest の修正だけ)
- [ ] `HWPlugin_SetMasterVolume` はエクスポートしない。ビルドが通ることを確かめる
- [ ] FitomHwIF の文書がエクスポート関数の一覧を持っていれば、「実装しない任意関数」として
      `HWPlugin_SetMasterVolume` を載せる

## 守ること

- FITOM_X は参照だけにして、編集しない
- FitomHwIF の CLAUDE.md と規約に従う

## 終わったら

コミットのハッシュをユーザーに報告する。FITOM_X のセッションが、`docs/STATUS.md` の
5節(周辺リポジトリの追従状況)を更新し、この文書を消す。
