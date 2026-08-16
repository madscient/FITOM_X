// tests/test_silence_bank.cpp
// 無音バンク (VOICE_PATCH_SILENCE / CC#0=127) のユニットテスト (Catch2 v3)

#include <catch2/catch_test_macros.hpp>
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include "fitom/FITOMdefine.h"

using namespace fitom;

// 「解決に成功した上で発音しない」という結果が、「解決に失敗した」
// 結果と区別できていることがこの機能の核心。前者はProgChange時に直前の
// パッチを破棄して無音になり、後者は直前のパッチを維持する
// (CInstCh::progChange参照)。
TEST_CASE("Silence bank resolves successfully with no layers", "[patch][silence]")
{
    FITOMConfig cfg;          // デバイス0台。無音バンクはconfigを参照しない
    PatchManager pm;
    Patch storage;

    SECTION("直接モード: バンク/プログラムの値によらず無音として成立する")
    {
        for (uint8_t bank : {uint8_t{0}, uint8_t{5}, uint8_t{127}}) {
            for (uint8_t prog : {uint8_t{0}, uint8_t{64}, uint8_t{127}}) {
                auto r = pm.resolveDirect(VOICE_PATCH_SILENCE, bank, prog, cfg, storage);
                CHECK(r.isValid());
                CHECK(r.layerCount == 0);
            }
        }
    }

    SECTION("未登録のチップ種別は解決失敗のまま(無音バンクと混同しない)")
    {
        auto r = pm.resolveDirect(VOICE_PATCH_OPM, 0, 0, cfg, storage);
        CHECK_FALSE(r.isValid());
    }
}

// レイヤードパッチのプレースホルダ用途。無音レイヤーは黙って捨てられ、
// 同じPatch内の他のレイヤーには影響しない。
TEST_CASE("Silence layers are dropped without invalidating the patch", "[patch][silence]")
{
    FITOMConfig cfg;
    PatchManager pm;

    Patch p;
    p.id = 0;
    for (auto& l : p.layers) {
        l.voicePatchType = VOICE_PATCH_SILENCE;
        l.enabled        = true;
    }

    auto r = pm.resolve(p, cfg);
    CHECK(r.isValid());          // Patch自体は見つかっている
    CHECK(r.layerCount == 0);    // ただし発音するレイヤーは無い
}
