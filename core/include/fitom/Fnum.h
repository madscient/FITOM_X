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

// FNUM_OFFSET: OPN/OPL/OPLL/PSG系(CSoundDevice::getFnumber()の標準実装を
// 使うチップ全般)の基準ノートオフセット。
//
// CSoundDevice::getFnumber()のblock(オクターブ)は、fnumTable_生成時に
// 焼き込まれるこのオフセットとは独立に、MIDIノート番号(lastNote)のみから
// floor(lastNote/12)相当で決まる(SoundDevImpl.cpp参照)。MIDIノートは
// 0-127(約10.6オクターブ)ある一方、block格納先は3bit(0-7の8オクターブ)
// までしか無いため、高音域(概ねnote>=96)でblockが8を超え、getFnumber()の
// oct>7クランプ処理(block=7に固定しfnumを左シフトで補う)がfnumを11bit
// (2047)超に押し上げてしまう「Fnumオーバーフロー」が発生していた。
// 標準値を-576(-9半音)から-1344(-576-768、1オクターブ分=768を追加で
// 引く)へ変更し、基準ノートを1オクターブ下げることで、同じ絶対音高に
// 対するblockを一律1つ減らし、オーバーフロー発生開始点をnote96→108へと
// 1オクターブぶん高音側へ押し上げている(2026年8月)。
#ifndef FNUM_OFFSET
#  define FNUM_OFFSET (-1344)
#endif
