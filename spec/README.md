# spec/ — 外部ツール向け機械可読仕様

このディレクトリには、FITOM_X本体の設定ファイルではなく、**外部ツール
(パッチエディタ等)がFITOM_Xのデータ構造を正しく扱うために参照する
機械可読な仕様データ**を置く。

`config_schema/`との違い:

| | 目的 | 検証対象 |
|---|---|---|
| `config_schema/*.schema.json` | 設定/バンクファイルの構文を検証するJSON Schema | ファイルそのもの |
| `spec/*.json` | チップごとの意味論・値域・排他条件を記述したデータ | ファイルの**中身が特定チップで意味を成すか** |

FITOM_Xのコア(C++)はこれらのファイルを読まない。実装の正は`core/`側の
コードであり、このディレクトリはその内容を外部ツールが利用できる形へ
書き写したものである。

---

## chip-capabilities.json

チップ種別(`VoicePatchType`)ごとに、HwPatch(`*.hwbank.json`)のどの
フィールドが実際に意味を持ち、値域・実効解像度・排他条件がどうなるかを
定義する。`hwbank.schema.json`は全チップ共通の型上限しか表現できない
ため、チップ単位の妥当性検証やパラメータUIの構築にはこちらを使う。

対応するエンドユーザー向け解説は
[`docs/manuals/hwpatch-reference.md`](../docs/manuals/hwpatch-reference.md)。

### 構造

```
format / format_version   フォーマット識別子とバージョン
conventions               各キーの読み方(このファイル自身の説明)
patch_kinds               チップの音色データ形式の分類
parameters                パラメータ共通定義(スコープ・JSON上の位置・型上限)
common_patch_fields       全チップ共通のパッチフィールド(prog/name/sw_bank/sw_prog)
chips[]                   チップごとの定義
```

`chips[]`の各要素:

| キー | 内容 |
|---|---|
| `id` | チップ識別子(`VOICE_PATCH_*`のサフィックス) |
| `voice_patch_type` / `_hex` | 直接デバイス選択値(CC#0)そのもの |
| `display_name` / `devices` | 表示名と対応する実チップ型番 |
| `voice_group` | HwBankの名前空間(同じ値のチップはバンクを共有しうる) |
| `bank_voice_patch_type` | バンク検索に使うCC#0の値(PSG系は全チップ`64`を共有) |
| `patch_kind` | 音色データの形式(`patch_kinds`参照) |
| `operator_count` | `ops[]`の要素数。可変の場合はオブジェクト |
| `params` | このチップで意味を持つパラメータ **のみ** |
| `notes` | 補足 |

### 読み方の要点

- **`params`に無いパラメータは、そのチップでは一切参照されない。** 値を
  書いてもドライバが無視するため、エディタは非表示または無効表示にして
  よい。
- `range`はそのチップで意味を持つ値域。`effective_range`はさらに狭い
  「実際に音へ反映される範囲」で、これを外れた値は飽和する。
- `quantum`は実機レジスタ1段に対応するパッチ値の刻み。例えば`2`なら、
  隣り合う奇数/偶数の値は同じ結果になる。省略時は1(丸めなし)。
- `condition`は他フィールドや発音チャンネルによる有効条件を表す。
  条件を満たさない場合、そのパラメータは参照されない。
- `same_params_as`を持つチップは`params`が空で、参照先のチップ定義を
  そのまま使う。
- `mapping`が`attenuation_db`のパラメータは減衰量(0=最大音量)で、
  `step_db`が1段あたりのdB値。

---

## 更新のルール

チップドライバ(`core/src/*_new.cpp`)がHwPatchのどのフィールドを参照
するか、あるいはレジスタへの変換方法を変更した場合は、同じセッション内で
`chip-capabilities.json`と`docs/manuals/hwpatch-reference.md`の両方を
更新する。新しい`VoicePatchType`を追加した場合は`chips[]`への追加も必須。
