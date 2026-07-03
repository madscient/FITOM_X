#pragma once
// fitom/Fnum.h
// F-number テーブル型定義
// ISoundDevice.h / チップドライバから参照される。
// 実体の CFnumTable は core/src/Fnum.h / Fnum.cpp にある。

// FnumTableType: チップドライバのコンストラクタで指定する
// FnumTableType::Fnumber 形式で参照できるよう名前空間付き enum を使う
namespace fitom {
    enum class FnumTableType {
        None       = 0,
        Fnumber    = 1,   // OPN / OPM 系 F-number テーブル
        TonePeriod = 2,   // PSG / SCC 系トーン周期テーブル
        DeltaN     = 3,   // ADPCM Delta-N テーブル
        OPL4       = 4,
        SSG        = 5,
        // 6 (旧 SAA) は廃止。SAA1099は「周波数レジスタが逆数的かつ
        // オクターブが別レジスタで2^octave倍する」という構造を持ち、
        // getFnumber()の標準的なオクターブ折り畳みロジック(fnum>>=1方式)
        // とは根本的に噛み合わない。CSAA1099はFnumTableType::Noneを使い、
        // updateFreq()内で実機式を直接計算する。
    };
} // namespace fitom

// チップドライバが using 宣言なしに FnumTableType:: を使えるよう
// グローバルスコープにも型エイリアスを公開する
using FnumTableType = fitom::FnumTableType;

// FNUM_OFFSET: OPN/OPM 系のノートオフセット (標準: -576)
#ifndef FNUM_OFFSET
#  define FNUM_OFFSET (-576)
#endif
