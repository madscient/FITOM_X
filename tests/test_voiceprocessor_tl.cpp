// tests/test_voiceprocessor_tl.cpp
// VoiceProcessor::recalcBaseTL() の vol / vel・exp(VTL) 適用範囲の回帰テスト。
//
// 設計上の分岐点:
//   - VTL (vel/exp 感度) は全opに適用する。モジュレータのTLはFMの変調指数
//     そのものであり、ここをvel/expで動かすことがFM音源のベロシティ表現
//     (強く弾くほど明るい音)の本体だから。
//   - vol (マスターボリューム) はキャリアのみに適用する。モジュレータにも
//     掛けると、ボリュームを絞っただけで変調指数が下がって音色そのものが
//     変わってしまうため。
//   - baseTL/effectiveTL はキャリア・モジュレータを問わずラウドネス空間
//     (0=無音, 127=最大音量) で保持する。チップドライバのeffTLToReg()が
//     全opに対してこの規約を前提に減衰量空間へ逆変換するため。
//
// テスト名を英語にしているのは、日本語のテスト名だとctestがCatch2へ渡す
// フィルタ文字列がコンソールコードページで文字化けし、テストが1件も
// マッチしないまま失敗扱いになるため。

#include <catch2/catch_test_macros.hpp>
#include "fitom/VoiceProcessor.h"
#include "fitom/VolumeUtils.h"

using namespace fitom;

namespace {

// op3 のみキャリア (OPN ALG=4 相当の単純な2系統ではなく、
// キャリア/モジュレータの区別だけを見るための最小構成)
constexpr uint8_t kCarrierMask = 1u << 3;
constexpr int kModOp     = 0;
constexpr int kCarrierOp = 3;

FmVoice makeVoice(uint8_t tl, uint8_t vtl)
{
    FmVoice voice{};
    for (int op = 0; op < 4; ++op) {
        voice.hwOp[op].TL  = tl;
        voice.swOp[op].VTL = vtl;
        voice.swOp[op].SLR = 0;   // オペレータLFO停止 → effectiveTL == baseTL
    }
    return voice;
}

// onNoteOn 直後の effectiveTL(op) を取る
uint8_t noteOnTL(const FmVoice& voice, uint8_t vol, uint8_t exp, uint8_t vel, int op)
{
    VoiceProcessor proc;
    proc.reset();
    proc.onNoteOn(vol, exp, vel, voice, kCarrierMask);
    return proc.effectiveTL(op);
}

} // namespace

TEST_CASE("VTL applies to modulator operators, not only to carriers",
          "[voiceprocessor][tl]")
{
    const FmVoice voice = makeVoice(/*tl=*/20, /*vtl=*/127);

    const uint8_t modFull = noteOnTL(voice, 127, 127, 127, kModOp);
    const uint8_t modSoft = noteOnTL(voice, 127, 127,  32, kModOp);
    CHECK(modSoft < modFull);   // ラウドネス空間なので低ベロシティ = 小さい値

    const uint8_t carFull = noteOnTL(voice, 127, 127, 127, kCarrierOp);
    const uint8_t carSoft = noteOnTL(voice, 127, 127,  32, kCarrierOp);
    CHECK(carSoft < carFull);

    // exp も vel と同じくVTLゲート経由でモジュレータに効く
    const uint8_t modLowExp = noteOnTL(voice, 127, 32, 127, kModOp);
    CHECK(modLowExp < modFull);
    CHECK(modLowExp == modSoft);  // vel/exp は低い方(min)を採る対称な設計
}

TEST_CASE("VTL=0 leaves an operator completely insensitive to vel/exp",
          "[voiceprocessor][tl]")
{
    const FmVoice voice = makeVoice(/*tl=*/20, /*vtl=*/0);

    CHECK(noteOnTL(voice, 127, 127, 1, kModOp)
          == noteOnTL(voice, 127, 127, 127, kModOp));
    CHECK(noteOnTL(voice, 127, 1, 127, kCarrierOp)
          == noteOnTL(voice, 127, 127, 127, kCarrierOp));
}

TEST_CASE("Volume attenuates carriers only, never modulators",
          "[voiceprocessor][tl]")
{
    const FmVoice voice = makeVoice(/*tl=*/20, /*vtl=*/127);

    const uint8_t modFullVol = noteOnTL(voice, 127, 127, 127, kModOp);
    const uint8_t modHalfVol = noteOnTL(voice,  64, 127, 127, kModOp);
    CHECK(modHalfVol == modFullVol);

    const uint8_t carFullVol = noteOnTL(voice, 127, 127, 127, kCarrierOp);
    const uint8_t carHalfVol = noteOnTL(voice,  64, 127, 127, kCarrierOp);
    CHECK(carHalfVol < carFullVol);
}

TEST_CASE("baseTL stays in loudness space for modulators as well as carriers",
          "[voiceprocessor][tl]")
{
    // TL=0 (減衰なし) + vol/exp/vel すべて最大 → 全opが最大音量側の127
    const FmVoice loud = makeVoice(/*tl=*/0, /*vtl=*/127);
    CHECK(noteOnTL(loud, 127, 127, 127, kModOp)     == 127);
    CHECK(noteOnTL(loud, 127, 127, 127, kCarrierOp) == 127);

    // 減衰のあるTLでは、モジュレータはvolを掛けない純粋な空間変換
    // (kGM2dB[127]=0dB) の結果と一致する
    const FmVoice quiet = makeVoice(/*tl=*/40, /*vtl=*/0);
    CHECK(noteOnTL(quiet, 64, 127, 127, kModOp)
          == fitom::calcLinearLevel(127, 40));
}
