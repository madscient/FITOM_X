# 依頼: FITOM_staging のスキーマの写しと、古くなったプロファイルの注記を直す

FITOM_X のセッションが書いた、FITOM_staging で作業するセッション向けの依頼文。
作業が終わったら、FITOM_X のセッションがこの文書を消す。

## 背景

FITOM_X の2つの変更で、FITOM_staging の記述が古くなった。

- FITOM_X `d214ff9`: マスターボリュームを HW プラグインへ任意関数
  `HWPlugin_SetMasterVolume` で渡すようにし、`config_schema/profile.schema.json` の
  `master_volume` の `description` を書き換えた(構造は変えていない)
- FITOM_X `5a9f054`: SAA1099 のマスタークロックを、他の PSG 系と同じく HW プラグインが
  報告する値(プラグイン設定の `clock`)から取るようにした。これまでは 8MHz 固定だった

## やること

- [ ] 作業の前に fetch / pull して最新にする(10月24日の時点で、手元が origin より8件遅れていた)
- [ ] `config_schema/profile.schema.json` を、FITOM_X の `d214ff9` の同じファイルで置き換える。
      10月24日の時点で、origin の写しと FITOM_X の違いは `master_volume` の `description` だけだった
      (他のスキーマファイルは同一)。置き換える前に、違いがそれだけであることを確かめる
- [ ] `config/profiles/emu_ibmpc.profile.json` の `_comment` にある
      「SAAのclockはCSAA1099のコンストラクタ既定値8MHz固定に合わせる必要がある。」を消すか、
      「SAAのclockは、FITOM_Xがこの値で周波数を計算する」の趣旨に直す。
      プラグイン設定(`hw_plugins/fmemuif_ibmpc.profile.json`)の `clock: 8000000` は変えない
      (FitomEmuIF はこの値をそのまま返すので、修正の前後で音程は変わらない)

## 守ること

- FITOM_X は参照だけにして、編集しない
- FITOM_staging の CLAUDE.md と規約に従う

## 終わったら

コミットのハッシュをユーザーに報告する。FITOM_X のセッションが、`docs/STATUS.md` の
5節(周辺リポジトリの追従状況)を更新し、この文書を消す。
