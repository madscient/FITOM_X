// tests/test_voiceprocessor_lfo.cpp
// LfoControl(fitom/VoiceProcessor.h)のワンショットLFO関連の回帰テスト。
//
// ユーザー報告: ソフトウェアピッチLFOにOneShotHoldを設定していても、
// NoteOff時(fadeout()呼び出し)にLFOが最初から繰り返し再生されている
// ように聞こえる。調査の結果、LfoControl::tick()のFadingOut分岐が
// mode_を見ずに無条件でphase_tick_を回し続けており(コメント「波形が
// 途切れないように」はRepeatモード前提の設計意図)、Held中(一周期完了・
// 末尾値保持中)や一周期完了前にfadeout()が呼ばれた場合、NoteOff〜
// kReleasingHoldMs(2000ms)の間ずっと位相が周期的に回り続けてしまう
// バグがあった(2026年8月修正)。OneShot系はfadeout開始時点の位相で
// 凍結し、デプス(fade_level_)だけを減衰させるのが正しい。
// このテストはchLfo_/opLfo_双方が共有するLfoControl本体を直接操作し、
// フェードアウト中の波形出力の絶対値が単調減少すること(=位相が再び
// 回り始めていないこと)を検証する。

#include <catch2/catch_test_macros.hpp>
#include "fitom/VoiceProcessor.h"
#include <cstdlib>

using namespace fitom;

TEST_CASE("LfoControl OneShotHold does not resume cycling during NoteOff fadeout",
          "[voiceprocessor][lfo]")
{
    LfoControl lfo;
    // rate=127(最速、約20ms=20tick周期)で短時間にHeldへ到達させる
    lfo.start(/*rate=*/127, /*delay_20ms=*/0, /*fadein=*/0, LfoMode::OneShotHold);

    for (int i = 0; i < 200 && lfo.phase() != LfoControl::Phase::Held; ++i) {
        lfo.tick();
    }
    REQUIRE(lfo.phase() == LfoControl::Phase::Held);

    const uint8_t waveform = 1; // square(t=0で最大振幅+120)
    const int8_t heldRaw = lfo.wave(waveform);
    REQUIRE(heldRaw != 0);

    // NoteOff相当: フェードアウト開始(周期(約20tick)をまたぐ長さにして、
    // 修正前の「位相が回り続ける」挙動が再現すれば振幅が非単調になる
    // ことを保証する)
    lfo.fadeout(20);

    int prevAbs = std::abs(static_cast<int>(heldRaw));
    for (int i = 0; i < 19; ++i) {
        lfo.tick();
        REQUIRE(lfo.phase() == LfoControl::Phase::FadingOut);
        const int curAbs = std::abs(static_cast<int>(lfo.wave(waveform)));
        // 修正前はphase_tick_が周期で折り返すため振幅が増減を繰り返し
        // (=LFOが繰り返し再生されているように聞こえるバグ)、この
        // 単調減少の検証に失敗していた。
        REQUIRE(curAbs <= prevAbs);
        prevAbs = curAbs;
    }
}

TEST_CASE("LfoControl up-saw/down-saw start at zero and OneShotHold holds at max amplitude",
          "[voiceprocessor][lfo]")
{
    // up-saw(0)/down-sawはいきなり最大振幅から始まるのではなく、
    // t=0で変位0から立ち上がる/下がるように位相シフトされている。
    // 一方でOneShotHoldはt=0(=変位0)ではなく、各波形が向かう方向の
    // 最大振幅側で保持する仕様(2026年8月)。
    {
        LfoControl lfo;
        lfo.start(/*rate=*/127, /*delay_20ms=*/0, /*fadein=*/0, LfoMode::OneShotHold);
        REQUIRE(lfo.wave(0) == 0); // up-saw: 開始直後は変位0
        REQUIRE(lfo.wave(4) == 0); // down-saw: 開始直後は変位0

        for (int i = 0; i < 200 && lfo.phase() != LfoControl::Phase::Held; ++i) {
            lfo.tick();
        }
        REQUIRE(lfo.phase() == LfoControl::Phase::Held);
        REQUIRE(lfo.wave(0) == 120);  // up-saw: 最大振幅(+側)で保持
        REQUIRE(lfo.wave(4) == -120); // down-saw: 最大振幅(-側)で保持
    }

    // Repeatモードでは開始直後にt=0で変位0から立ち上がり、直後のtickで
    // 変位が生じることを確認する(いきなり最大振幅から始まらないこと)
    {
        LfoControl lfo;
        lfo.start(/*rate=*/127, /*delay_20ms=*/0, /*fadein=*/0, LfoMode::Repeat);
        REQUIRE(lfo.wave(0) == 0);
        REQUIRE(lfo.wave(4) == 0);
        lfo.tick();
        REQUIRE(lfo.wave(0) > 0);  // up-saw: 正方向へ立ち上がる
        REQUIRE(lfo.wave(4) < 0);  // down-saw: 負方向へ立ち下がる
    }
}

TEST_CASE("LfoControl Repeat mode keeps cycling during fadeout as before",
          "[voiceprocessor][lfo]")
{
    // Repeatモードは従来通りフェードアウト中も波形が回り続ける
    // (ビブラートが不自然に途切れないようにする既存の設計意図)ことを
    // 確認する。今回の修正がOneShot系にのみ影響し、Repeatの挙動を
    // 変えていないことの回帰テスト。
    LfoControl lfo;
    lfo.start(/*rate=*/127, /*delay_20ms=*/0, /*fadein=*/0, LfoMode::Repeat);

    lfo.fadeout(40); // 周期(約20tick)を2周またぐ長さ

    bool sawRise = false;
    int prevAbs = std::abs(static_cast<int>(lfo.wave(4)));
    for (int i = 0; i < 39; ++i) {
        lfo.tick();
        const int curAbs = std::abs(static_cast<int>(lfo.wave(4)));
        if (curAbs > prevAbs) sawRise = true;
        prevAbs = curAbs;
    }
    REQUIRE(sawRise); // 位相が周期を回っていれば振幅の増加が観測されるはず
}
