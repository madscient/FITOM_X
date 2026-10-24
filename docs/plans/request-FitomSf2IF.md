# 依頼: FitomSf2IF の IHWPlugin.h の写しを同期する

FITOM_X のセッションが書いた、FitomSf2IF で作業するセッション向けの依頼文。
作業が終わったら、FITOM_X のセッションがこの文書を消す。

## 背景

FITOM_X の HW プラグインの契約(`plugin_sdk/include/fitom/IHWPlugin.h`)には、
FitomSf2IF の写しより後に、任意の関数の組が2つ足されている。

- 部位ごとのゲイン(`HWPlugin_GetPartCount` / `GetPartName` / `SetPartGain` / `GetPartGain`)
- マスターボリューム(`HWPlugin_SetMasterVolume`、FITOM_X のコミット `d214ff9`)

どちらも任意で、FitomSf2IF は実装しなくてよい。FITOM は SF2 のデバイス(`chip: "SF2"`)に
`HWPlugin_SetMasterVolume` を呼ばず、マスターボリュームはこれまでどおり SysEx
(`F0 7F 7F 04 01 00 <vol> F7`)で送る。写しを同期しても、FitomSf2IF の動作は変わらない。

## やること

- [ ] 作業の前に fetch / pull して最新にする
- [ ] `plugin_sdk/include/fitom/IHWPlugin.h` を、FITOM_X の `d214ff9` の
      `plugin_sdk/include/fitom/IHWPlugin.h` で置き換える
      (`git -C <FITOM_X> show d214ff9:plugin_sdk/include/fitom/IHWPlugin.h` で取り出せる)
- [ ] 部位ゲインの組と `HWPlugin_SetMasterVolume` はエクスポートしない。ビルドが通ることを確かめる
- [ ] マスターボリュームの SysEx の扱いが、FITOM_X の `docs/interfaces/plugin-hwif.md`
      「SoundFont プラグインへの追加の約束」節と食い違っていないかを見る(変える必要は無いはず)

## 守ること

- FITOM_X は参照だけにして、編集しない
- FitomSf2IF の CLAUDE.md と規約に従う

## 終わったら

コミットのハッシュをユーザーに報告する。FITOM_X のセッションが、`docs/STATUS.md` の
5節(周辺リポジトリの追従状況)を更新し、この文書を消す。
