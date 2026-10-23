// tests/test_voiceprocessor_lfo.cpp
// LfoControl(fitom/VoiceProcessor.h)のワンショットLFO関連の回帰テスト。
//
// OneShot系のLFOは、fadeout開始時点の位相で凍結し、デプス(fade_level_)
// だけを減衰させる。LfoControl::tick()のFadingOut分岐がmode_を見ずに
// phase_tick_を回し続けると(「波形が途切れないように」はRepeatモード
// 前提の設計意図)、Held中(一周期完了・末尾値保持中)や一周期完了前に
// fadeout()が呼ばれた場合、NoteOff〜kReleasingHoldMs(2000ms)の間ずっと
// 位相が周期的に回り続け、LFOが最初から繰り返し再生されているように
// 聞こえる。この誤りを検出する。
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
    // 「位相が回り続ける」誤りがあれば振幅が非単調になることを保証する)
    lfo.fadeout(20);

    int prevAbs = std::abs(static_cast<int>(heldRaw));
    for (int i = 0; i < 19; ++i) {
        lfo.tick();
        REQUIRE(lfo.phase() == LfoControl::Phase::FadingOut);
        const int curAbs = std::abs(static_cast<int>(lfo.wave(waveform)));
        // phase_tick_が周期で折り返すと振幅が増減を繰り返し
        // (=LFOが繰り返し再生されているように聞こえる)、この
        // 単調減少の検証に失敗する。
        REQUIRE(curAbs <= prevAbs);
        prevAbs = curAbs;
    }
}

TEST_CASE("LfoControl up-saw/down-saw start at zero and OneShotHold holds at max amplitude",
          "[voiceprocessor][lfo]")
{
    // up-saw(0)/down-sawはいきなり最大振幅から始まるのではなく、
    // t=0で変位0から立ち上がる/下がるようになっている。
    // 一方でOneShotHoldはt=0(=変位0)ではなく、各波形が向かう方向の
    // 最大振幅側で保持する仕様。
    {
        LfoControl lfo;
        lfo.start(/*rate=*/127, /*delay_20ms=*/0, /*fadein=*/0, LfoMode::OneShotHold);
        REQUIRE(lfo.wave(0) == 0); // up-saw: 開始直後は変位0
        REQUIRE(lfo.wave(4) == 0); // down-saw: 開始直後は変位0

        // ワンショットの1周期は「エンベロープ」であり、途中に不連続点が
        // あってはならない(あると波形が2回繰り返して聞こえる)。
        // 周期内で単調に±120へ向かうことを検証する。
        int prevUp = lfo.wave(0), prevDown = lfo.wave(4);
        for (int i = 0; i < 200 && lfo.phase() != LfoControl::Phase::Held; ++i) {
            lfo.tick();
            if (lfo.phase() != LfoControl::Phase::Running) break;
            const int curUp = lfo.wave(0), curDown = lfo.wave(4);
            REQUIRE(curUp   >= prevUp);   // up-saw:   単調増加
            REQUIRE(curDown <= prevDown); // down-saw: 単調減少
            prevUp = curUp; prevDown = curDown;
        }

        for (int i = 0; i < 200 && lfo.phase() != LfoControl::Phase::Held; ++i) {
            lfo.tick();
        }
        REQUIRE(lfo.phase() == LfoControl::Phase::Held);
        REQUIRE(lfo.wave(0) == 120);  // up-saw: 最大振幅(+側)で保持
        REQUIRE(lfo.wave(4) == -120); // down-saw: 最大振幅(-側)で保持
        // ホールド値は周期末尾の値と連続していること(跳ね上がらない)
        REQUIRE(prevUp   >= 100);
        REQUIRE(prevDown <= -100);
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
    // Repeatモードはフェードアウト中も波形が回り続ける
    // (ビブラートが不自然に途切れないようにする設計意図)ことを
    // 確認する。OneShot系の位相凍結がRepeatに波及していないことの
    // 回帰テスト。
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

// CC#76-78(ソフトウェアLFO Rate/Depth/Delay上書き)は、GM2規格の
// Sound Controllerと同じ「値64=中央(補正なし)」というオフセット方式で
// 音色データの値に作用する(VoiceProcessor::recalcChLfo()参照)。
// GM2準拠のDAW/MIDIファイルはSound Controller群(CC#70-79)を既定値64へ
// リセットする定型初期化をしばしば送る。受信値で音色データの値を絶対上書き
// すると、CC#76=64を「Rateを64に強制設定」と解釈し、sw.LFR=0(LFO未使用)の
// 音色にまでLFOが起動してしまう。ここではDepth(CC#77)のオフセット方式を
// 検証する(Rate/DelayはCInstCh::noteOn()内でSwPatchに焼き込む方式のため
// ここでは検証できない)。
TEST_CASE("VoiceProcessor::setLfoDepthOverride() applies as a GM2-style offset "
          "from the patch's own depthCents (0 = no change, matching CC value 64)",
          "[voiceprocessor][lfo]")
{
    VoiceProcessor proc;
    FmVoice voice{};
    voice.sw.LFR = 20;          // 音色固有LFO有効
    voice.sw.LWF = 1;           // square(t=0から最大振幅+120)
    voice.sw.LFM = 0;           // Repeat
    voice.sw.depthCents = 600;  // 音色本来のデプス

    proc.onNoteOn(/*vol=*/127, /*exp=*/127, /*vel=*/100, voice, /*carrierMask=*/0);
    REQUIRE(proc.channelLfoActive());

    proc.onTick(voice); // recalcChLfo()を1回走らせる
    const int16_t baseline = proc.channelLfoValue();
    REQUIRE(baseline != 0);

    // CC#77=64(GM2のSound Controllerにおける「中央値」)相当のオフセット0を
    // 送っても、音色本来のデプスから変化しないこと(絶対上書き方式だと、
    // これが「デプスを0[無効化]に上書き」になる)。
    proc.setLfoDepthOverride(0);
    proc.onTick(voice);
    CHECK(proc.channelLfoValue() == baseline);

    // 正方向のオフセットで音色本来のデプスより深くなること。
    proc.setLfoDepthOverride(200);
    proc.onTick(voice);
    CHECK(proc.channelLfoValue() > baseline);

    // 音色本来のデプスを打ち消すほどの負方向オフセットで、符号が反転
    // すること(±1200セントの範囲でclampされる)。
    proc.setLfoDepthOverride(-1200);
    proc.onTick(voice);
    CHECK(proc.channelLfoValue() < 0);
}
