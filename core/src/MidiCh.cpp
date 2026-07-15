// fitom/MidiCh.cpp
// CInstCh / CRhythmCh 実装

#include "fitom/MidiCh.h"
#include "fitom/CFITOM.h"
#include "fitom/Log.h"
#include <algorithm>
#include <cstring>

namespace fitom {

// ================================================================
//  PortaCtrl
// ================================================================

namespace {
// Portamento Rate テーブル (128要素)。GM2規格書の「Portamento Rate」
// グラフ (Y軸: Pitch increment speed [cent/msec] 対数スケール、
// X軸: cc#5 0-127) から、区分指数関数で再構築した値。
// 旧FITOM(ROM::portspeed[])と同じ符号エンコード方式を踏襲する:
//   delta>0: 1tickに delta ステップ(1ステップ=1kfs=100/64cent)進む
//   delta<0: -delta ティックに1ステップ進む
// tick間隔は1ms(CFITOM::startTimerThreadの既定値)を前提とする。
// GM2グラフとの照合により全域で概ね良好な一致を確認済み。
// cc5=64-88付近(1ティックあたり±1-2ステップしか取れない領域)でのみ
// 最大約27%の量子化誤差があるが、聴感上の影響は小さいと判断し許容する。
constexpr int16_t kPortaSpeedTable[128] = {
    640,526,433,356,293,241,198,163,134,110,91,74,61,50,41,34,28,23,
    19,16,13,12,11,11,10,10,9,9,8,8,7,7,6,6,6,6,
    5,5,5,5,4,4,4,4,4,3,3,3,3,3,3,3,2,2,
    2,2,2,2,2,2,2,2,2,1,1,1,1,1,1,1,1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-2,-2,-2,-2,-2,-2,-2,-2,-2,-3,
    -3,-3,-3,-3,-3,-4,-4,-4,-4,-5,-5,-5,-6,-6,-6,-7,-7,-7,
    -8,-8,-9,-9,-10,-10,-11,-12,-12,-13,-14,-15,-16,-26,-43,-70,-116,-191,
    -316,-521
};
} // namespace

void PortaCtrl::update()
{
    if (state_ != State::Running || !enabled_) return;

    ++count_;
    int16_t delta = kPortaSpeedTable[speed_];

    // 低速域(delta<0): -deltaティックに1回だけ、条件を満たした回のみ
    // 1ステップ進める。それ以外のtickでは何もしない。
    if (delta < 0) {
        if ((count_ % static_cast<uint32_t>(-delta)) != 0) return;
        delta = 1;
    }

    // absnote/target は「1半音=64ステップ(100/64cent/ステップ)」単位。
    // current_(半音)とfine_(0-63、半音未満の端数)を合成した絶対位置。
    int16_t absnote = (static_cast<int16_t>(current_) << 6) | fine_;
    int16_t target  = static_cast<int16_t>(end_) << 6;
    int16_t remain  = target - absnote;

    if (remain == 0) {
        state_ = State::Idle;
        count_ = 0;
        return;
    }
    if (remain < 0) {
        absnote = (delta < -remain) ? (absnote - delta) : target;
    } else {
        absnote = (delta < remain) ? (absnote + delta) : target;
    }
    current_ = static_cast<uint8_t>(absnote >> 6);
    fine_    = static_cast<uint8_t>(absnote & 0x3F);
}

// ================================================================
//  CInstCh
// ================================================================

CInstCh::CInstCh(uint8_t ch, CFITOM* parent)
    : ch_(ch), fitom_(parent)
{
    notes_.fill(NoteHist{});
}

CInstCh::~CInstCh() { allNoteOff(); }

void CInstCh::setup(PatchManager* pm, CFITOM* fitom)
{
    patchMgr_ = pm;
    fitom_    = fitom;
    // poly_ はデフォルト値のまま。progChange() 実行時に、解決された
    // パッチが使うデバイスのチャンネル数から動的に決定される。
}

// ----------------------------------------------------------------
//  ProgChange
//
//  CC#0(MSB)がモードを決める:
//    0        : 通常モード。CC#32(LSB)がPatchBank番号を選択する。
//    0x01-0x6F: 直接モード。MSB自体がVoicePatchType、CC#32(LSB)が
//               そのVoicePatchType用にプロファイル登録された
//               HwBankのインデックスを選択する。
//    0x78/0x79: MidiProcessor層で消費済み (リズム/メロディ切替)。
//               このメソッドに渡るbankSelM_には決して現れない。
// ----------------------------------------------------------------
void CInstCh::progChange(uint8_t prog)
{
    programNo_ = prog;
    // NRPN 96,1による物理チャンネル固定は、プログラムチェンジ受信で
    // 都度解除される(applyPhyChOverride参照)。
    phyCh_ = 127;
    // NRPN 96,2/96,3によるパフォーマンスバンク/プログラムの上書きも、
    // プログラムチェンジ受信で解除される(仕様上、次のプログラム
    // チェンジまで有効)。
    pendingSwBankOverride_ = -1;
    swBankOverride_ = -1;
    swProgOverride_ = -1;
    clearHwPatchOverrides();
    if (!patchMgr_ || !fitom_) return;

    ResolvedPatch resolved;
    if (bankSelM_ == 0) {
        // 通常モード: LSBがPatchBank番号
        resolved = patchMgr_->resolve(bankSelL_, prog, fitom_->getConfig());
    } else {
        // 直接モード: MSB=VoicePatchType, LSB=HwBankインデックス
        resolved = patchMgr_->resolveDirect(bankSelM_, bankSelL_, prog,
                                             fitom_->getConfig(), directPatch_);
    }

    if (!resolved.isValid()) {
        // 無効なバンク/プログラムの場合は resolver_ を上書きしない。
        // 直前まで有効だったパッチをそのまま保持し、チャンネルが
        // 無音状態に陥ることを防ぐ。
        FITOM_LOG_WARN("CInstCh ch=" << static_cast<int>(ch_)
            << ": ProgChange " << static_cast<int>(prog)
            << " (bank=" << static_cast<int>(bankSelM_)
            << " lsb=" << static_cast<int>(bankSelL_) << ") — no patch found"
            << " (keeping previous patch)");
        return;
    }

    resolver_.apply(resolved);

    // ポリフォニー数はデバイス依存: このパッチが使う全レイヤーのうち、
    // 最小のチャンネル数を持つデバイスが上限になる (どのレイヤーも
    // 同時に鳴らす必要があるため、一番小さいデバイスがボトルネックになる)。
    // channel_map等の固定設定は使わず、ProgChangeのたびに動的に決定する。
    uint8_t newPoly = MAX_NOTES;
    for (int li = 0; li < resolved.layerCount; ++li) {
        const auto& rl = resolved.layers[li];
        ISoundDevice* dev = fitom_->getDevice(rl.deviceIndex);
        if (!dev) continue;
        uint8_t chCount = dev->getChCount();
        if (chCount > 0 && chCount < newPoly) newPoly = chCount;
    }
    poly_ = (newPoly > 0) ? std::min<uint8_t>(newPoly, MAX_NOTES) : 1;
    // CC#126で明示的にボイス数上限を指定していない限り、パッチの
    // 自動算出値に追従させる。
    if (!voiceLimitOverride_) voiceLimit_ = poly_;

    FITOM_LOG_DEBUG("CInstCh ch=" << static_cast<int>(ch_)
        << ": ProgChange " << static_cast<int>(prog)
        << " '" << resolved.patch->name << "'"
        << " layers=" << resolved.layerCount
        << " poly=" << static_cast<int>(poly_));

    // Program Change は発音中のノートに一切作用しない (全モード共通の仕様)。
    // 新しいパッチは次の NoteOn から適用される。
}

// ----------------------------------------------------------------
//  DVA (発音時の動的チャンネル割り当て) 中のフォールバック・ハンドオフ
//
//  一次候補デバイス(resolve()で決定済み)に空きチャンネルが無い場合、
//  そのデバイスで強制スティールする前に、フォールバック受け入れ可能な
//  他のデバイス(例: OPN満杯時のOPN2)に空きがあればそちらへハンドオフする。
//  Program Change時のフォールバック(voicePatchTypeに一致するデバイスが
//  1台も接続されていない場合の代替先選択)とは異なる、独立した仕組み。
//
//  優先順位: ①一次候補デバイスの空き/Releasing ②フォールバック候補デバイス
//  群の空き/Releasing(devices[]順) ③一次候補デバイスでの強制スティール
//  (最終手段、フォールバック候補が全て埋まっている場合のみ)。
//
//  AWM等HwPatchを持たないVoicePatchType(patch==nullptr)はフォールバック
//  非対応のため、一次候補デバイスでの通常のallocCh(スティール込み)のみ
//  行う。
// ----------------------------------------------------------------
namespace {
std::pair<ISoundDevice*, uint8_t> allocChWithFallback(
    CFITOM* fitom, ISoundDevice* dev, const ResolvedLayer* rl,
    IMidiCh* owner, const HwPatch* patch, uint8_t vel, const SwPatch* swPatch,
    const SampleZonePatch* samplePatch)
{
    // ① 一次候補デバイスに、スティール無しで空きがあるか確認
    if (patch) {
        uint8_t ch = dev->queryCh(owner, patch, /*mode=*/1);
        if (ch != 0xFF) {
            dev->assignCh(ch, owner, patch, vel, swPatch, samplePatch);
            return {dev, ch};
        }
    } else {
        // AWM等: フォールバック非対応、通常のallocCh(スティール込み)のみ
        uint8_t ch = dev->allocCh(owner, patch, vel, swPatch, samplePatch);
        return {dev, ch};
    }

    // ② 一次候補デバイスが満杯 → フォールバック候補デバイスを探す
    auto candidates = fitom->getConfig().findAllFallbackDeviceIndices(
        rl->layer->voicePatchType, *patch);
    for (int idx : candidates) {
        if (idx == rl->deviceIndex) continue; // 自分自身(一次候補)は除外済み
        ISoundDevice* candDev = fitom->getDevice(idx);
        if (!candDev) continue;
        uint8_t ch = candDev->queryCh(owner, patch, /*mode=*/1);
        if (ch != 0xFF) {
            candDev->assignCh(ch, owner, patch, vel, swPatch, samplePatch);
            FITOM_LOG_INFO("DVA fallback: device[" << rl->deviceIndex
                << "] busy, handed off to device[" << idx << "]");
            return {candDev, ch};
        }
    }

    // ③ どの候補も空きが無い → 一次候補デバイスで強制スティール(最終手段)
    uint8_t ch = dev->allocCh(owner, patch, vel, swPatch, samplePatch);
    return {dev, ch};
}
} // namespace

// ----------------------------------------------------------------
//  NoteOn
// ----------------------------------------------------------------
void CInstCh::noteOn(uint8_t note, uint8_t vel)
{
    if (resolver_.layerCount() == 0) return;

    // ボイス数上限によるスティール(モノフォニックモード時は、既存の
    // 同一レイヤー内スティール(下のmono_分岐)で対応するため対象外)。
    if (!mono_) stealOldestNoteIfNeeded();

    // このNoteOnイベントで生成される全レイヤーのエントリに共通の
    // 発音順シーケンス番号(ボイス数上限スティールでの「最も古いノート」
    // 判定に使う)。
    const uint32_t thisSeq = ++noteSeq_;

    for (int li = 0; li < resolver_.layerCount(); ++li) {
        const auto* rl = resolver_.layer(li);
        if (!rl || !rl->layer || !rl->layer->isActive()) continue;
        if (!rl->layer->inRange(note)) continue;

        int transposed = rl->layer->transposedNote(note);
        // SwPatch.fineTranspose(HwPatch由来の演奏特性の一部、
        // セント単位・±1200)を加算する。ToneLayer.transpose(半音単位、
        // ネイティブパッチのレイヤー固有パラメータ)とは独立した概念で、
        // 両方指定されている場合は加算する。
        // セント値は「半音部分(ノート番号に加算)」と「セント端数部分
        // (下でfineに合成する)」に分解する。整数除算/剰余は0方向への
        // 切り捨てのため、負値でも正しく分解できる
        // (例: -150セント → 半音-1 + 端数-50セント)。
        int16_t swTransposeFineCents = 0;
        if (rl->swPatch && rl->swPatch->fineTranspose != 0) {
            int cents = rl->swPatch->fineTranspose;
            transposed += cents / 100;
            swTransposeFineCents = static_cast<int16_t>(cents % 100);
        }
        if (transposed < 0 || transposed > 127) continue;

        ISoundDevice* dev = fitom_->getDevice(rl->deviceIndex);
        if (!dev) continue;

        // SysEx(target-type=0x00)によるHwPatchパラメータオーバーライドが
        // このレイヤーで有効なら、rl->hwPatch(音色本来のパラメータ)の
        // 代わりに使う。サンプルベース音源系(samplePatch経由)は対象外
        // (HwPatchを使わない別スキーマのため)。
        const HwPatch* patch = (hwPatchOverrideActive_[li] && rl->hwPatch)
            ? &hwPatchOverride_[li] : rl->hwPatch;
        // サンプルベース音源系 (VOICE_PATCH_AWM等) の場合のみ非nullptr。
        // patchとは排他 (PatchManager::resolve()が保証する)。
        const SampleZonePatch* samplePatch = rl->samplePatch;

        // NRPN 96,2/96,3(パフォーマンスバンク/プログラムセレクト)が
        // 有効なら、HwPatch本来の参照先(rl->swPatch)の代わりにこちらを
        // 基点にする。解決に失敗した場合(バンク/プログラムが存在しない
        // 等)は、無指定時と同じ扱いとしてrl->swPatchにソフトフォール
        // バックする(DrumNote::swBank/swProgと同じ規約)。
        const SwPatch* baseSwPatch = rl->swPatch;
        if (swBankOverride_ >= 0 && patchMgr_) {
            const SwPatch* overridden = patchMgr_->resolveSwPatch(swBankOverride_, swProgOverride_);
            if (overridden) baseSwPatch = overridden;
        }

        // CC#76/78(ソフトウェアLFO Rate/Delay)の演奏時上書きがあれば、
        // (上記で決まった)基点となるSwPatchの一時コピーへ焼き込んで
        // からassignCh/allocChへ渡す。VoiceProcessor::onNoteOn()は
        // assignCh()の内部で呼ばれるため、この時点で焼き込んでおく
        // 必要がある(詳細はlfoRateOverride_のコメント参照)。元の
        // SwPatch(共有パッチデータ)は直接書き換えない。
        SwPatch overriddenSw{};
        const SwPatch* effSwPatch = baseSwPatch;
        if (lfoRateOverride_ >= 0 || lfoDelayOverride_ >= 0) {
            if (baseSwPatch) overriddenSw = *baseSwPatch;
            if (lfoRateOverride_ >= 0)
                overriddenSw.sw.LFR = static_cast<uint8_t>(lfoRateOverride_);
            if (lfoDelayOverride_ >= 0)
                overriddenSw.sw.LFD = static_cast<uint8_t>(lfoDelayOverride_);
            effSwPatch = &overriddenSw;
        }

        // ────────────────────────────────────────────────────────
        // ハードチャンネル割り当て
        // ────────────────────────────────────────────────────────
        // ポルタメントのグライド元候補 (モノ時、同一レイヤーで直前に
        // 鳴っていたノート番号)。0xFF = 該当なし (このレイヤー初のノート)。
        uint8_t prevNote = 0xFF;

        uint8_t devCh = 0xFF;
        if (rl->forcedCh >= 0) {
            // ハードウェア制約による強制チャンネル(ビルトインリズム等)。
            // 同一レイヤーでは常に同じ値(progChange時に解決・固定済み)
            // のため、mono_/legatoの「前ノートのチャンネルを奪う」処理は
            // 不要 — 常にforcedChへ再アサインするだけでよい。phyCh_
            // (NRPNによるユーザー明示指定)より優先する(理由はCRhythmCh
            // 側の同種コメント参照)。
            devCh = dev->assignCh(static_cast<uint8_t>(rl->forcedCh), this, patch,
                                   vel, effSwPatch, samplePatch);
        } else if (phyCh_ != 127 && phyCh_ < dev->getChCount()) {
            devCh = dev->assignCh(phyCh_, this, patch, vel, effSwPatch, samplePatch);
        } else if (mono_ && timbres_ > 0) {
            // モノ: 同一レイヤーの最初のノートを奪う
            for (int hi = 0; hi < MAX_NOTES; ++hi) {
                if (notes_[hi].isValid() && notes_[hi].layerIdx == li) {
                    devCh = notes_[hi].devCh;
                    prevNote = notes_[hi].note;   // ポルタメント用に記録
                    if (!legato_) dev->noteOff(devCh);
                    leaveNote(hi);
                    break;
                }
            }
            if (devCh == 0xFF) {
                auto [handoffDev, handoffCh] = allocChWithFallback(
                    fitom_, dev, rl, this, patch, vel, effSwPatch, samplePatch);
                dev = handoffDev;
                devCh = handoffCh;
            }
        } else {
            auto [handoffDev, handoffCh] = allocChWithFallback(
                fitom_, dev, rl, this, patch, vel, effSwPatch, samplePatch);
            dev = handoffDev;
            devCh = handoffCh;
        }

        if (devCh == 0xFF) {
            FITOM_LOG_DEBUG("CInstCh ch=" << (int)ch_
                << " layer=" << li << ": no device ch available");
            continue;
        }

        // ────────────────────────────────────────────────────────
        // コントローラ状態を適用
        // ────────────────────────────────────────────────────────

        // ボリューム・エクスプレッション (レイヤーのオフセット加味)
        int adjVol = static_cast<int>(volume_) + rl->layer->volumeOffset;
        adjVol = std::clamp(adjVol, 0, 127);
        dev->setVolume(devCh, static_cast<uint8_t>(adjVol), false);
        dev->setExpression(devCh, expression_, false);
        dev->setSustain(devCh, sustain_, false);

        // パン (レイヤーオフセット加味, -64..+63 に正規化)
        int adjPan = static_cast<int>(panpot_) - 64 + rl->layer->panOffset;
        adjPan = std::clamp(adjPan, -64, 63);
        dev->setPanpot(devCh, static_cast<int8_t>(adjPan), false);

        // HW LFO(CC#14/#15由来のhwLfoDepth_/hwLfoRate_)。有効/無効自体は
        // CCではなく、このノートが今まさに使うボイス自身のAMS/PMS値で
        // 決まる(デバイス側のenablePM/enableAM実装がAMS/PMS=0なら実質
        // 無効果になる)。CC#1(pmDepth_)はここでは一切参照しない
        // (2026年7月、ソフトウェアLFOと完全に分離した)。
        dev->enablePM(devCh, true);
        dev->enableAM(devCh, true);
        dev->setLFODepth(devCh, hwLfoDepth_);
        dev->setLFORate(devCh, hwLfoRate_);

        // ソフトウェアLFO Depth(CC#77)。このデバイスチャンネルを直前に
        // 使っていた別のMIDIチャンネル/ノートのオーバーライド値が
        // 残っていないよう、上書きが無効(-2000)の場合も含め毎ノート
        // 必ず送る。
        dev->setLfoDepthOverride(devCh, lfoDepthOverrideCents_);

        // ────────────────────────────────────────────────────────
        // ポルタメント (モノフォニックチャンネル専用)
        // ────────────────────────────────────────────────────────
        // ポリフォニックチャンネルでは常に無効。ポルタメント状態は
        // CInstCh に1つしかなく、複数ノート (和音) に同じグライドを
        // 適用すると和音が崩れるため、mono_ == true のときのみ動作する。
        const bool portaActive = mono_ && portamento_.isEnabled();

        if (portaActive) {
            if (portaSourceNote_ != 0xFF) {
                // CC#84 で明示指定されたノートからグライド開始 (one-shot)
                portamento_.setSource(portaSourceNote_);
                portaSourceNote_ = 0xFF;
            } else if (prevNote == 0xFF) {
                // このチャンネルで最初のノート: グライド元がないので
                // 現在地 (=目標そのもの) から開始し、実質グライドなしにする。
                portamento_.setSource(static_cast<uint8_t>(transposed));
            }
            // それ以外 (prevNote が有効): setSource() を呼ばない。
            // PortaCtrl::current_ は前回のグライドの実際の到達点を
            // 保持しているため、そこから連続的に新しい目標へ滑らせる。
            portamento_.start(static_cast<uint8_t>(transposed));
        } else {
            // ポリフォニック、またはポルタメント無効:
            // 進行中のグライド状態が残っていても使わない。
            portaSourceNote_ = 0xFF; // one-shot は消費扱いにする
        }

        // ピッチ (ベンド + チューニング + ポルタメント)
        //
        // portaActive の場合、上の portamento_.start() で isRunning()==true に
        // なっているため、ここでは即座に書き込まず timerCallback の
        // portamento_.update() に滑らかな追従を任せる。
        int16_t fine = 0; // kfs単位 (1半音=64ステップ、docs/terminology.md参照)
        if (!portaActive) {
            // bendKfs/tuneKfs: 変数名の通りkfs単位(旧bendCents/tuneCentsから
            // 改名。実体はプレーンなセントではなくkfsだったため、実体に
            // 合わせて2026年7月にリネームした)。
            int16_t bendKfs = static_cast<int16_t>(bendRange_)
                             * (static_cast<int16_t>(pitchBend_ >> 7) - 64);
            int16_t tuneKfs = static_cast<int16_t>(tuning_ >> 7) - 64;
            fine = bendKfs + tuneKfs;
            // SwPatch.fineTransposeのセント端数部分をkfs単位に変換して加算
            // (fineFreq = 64kfs/半音)。
            if (swTransposeFineCents != 0) {
                fine = static_cast<int16_t>(fine + swTransposeFineCents * 64 / 100);
            }
        }
        dev->setNoteFine(devCh, static_cast<uint8_t>(transposed), fine, !portaActive);

        // SwPatch(パフォーマンスパッチ)は、上記のassignCh/allocChWithFallback
        // 呼び出し時に直接渡し済み(2026年7月、pendingSwPatch機構を廃止して
        // 単純化。旧設計はVoiceProcessor::onNoteOn()がupdateVoice()より
        // 後に呼ばれていたための回避策だったが、根本原因(呼び出し順序)を
        // 修正したため不要になった)。

        // NoteOn
        if (!mono_ || !legato_) {
            // 通常の NoteOn: エンベロープを再トリガーする
            dev->noteOn(devCh, vel);
        } else {
            // レガート: エンベロープは維持したまま、ベロシティのみ更新する
            // (ピッチは上記の setNoteFine / ポルタメントで既に反映済み)
            dev->setVelocity(devCh, vel);
        }

        enterNote(li, devCh, note, dev, thisSeq);
    }
}

// ----------------------------------------------------------------
//  NoteOff
// ----------------------------------------------------------------
void CInstCh::noteOff(uint8_t note)
{
    if (mono_ && legato_) return;

    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid()) continue;
        if (note != 0xFF && h.note != note) continue;

        if (h.sostenutoHeld) {
            // Sostenuto 保持中: 実際の noteOff() を遅延させる。
            // notes_[] のエントリは残したまま、pendingRelease だけ立てる。
            h.pendingRelease = true;
        } else {
            if (h.dev) {
                h.dev->noteOff(h.devCh);
                // releaseCh は noteOff に内包済み。別途呼び出し不要。
            }
            leaveNote(hi);
        }
        if (note != 0xFF) break; // 1音だけ止める
    }
}

// ----------------------------------------------------------------
//  AllNoteOff
// ----------------------------------------------------------------
void CInstCh::allNoteOff()
{
    for (int hi = MAX_NOTES - 1; hi >= 0; --hi) {
        auto& h = notes_[hi];
        if (!h.isValid()) continue;
        if (h.dev) {
            h.dev->noteOff(h.devCh);   // releaseCh は noteOff に内包済み
        }
        leaveNote(hi);   // sostenutoHeld/pendingRelease も同時にクリアされる
    }
    portamento_.stop();
    timbres_ = 0;
}

// CC#120 (All Sound Off): sustain を無視して即座に消音する。
// EG のリリースレートを一時的に最大化してから通常の release() に入るため、
// allNoteOff (CC#123) より速く無音になる。
void CInstCh::allSoundOff()
{
    for (int hi = MAX_NOTES - 1; hi >= 0; --hi) {
        auto& h = notes_[hi];
        if (!h.isValid()) continue;
        if (h.dev) {
            h.dev->forceDamp(h.devCh);
        }
        leaveNote(hi);   // sostenutoHeld/pendingRelease も同時にクリアされる
    }
    portamento_.stop();
    timbres_ = 0;
}

// ----------------------------------------------------------------
//  ResetAllCtrl
// ----------------------------------------------------------------
void CInstCh::resetAllCtrl()
{
    setVolume(100);
    setExpression(127);
    setPanpot(64);
    setPitchBend(8192);
    setSustain(false);
    setSostenuto(false);   // 保留中のノートも releaseSostenutoNotes() で解放される
    setModulation(0);
    setPortamento(false);
    setLegato(false);
    portaSourceNote_ = 0xFF;  // CC#84 の保留状態もクリア
    bendRange_ = 2;
    tuning_    = 8192;
    pmDepth_ = 0;
    setHwLfoDepth(0);
    setHwLfoRate(0);
    lfoRateOverride_  = -1;
    lfoDelayOverride_ = -1;
    lfoDepthOverrideCents_ = -2000; // 上書き解除(音色データのdepthCentsに戻す)
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (h.isValid() && h.dev) h.dev->setLfoDepthOverride(h.devCh, lfoDepthOverrideCents_);
    }
    modDepthRange_ = 32;  // kCC1DefaultMaxDepth
}

// ----------------------------------------------------------------
//  コントロールチェンジ
// ----------------------------------------------------------------

void CInstCh::setVolume(uint8_t vol)
{
    volume_ = vol;
    applyVolExpToAll();
}

void CInstCh::setExpression(uint8_t exp)
{
    expression_ = exp;
    applyVolExpToAll();
}

void CInstCh::setPanpot(uint8_t pan)
{
    panpot_ = pan;
    applyPanpotToAll();
}

void CInstCh::setPitchBend(uint16_t pb)
{
    pitchBend_ = pb;
    applyPitchBendToAll();
}

void CInstCh::setSustain(uint8_t sus)
{
    sustain_ = (sus >= 64);
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (h.isValid() && h.dev) h.dev->setSustain(h.devCh, sustain_);
    }
}

void CInstCh::setModulation(uint8_t dep)
{
    pmDepth_ = dep;

    // 発音中の全チャンネルに CC#1 変化を通知
    // LFR=0 の音色のみ反映される（VoiceProcessor::setCC1Modulation 内で判定）
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        // devCh の VoiceProcessor に CC#1 値を通知
        h.dev->setCC1Modulation(h.devCh, dep, modDepthRange_);
    }
}

// CC#4(フットコントローラー)。以前はハードウェアLFOのAMデプスに
// 間接的に使われていたが、2026年7月にAM/PMの有効化をボイス自身の
// AMS/PMSへ一本化したのに伴い、フットコントローラーが操作すべき
// パラメータが無くなった。ソフトウェアAM(トレモロ)相当の仕組みは
// 現状存在しないため、当面は無効(受信するが何もしない)。
void CInstCh::setFootCtrl(uint8_t /*dep*/)
{
}

// CC#2(ブレスコントローラー)。CC#4と同じ理由で当面は無効。
void CInstCh::setBreathCtrl(uint8_t /*dep*/)
{
}

void CInstCh::setPortamento(bool on)
{
    portamento_.enable(on);
    if (!on) portamento_.stop();
}

// CC#84 Portamento Control (Source Note)
// 次の NoteOn のグライド元を明示指定する one-shot コントローラ。
// 通常は「直前に鳴っていたノート」が自動的にグライド元になるが、
// これを使うと任意のノートを起点に指定できる。
void CInstCh::setPortamentoSource(uint8_t note)
{
    portaSourceNote_ = note;
}

void CInstCh::setPortTime(uint8_t pt)
{
    portamento_.setSpeed(pt);
}

void CInstCh::setLegato(bool leg)
{
    legato_ = leg;
}

void CInstCh::setSostenuto(bool sos)
{
    if (sos == sostenuto_) return;
    sostenuto_ = sos;

    if (sos) {
        // ON: 現在発音中の全ノートをスナップショットしてホールド対象にする。
        // これ以降に NoteOn したノートは対象外 (sostenutoHeld=false のまま)。
        for (auto& h : notes_) {
            if (h.isValid()) h.sostenutoHeld = true;
        }
    } else {
        // OFF: ホールド中のノートを解放する。
        releaseSostenutoNotes();
    }
}

// Sostenuto OFF 時、または強制リセット時に呼ばれる。
// pendingRelease 中 (MIDI NoteOff 済み) のノートは実際に noteOff() する。
// まだ鍵盤が押されたまま (pendingRelease=false) のノートは
// sostenutoHeld フラグだけ落とし、通常の NoteOff 待ちに戻す。
void CInstCh::releaseSostenutoNotes()
{
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.sostenutoHeld) continue;

        h.sostenutoHeld = false;

        if (h.pendingRelease) {
            // ボイススティールで devCh が別の発音に再利用されていないか確認
            if (h.dev && h.dev->isChOwnedBy(h.devCh, this)) {
                h.dev->noteOff(h.devCh);
            }
            leaveNote(hi);
        }
        // pendingRelease=false (まだ鍵盤保持中) はそのまま発音継続
    }
}

void CInstCh::setForceDamp(bool fd)
{
    forceDamp_ = fd;
    if (fd) allNoteOff();
}

void CInstCh::bankSelMSB(uint8_t msb)
{
    bankSelM_ = msb;
    // NRPN 96,1による物理チャンネル固定は、バンクセレクト受信で
    // 都度解除される(applyPhyChOverride参照)。
    phyCh_ = 127;
    // MSB が変わったら prog を再ロード (旧挙動)
    // progChange(programNo_);  // 必要に応じて有効化
}

void CInstCh::bankSelLSB(uint8_t lsb)
{
    bankSelL_ = lsb;
    phyCh_ = 127;
}

void CInstCh::setBendRange(uint8_t range)
{
    bendRange_ = range;
    applyPitchBendToAll();
}

void CInstCh::setFineTune(uint16_t tune)
{
    tuning_ = tune;
    applyPitchBendToAll();
}

void CInstCh::setCoarseTune(uint16_t tune)
{
    coarseTune_ = tune;
    applyPitchBendToAll();
}

void CInstCh::refreshPitch()
{
    applyPitchBendToAll();
}

void CInstCh::setRPNRegister(uint16_t reg, uint16_t val)
{
    switch (reg) {
    case 0x0000: setBendRange(static_cast<uint8_t>(val >> 7)); break;
    case 0x0001: setFineTune(val); break;
    case 0x0002: setCoarseTune(val); break;
    case 0x0005: {
        // RPN#5: Modulation Depth Range (Vibrato Depth Range)
        // val は 14bit (0〜16383)。MSB 7bit がセント単位の最大デプス。
        // セント → Fnum steps: steps = cents * 64 / 100
        const uint8_t cents = static_cast<uint8_t>(val >> 7);
        modDepthRange_ = static_cast<int16_t>(cents * 64 / 100);
        // 発音中チャンネルに即時反映
        for (int hi = 0; hi < MAX_NOTES; ++hi) {
            auto& h = notes_[hi];
            if (!h.isValid() || !h.dev) continue;
            h.dev->setCC1Modulation(h.devCh, pmDepth_, modDepthRange_);
        }
        break;
    }
    default: break;
    }
}

void CInstCh::setNRPNRegister(uint16_t reg, uint16_t val)
{
    switch (reg) {
    case 0x3001:
        // valはCC#6(Data Entry MSB)<<7 | CC#38(Data Entry LSB)。
        // このNRPNはMSBのみを使う(2026年7月、条件付き発動へ再設計。
        // 以前はLSB側を参照していたが、DAW等が通常MSBのみを送るため
        // 事実上機能していなかった)。
        applyPhyChOverride(static_cast<uint8_t>(val >> 7));
        break;
    case 0x3002: // NRPN 96,2: パフォーマンスバンクセレクト
        // まだ確定しない。96,3受信時に、この値とセットで確定させる。
        pendingSwBankOverride_ = static_cast<int8_t>(val >> 7);
        break;
    case 0x3003: // NRPN 96,3: パフォーマンスプログラムセレクト
        // このNRPN受信時点のpendingSwBankOverride_と、このNRPN自身の
        // Data Entry MSB値をセットで確定させる。次のプログラム
        // チェンジ受信まで有効。
        swBankOverride_ = pendingSwBankOverride_;
        swProgOverride_ = static_cast<int8_t>(val >> 7);
        break;
    default: break;
    }
}

// NRPN 96,1 (0x3001) + Data Entry MSB受信時の物理チャンネル固定。
// 詳細な条件はMidiCh.hの宣言コメント参照。
void CInstCh::applyPhyChOverride(uint8_t requestedCh)
{
    phyCh_ = 127; // まず解除(新たな受信のたびに都度クリアする仕様)

    if (!mono_) return;                        // モノフォニックのみ対象
    if (bankSelM_ == 0) return;                 // 直接モードのみ対象
    if (resolver_.layerCount() == 0) return;    // 有効なデバイス未選択

    const auto* rl = resolver_.layer(0);
    if (!rl || rl->deviceIndex < 0 || !fitom_) return;
    ISoundDevice* dev = fitom_->getDevice(rl->deviceIndex);
    if (!dev) return;

    // dev->getChCount()はCSpanDevice(複数チップの束ね)の場合、既に
    // 合算後のチャンネル数を返す。
    if (requestedCh < dev->getChCount()) {
        phyCh_ = requestedCh;
    }
}

// ----------------------------------------------------------------
//  CC#96 / #97 (Data Increment / Decrement)
//
//  各パラメータの「1ステップ」は、対応するRPN/NRPNのデータエントリー
//  (CC#6/#38)が実際にどのビット位置を参照するかに合わせている:
//  bendRange_はMSBのみ(1ステップ=1)、tuning_/coarseTune_は14bit値
//  そのものだがMSB単位で変化させるのが自然なため1ステップ=128。
//  NRPN 96,1(物理チャンネル固定)は1ステップ=1だが、
//  applyPhyChOverride()の条件判定(モノ/直接モード/範囲)を毎回
//  経由する点が他と異なる(詳細はcase 0x3001のコメント参照)。
// ----------------------------------------------------------------
void CInstCh::dataIncrement(uint16_t reg, bool isNrpn)
{
    if (isNrpn) {
        switch (reg) {
        case 0x3001: {
            // applyPhyChOverride()の条件判定(モノ/直接モード/範囲)を
            // 経由させるため、直接phyCh_を書き換えず現在値を起点に
            // +1した値で再度条件判定させる。未発動中(phyCh_==127)なら
            // 0から開始する。
            int base = (phyCh_ != 127) ? static_cast<int>(phyCh_) : -1;
            applyPhyChOverride(static_cast<uint8_t>(std::min(base + 1, 126)));
            break;
        }
        default: break;
        }
        return;
    }
    switch (reg) {
    case 0x0000:
        setBendRange(static_cast<uint8_t>(std::min<int>(static_cast<int>(bendRange_) + 1, 127)));
        break;
    case 0x0001:
        setFineTune(static_cast<uint16_t>(std::min<int>(static_cast<int>(tuning_) + 128, 16383)));
        break;
    case 0x0002:
        setCoarseTune(static_cast<uint16_t>(std::min<int>(static_cast<int>(coarseTune_) + 128, 16383)));
        break;
    case 0x0005: {
        int cents = std::clamp(modDepthRange_ * 100 / 64 + 1, 0, 127);
        setRPNRegister(0x0005, static_cast<uint16_t>(cents) << 7);
        break;
    }
    default: break;
    }
}

void CInstCh::dataDecrement(uint16_t reg, bool isNrpn)
{
    if (isNrpn) {
        switch (reg) {
        case 0x3001: {
            // applyPhyChOverride()の条件判定を経由させるため、直接
            // phyCh_を書き換えず現在値を起点に-1した値で再度判定させる。
            // 未発動中(phyCh_==127)なら0のまま(それ以上下げない)。
            int base = (phyCh_ != 127) ? static_cast<int>(phyCh_) : 0;
            applyPhyChOverride(static_cast<uint8_t>(std::max(base - 1, 0)));
            break;
        }
        default: break;
        }
        return;
    }
    switch (reg) {
    case 0x0000:
        setBendRange(static_cast<uint8_t>(std::max<int>(static_cast<int>(bendRange_) - 1, 0)));
        break;
    case 0x0001:
        setFineTune(static_cast<uint16_t>(std::max<int>(static_cast<int>(tuning_) - 128, 0)));
        break;
    case 0x0002:
        setCoarseTune(static_cast<uint16_t>(std::max<int>(static_cast<int>(coarseTune_) - 128, 0)));
        break;
    case 0x0005: {
        int cents = std::clamp(modDepthRange_ * 100 / 64 - 1, 0, 127);
        setRPNRegister(0x0005, static_cast<uint16_t>(cents) << 7);
        break;
    }
    default: break;
    }
}

// ----------------------------------------------------------------
//  CC#126 (Mono Mode On) / CC#127 (Poly Mode On)
//
//  voices(CC#126の値、M)の扱い:
//   M=1        : 真のモノフォニック。レガート(CC#68)・ポルタメント
//                (CC#5/#65/#84)が機能する既存のモノ専用経路
//                (noteOn()内のmono_分岐)を有効にする。
//   M=0 または
//   M>=2       : レガート/ポルタメントとの整合性のため、mono_は
//                falseのまま(=ポリフォニック動作)とし、代わりに
//                「ボイス数上限付きスティール」(voiceLimit_)を有効に
//                する。M=0は規格上「使える分だけ」を意味するため、
//                パッチが自動算出したpoly_をそのまま上限に使う。
//                M>=2はその値(MAX_NOTES上限)を明示的なボイス数上限
//                として使う。
// ----------------------------------------------------------------
void CInstCh::setMonoMode(uint8_t voices)
{
    allNoteOff();   // モード切替時は発音中のノートを一旦整理する(旧実装と同じ方針)
    if (voices == 1) {
        mono_ = true;
    } else {
        mono_ = false;
        voiceLimitOverride_ = true;
        voiceLimit_ = (voices == 0)
            ? poly_
            : std::min<uint8_t>(voices, static_cast<uint8_t>(MAX_NOTES));
    }
}

void CInstCh::setPolyMode()
{
    allNoteOff();
    mono_   = false;
    legato_ = false;
    voiceLimitOverride_ = false;
    voiceLimit_ = poly_;
}

// ----------------------------------------------------------------
//  タイマーコールバック
// ----------------------------------------------------------------

void CInstCh::timerCallback(uint32_t tick)
{
    // ポルタメント更新 (モノフォニックチャンネル専用)
    // mono_ でチェックすることで、ポリ⇔モノ切り替え時に
    // 残留したグライド状態がポリの和音に影響しないことを保証する。
    if (mono_ && portamento_.isEnabled() && portamento_.isRunning()) {
        portamento_.update();
        for (int hi = 0; hi < MAX_NOTES; ++hi) {
            auto& h = notes_[hi];
            if (!h.isValid() || !h.dev) continue;
            int16_t fine = static_cast<int16_t>(portamento_.getCurrentFine());
            h.dev->setNoteFine(h.devCh, portamento_.getCurrentNote(), fine);
        }
    }
}

void CInstCh::midiClockCallback(uint32_t /*tick*/)
{
    // 将来: シーケンサー同期 LFO 等
}

// ----------------------------------------------------------------
//  モニタリング
// ----------------------------------------------------------------

uint8_t CInstCh::getLastNote() const
{
    for (int hi = MAX_NOTES - 1; hi >= 0; --hi) {
        if (notes_[hi].isValid()) return notes_[hi].note;
    }
    return 0xFF;
}

uint8_t CInstCh::getLastDeviceIndex() const
{
    for (int hi = MAX_NOTES - 1; hi >= 0; --hi) {
        if (!notes_[hi].isValid()) continue;
        const auto* layer = resolver_.layer(notes_[hi].layerIdx);
        if (layer) return static_cast<uint8_t>(layer->deviceIndex);
    }
    return 0xFF;
}

uint8_t CInstCh::getLastDevCh() const
{
    for (int hi = MAX_NOTES - 1; hi >= 0; --hi) {
        if (notes_[hi].isValid()) return notes_[hi].devCh;
    }
    return 0xFF;
}

// ----------------------------------------------------------------
//  内部ヘルパー
// ----------------------------------------------------------------

ISoundDevice* CInstCh::getLayerDevice(int layerIdx) const
{
    if (!fitom_) return nullptr;
    const auto* rl = resolver_.layer(layerIdx);
    if (!rl) return nullptr;
    return fitom_->getDevice(rl->deviceIndex);
}

CInstCh::NoteHist* CInstCh::findNote(uint8_t note, int layerIdx)
{
    for (auto& h : notes_) {
        if (!h.isValid()) continue;
        if (h.note == note && (layerIdx < 0 || h.layerIdx == layerIdx))
            return &h;
    }
    return nullptr;
}

void CInstCh::enterNote(int layerIdx, uint8_t devCh, uint8_t note, ISoundDevice* dev, uint32_t seq)
{
    // 空きスロットを探す
    for (auto& h : notes_) {
        if (!h.isValid()) {
            h.layerIdx = static_cast<uint8_t>(layerIdx);
            h.devCh    = devCh;
            h.note     = note;
            h.dev      = dev;
            h.seq      = seq;
            ++timbres_;
            return;
        }
    }
    // 空きなし(MAX_NOTES件全て使用中): stealOldestNoteIfNeeded()が
    // noteOn()の先頭で毎回ボイス数上限を強制しているため、通常は
    // ここに到達しない。万一到達した場合に備え、最も古いエントリ
    // (notes_[0])が使っていたデバイスチャンネルを正しく解放してから
    // 上書きする(2026年7月修正: 以前はnoteOff()を呼ばずに上書きして
    // おり、デバイスチャンネルが解放されないまま迷子になっていた)。
    if (notes_[0].dev) notes_[0].dev->noteOff(notes_[0].devCh);
    notes_[0].layerIdx = static_cast<uint8_t>(layerIdx);
    notes_[0].devCh    = devCh;
    notes_[0].note     = note;
    notes_[0].dev      = dev;
    notes_[0].seq      = seq;
    notes_[0].sostenutoHeld  = false;
    notes_[0].pendingRelease = false;
    FITOM_LOG_DEBUG("CInstCh ch=" << (int)ch_ << ": note history overflow");
}

// ボイス数上限(voiceLimit_)によるスティール。mono_==trueの間は呼ばれない
// (noteOn()側で分岐)。
void CInstCh::stealOldestNoteIfNeeded()
{
    if (voiceLimit_ == 0) return;

    // notes_[]はレイヤー単位のエントリのため、note番号ごとにユニークな
    // 1ボイスとして集計し直す(同じnoteの複数レイヤーは同じseqを持つ)。
    uint8_t  heldNotes[MAX_NOTES];
    uint32_t heldSeq[MAX_NOTES];
    int heldCount = 0;
    for (const auto& h : notes_) {
        if (!h.isValid()) continue;
        int idx = -1;
        for (int k = 0; k < heldCount; ++k) {
            if (heldNotes[k] == h.note) { idx = k; break; }
        }
        if (idx < 0) {
            heldNotes[heldCount] = h.note;
            heldSeq[heldCount]   = h.seq;
            ++heldCount;
        } else if (h.seq < heldSeq[idx]) {
            heldSeq[idx] = h.seq;
        }
    }

    if (heldCount < static_cast<int>(voiceLimit_)) return;

    // 発音順が最も古い(seqが最小)ノートを選ぶ
    int oldestIdx = 0;
    for (int k = 1; k < heldCount; ++k) {
        if (heldSeq[k] < heldSeq[oldestIdx]) oldestIdx = k;
    }
    const uint8_t stealNote = heldNotes[oldestIdx];

    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (h.isValid() && h.note == stealNote) {
            if (h.dev) h.dev->noteOff(h.devCh);
            leaveNote(hi);
        }
    }
}

void CInstCh::leaveNote(int histIdx)
{
    if (histIdx < 0 || histIdx >= MAX_NOTES) return;
    if (!notes_[histIdx].isValid()) return;
    notes_[histIdx] = NoteHist{};
    if (timbres_ > 0) --timbres_;
}

void CInstCh::applyVolExpToAll()
{
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        const auto* rl = resolver_.layer(h.layerIdx);
        int adjVol = volume_ + (rl ? rl->layer->volumeOffset : 0);
        adjVol = std::clamp(adjVol, 0, 127);
        h.dev->setVolume(h.devCh, static_cast<uint8_t>(adjVol));
        h.dev->setExpression(h.devCh, expression_);
    }
}

void CInstCh::applyPanpotToAll()
{
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        const auto* rl = resolver_.layer(h.layerIdx);
        int adjPan = static_cast<int>(panpot_) - 64 + (rl ? rl->layer->panOffset : 0);
        adjPan = std::clamp(adjPan, -64, 63);
        h.dev->setPanpot(h.devCh, static_cast<int8_t>(adjPan));
    }
}

void CInstCh::applyPitchBendToAll()
{
    // fineFreq(setNoteFineの引数)の単位は「1半音 = 64ステップ」
    // (CSoundDevice::getFnumberのindex計算: lastNote*64 + ... 参照)。
    // 以下、bendRange_[半音]の適用も含めて、全てこの単位系に揃える。
    int16_t bendSteps = static_cast<int16_t>(bendRange_)
                      * (static_cast<int16_t>(pitchBend_ >> 7) - 64);
    // RPN#1 Channel Fine Tuning: 14bit全体(LSBまで)を使う。中心0x2000(8192)。
    // MIDI規格上の範囲は概ね±100cents(=約±1半音=±64ステップ)。
    int16_t fineTuneSteps = static_cast<int16_t>(
        (static_cast<int32_t>(tuning_) - 8192) * 64 / 8192);
    // RPN#2 Channel Coarse Tuning: MSBのみ有効。中心0x40(64)。
    // 範囲-64〜+63半音 → ×64でステップ数に変換。
    int16_t coarseTuneSteps = static_cast<int16_t>(
        (static_cast<int16_t>(coarseTune_ >> 7) - 64) * 64);
    int16_t commonFine = bendSteps + fineTuneSteps + coarseTuneSteps;

    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        // Scale/Octave Tuning (Universal SysEx): ノート(音名, mod12)ごとに
        // 異なるcentsオフセットを持つため、共通finalとは別にノートごとに
        // 加算する。fineFreq単位(1半音=64ステップ)に変換。
        int16_t scaleCents = fitom_ ? fitom_->getScaleTuningCents(h.note) : 0;
        int16_t scaleSteps = static_cast<int16_t>(
            static_cast<int32_t>(scaleCents) * 64 / 100);
        h.dev->setNoteFine(h.devCh, h.note, commonFine + scaleSteps);
    }
}

// CC#14/#15受信時、既に発音中の全ノートへ即座に反映する
// (ボリューム/パン等、他のCCと同じ「即時反映」方針に合わせる)。
// 有効/無効自体はボイス自身のAMS/PMSで決まるため、ここでは常に
// enablePM/enableAM=trueを送るだけでよい(noteOn()と同じ考え方)。
void CInstCh::applyLFOToAll()
{
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        h.dev->enablePM(h.devCh, true);
        h.dev->enableAM(h.devCh, true);
        h.dev->setLFODepth(h.devCh, hwLfoDepth_);
        h.dev->setLFORate(h.devCh, hwLfoRate_);
    }
}

// CC#14(非標準): HW LFO Depth
void CInstCh::setHwLfoDepth(uint8_t dep)
{
    hwLfoDepth_ = dep;
    applyLFOToAll();
}

// CC#15(非標準): HW LFO Rate
void CInstCh::setHwLfoRate(uint8_t rate)
{
    hwLfoRate_ = rate;
    applyLFOToAll();
}

// CC#76(Sound Controller 7 / Vibrato Rate): ソフトウェアLFOのRateを
// 上書きする。0-127をsw.LFRと同じ単位としてそのまま使う。LFO(再)始動
// 時にしか意味を持たないため、noteOn()側でSwPatchに焼き込んで
// assignCh/allocChへ渡す(即時反映はできない、次のノートオンから反映)。
void CInstCh::setSoftLfoRate(uint8_t rate)
{
    lfoRateOverride_ = rate;
}

// CC#77(Sound Controller 8 / Vibrato Depth): ソフトウェアLFOのDepthを
// 上書きする。0-127を-1200〜+1200セントへ線形マッピングする
// (127で最大デプス)。VoiceProcessor側で毎tick再計算されるため、
// 発音中のノートにも即座に反映される。
void CInstCh::setSoftLfoDepth(uint8_t depth)
{
    lfoDepthOverrideCents_ = static_cast<int16_t>(static_cast<int32_t>(depth) * 1200 / 127);
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        h.dev->setLfoDepthOverride(h.devCh, lfoDepthOverrideCents_);
    }
}

// CC#78(Sound Controller 9 / Vibrato Delay): ソフトウェアLFOのDelayを
// 上書きする。0-127をsw.LFDと同じ単位(20ms)としてそのまま使う。
// Rateと同じ理由でnoteOn()側でSwPatchに焼き込む方式。
void CInstCh::setSoftLfoDelay(uint8_t delay)
{
    lfoDelayOverride_ = delay;
}

// SysEx(private, 00H 48H 01H, sub-cmd 0x01, target-type 0x00)による
// HwPatchパラメータオーバーライド。詳細な規約はMidiCh.hの
// hwPatchOverride_コメント参照。
bool CInstCh::mergeHwPatchOverride(uint8_t layer, const std::string& jsonText)
{
    if (layer >= MAX_TONE_LAYERS) return false;

    // 空オブジェクト"{}"は明示的な解除コマンドとして扱う(マージ対象
    // フィールドが1つも無い=何も変化しない、では利用者が「元に戻す」
    // 操作をできなくなるため、特別扱いする)。
    // 簡易判定: 空白文字を除いて"{}"であれば解除とみなす
    // (本格的なJSONパースをする前の軽量なショートカット)。
    {
        std::string trimmed;
        for (char c : jsonText) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') trimmed += c;
        }
        if (trimmed == "{}") {
            hwPatchOverrideActive_[layer] = false;
            // 発音中のノートにも即座に「元の音色」を反映する。
            for (int hi = 0; hi < MAX_NOTES; ++hi) {
                auto& h = notes_[hi];
                if (!h.isValid() || h.layerIdx != layer || !h.dev) continue;
                const auto* rl = resolver_.layer(layer);
                if (rl && rl->hwPatch) h.dev->setVoice(h.devCh, *rl->hwPatch, true);
            }
            return true;
        }
    }

    if (!patchMgr_) return false;

    // マージの起点: 既にこのレイヤーのオーバーライドが有効なら
    // 積み上げてマージする(複数回のSysExで少しずつ調整できるように)。
    // 無効なら、現在このチャンネルが実際に使っているHwPatchを起点に
    // コピーする。
    if (!hwPatchOverrideActive_[layer]) {
        const auto* rl = resolver_.layer(layer);
        if (rl && rl->hwPatch) {
            hwPatchOverride_[layer] = *rl->hwPatch;
        } else {
            hwPatchOverride_[layer] = HwPatch{};
        }
    }

    std::string err;
    if (!patchMgr_->mergeHwPatchFromJsonText(jsonText, hwPatchOverride_[layer], &err)) {
        FITOM_LOG_WARN("CInstCh ch=" << (int)ch_ << " layer=" << (int)layer
            << ": HwPatch override JSON parse failed: " << err);
        return false;
    }
    hwPatchOverrideActive_[layer] = true;

    // 発音中のノートにも即座に反映する(音作りをリアルタイムで
    // 試聴しながら調整できるように)。
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || h.layerIdx != layer || !h.dev) continue;
        h.dev->setVoice(h.devCh, hwPatchOverride_[layer], true);
    }
    return true;
}

void CInstCh::clearHwPatchOverrides()
{
    hwPatchOverrideActive_.fill(false);
}

// ================================================================
//  CRhythmCh
// ================================================================

CRhythmCh::CRhythmCh(uint8_t ch, CFITOM* parent)
    : ch_(ch), fitom_(parent)
{
    for (auto& s : noteSlots_) { s.layers.fill(LayerSlot{}); s.gateRem = 0; }
    noteAdj_.fill(NoteAdj{});
    noteCache_.fill(NoteCache{});
}

CRhythmCh::~CRhythmCh() { allNoteOff(); }

// ----------------------------------------------------------------
//  ProgChange: DrumPatch を選択してキャッシュをクリア
// ----------------------------------------------------------------
// ----------------------------------------------------------------
//  ProgChange: ドラムバンクは固定バンク番号(0)を使う。CC#0/CC#32は
//  無視されるため、MIDI経由でのドラムバンク切替はできない仕様。
//  Prog Chg. の値が、固定バンク内のドラムキット(DrumPatch)インデックスを
//  選択する。
// ----------------------------------------------------------------
void CRhythmCh::progChange(uint8_t prog)
{
    programNo_    = prog;
    currentPatch_ = nullptr;
    clearNoteCache();

    if (!fitom_) return;

    static constexpr int kFixedDrumBank = 0;
    auto& pm = fitom_->getPatchManager();
    currentPatch_ = pm.resolveDrum(kFixedDrumBank, prog);
    if (!currentPatch_) {
        FITOM_LOG_WARN("CRhythmCh ch=" << (int)ch_
            << ": DrumPatch not found bank=" << kFixedDrumBank
            << " prog=" << (int)prog);
    } else {
        FITOM_LOG_DEBUG("CRhythmCh ch=" << (int)ch_
            << ": DrumPatch '" << currentPatch_->name << "'");
    }
}

// リズムチャンネルはCC#0/CC#32(バンクセレクト)を一切使わない。
// (CC#0=0x79によるメロディチャンネルへの切替は、この関数に到達する前に
//  MidiProcessor::processControl層で消費・処理される。それ以外の値
//  (通常のバンクセレクト値)は単に無視する)。
void CRhythmCh::bankSelMSB(uint8_t /*msb*/) {}
void CRhythmCh::bankSelLSB(uint8_t /*lsb*/) {}
void CRhythmCh::setVolume(uint8_t vol)   { volume_   = vol; }

// ----------------------------------------------------------------
//  NoteOn
// ----------------------------------------------------------------
void CRhythmCh::noteOn(uint8_t midiNote, uint8_t vel)
{
    lastNote_ = midiNote;
    if (!fitom_ || !currentPatch_) return;

    const DrumNote* dn = currentPatch_->getNote(midiNote);
    if (!dn) return;

    applyNoteOn(midiNote, vel, *dn);
}

// ----------------------------------------------------------------
//  applyNoteOn: CInstCh::noteOn と同じ PatchResolver 経路
// ----------------------------------------------------------------
void CRhythmCh::applyNoteOn(uint8_t midiNote, uint8_t vel, const DrumNote& dn)
{
    // 既発音を停止（同一ノートを上書き）
    noteSlots_[midiNote].stopAll();

    // チョークグループ: 同一グループの他のノートが発音中なら強制ダンプ
    // する(例: クローズ/オープンハイハット)。ハードウェアのチャンネル
    // 共有には依存しない、ノート単位の明示的な停止処理。全ToneLayerを
    // 止めるためマルチレイヤーでも正しく機能する。
    if (currentPatch_) {
        const auto* group = currentPatch_->findChokeGroup(midiNote);
        if (group) {
            for (uint8_t n : *group) noteSlots_[n].stopAll();
        }
    }

    // Patch を解決（キャッシュ優先）
    const ResolvedPatch* rp = resolveNote(midiNote, dn);
    if (!rp || !rp->isValid()) {
        FITOM_LOG_WARN("CRhythmCh: Patch not found for note=" << (int)midiNote
            << " patchBank=" << (int)dn.patchBank
            << " patchProg=" << (int)dn.patchProg);
        return;
    }

    // NRPN リアルタイム調整
    const auto& adj = noteAdj_[midiNote];

    auto& slots = noteSlots_[midiNote];
    slots.gateRem = dn.gateTime;

    // 全 ToneLayer を CInstCh::noteOn と同じ経路で発音
    for (int li = 0; li < rp->layerCount; ++li) {
        const auto* rl = &rp->layers[li];
        if (!rl || !rl->layer || !rl->layer->isActive()) continue;

        ISoundDevice* dev = fitom_->getDevice(rl->deviceIndex);
        if (!dev) continue;

        const HwPatch* patch = rl->hwPatch;
        // サンプルベース音源系 (VOICE_PATCH_AWM等) の場合のみ非nullptr。
        // patchとは排他 (PatchManager::resolve()が保証する)。
        // 以前はpatch==nullptrで即座にレイヤーをスキップしていたため、
        // AWM系のドラム音色が一切発音されなかった (未解決の欠陥)。
        const SampleZonePatch* samplePatch = rl->samplePatch;
        if (!patch && !samplePatch) continue;

        // ─── チャンネル割り当て ─────────────────────────────────────
        // 内蔵リズム音源(COPNARhythm/COPLLRhythm)の「チャンネル番号=
        // 発音される楽器」というハードウェア制約は、rl->forcedChとして
        // PatchManager::resolveBuiltinRhythm側で既に解決・検証済み。
        // forcedChが無効(-1)のままここに来ることはない —
        // resolveBuiltinRhythmはhwProgが範囲外の場合、レイヤー自体を
        // 無効(rl->layer/hwPatchが無い状態)として返すため、この関数の
        // 先頭で既にスキップされている。
        //
        // このノートに適用すべきSwPatch(パフォーマンスパッチ)。
        // DrumNote.swBank/swProgによる上書き(layer[0]専用の制約)は
        // resolveNote()内で事前に解決・キャッシュ済み
        // (noteCache_[midiNote].effectiveSwPatch0)。layer[0]以外は、
        // そのレイヤーが参照するHwPatch自身のswPatchをそのまま使う。
        // assignCh/allocCh呼び出し時に直接渡す(2026年7月、
        // pendingSwPatch機構を廃止して単純化)。
        const SwPatch* effectiveSwPatch =
            (li == 0) ? noteCache_[midiNote].effectiveSwPatch0 : rl->swPatch;

        // ベロシティ (vol + NRPN)
        int adjVel = static_cast<int>(vel) + adj.vel;
        adjVel = std::clamp(adjVel, 1, 127);

        uint8_t devCh = 0xFF;
        if (rl->forcedCh >= 0) {
            // ハードウェア制約による強制チャンネル(ビルトインリズム等)。
            devCh = dev->assignCh(static_cast<uint8_t>(rl->forcedCh), this, patch,
                                   static_cast<uint8_t>(adjVel), effectiveSwPatch, samplePatch);
        } else {
            devCh = dev->allocCh(this, patch, static_cast<uint8_t>(adjVel),
                                  effectiveSwPatch, samplePatch);
        }
        if (devCh == 0xFF) continue;

        // ─── パラメータ設定 ─────────────────────────────────────────
        // ボリューム (ドラム全体 vol + レイヤーオフセット)
        int adjVol = static_cast<int>(volume_) + rl->layer->volumeOffset;
        adjVol = std::clamp(adjVol, 0, 127);
        dev->setVolume(devCh, static_cast<uint8_t>(adjVol), false);

        // パン (DrumNote.pan + レイヤーの pan_offset + NRPN)
        int adjPan = static_cast<int>(dn.pan)
                   + rl->layer->panOffset
                   + adj.pan;
        adjPan = std::clamp(adjPan, -64, 63);
        dev->setPanpot(devCh, static_cast<int8_t>(adjPan), false);

        // ノート: DrumNote.playNote を絶対指定、ToneLayer.transpose は無視
        // (ドラムはトランスポーズより play_note の絶対指定が自然)。
        // SwPatch.fineTransposeも同じ理由で、ここでは意図的に適用しない。
        int playNote = static_cast<int>(dn.playNote) + adj.pitch;
        playNote = std::clamp(playNote, 0, 127);
        int16_t fine = static_cast<int16_t>(dn.fineTune);
        dev->setNoteFine(devCh, static_cast<uint8_t>(playNote), fine, true);

        dev->noteOn(devCh, static_cast<uint8_t>(adjVel));

        // 発音記録
        slots.layers[li] = LayerSlot{ dev, devCh,
                                       static_cast<uint8_t>(li) };
    }
}

// ----------------------------------------------------------------
//  NoteCache: patchBank/patchProg が同じなら再解決しない
// ----------------------------------------------------------------
const ResolvedPatch* CRhythmCh::resolveNote(uint8_t midiNote, const DrumNote& dn)
{
    auto& cache = noteCache_[midiNote];
    if (cache.voicePatchType == dn.voicePatchType &&
        cache.patchBank == dn.patchBank &&
        cache.patchProg == dn.patchProg &&
        cache.swBank == dn.swBank &&
        cache.swProg == dn.swProg &&
        cache.isValid()) {
        return &cache.resolved;
    }
    // キャッシュミス → 解決
    cache.voicePatchType = dn.voicePatchType;
    cache.patchBank = dn.patchBank;
    cache.patchProg = dn.patchProg;
    cache.swBank = dn.swBank;
    cache.swProg = dn.swProg;
    auto& pm = fitom_->getPatchManager();

    if (dn.voicePatchType == VOICE_PATCH_NONE) {
        // 通常モード: CInstCh::progChangeのbankSelM_==0と同じ経路
        cache.resolved = pm.resolve(dn.patchBank, dn.patchProg, fitom_->getConfig());
    } else {
        // 直接モード: CInstCh::progChangeのbankSelM_!=0と同じ経路。
        // dn.patchBank/patchProgはHwBankインデックス/HwProgとして
        // 読み替わる (DrumNoteのコメント参照)。
        cache.resolved = pm.resolveDirect(dn.voicePatchType, dn.patchBank, dn.patchProg,
                                          fitom_->getConfig(), cache.directStorage);
    }

    // DrumNote.swBank/swProg上書き(layer[0]専用)を解決し、キャッシュする。
    // 優先順位: ①DrumNote指定が解決できればそれを使う → ②DrumNote無指定
    // (-1)、または指定されたが解決に失敗した場合は、"無指定だった場合と
    // 同じ扱い"としてHwPatch自身が参照するswPatchにフォールバックする
    // → ③それも無ければパフォーマンスパッチ無し(無音にはならない、
    // ソフトな失敗)。
    const SwPatch* fromDrumNote =
        (dn.swBank >= 0) ? pm.resolveSwPatch(dn.swBank, dn.swProg) : nullptr;
    if (fromDrumNote) {
        cache.effectiveSwPatch0 = fromDrumNote;
    } else {
        cache.effectiveSwPatch0 = (cache.resolved.layerCount > 0)
            ? cache.resolved.layers[0].swPatch : nullptr;
    }

    return cache.isValid() ? &cache.resolved : nullptr;
}

void CRhythmCh::clearNoteCache()
{
    noteCache_.fill(NoteCache{});
}

// ----------------------------------------------------------------
//  NoteOff
// ----------------------------------------------------------------
void CRhythmCh::noteOff(uint8_t midiNote)
{
    auto& slots = noteSlots_[midiNote];
    if (!slots.anyActive()) return;
    // gateTime > 0 のノートは NoteOff を無視（ゲートタイムで自動停止）
    if (slots.gateRem > 0) return;
    slots.stopAll();
}

void CRhythmCh::allNoteOff()
{
    for (auto& slots : noteSlots_) slots.stopAll();
}

void CRhythmCh::resetAllCtrl()
{
    volume_   = 100;
    bankSelM_ = 0;
    bankSelL_ = 0;
    noteAdj_.fill(NoteAdj{});
}

int CRhythmCh::activeNoteCount() const
{
    int n = 0;
    for (const auto& s : noteSlots_) if (s.anyActive()) ++n;
    return n;
}

uint8_t CRhythmCh::getLastDeviceIndex() const
{
    if (lastNote_ >= 128 || !fitom_) return 0xFF;
    for (const auto& l : noteSlots_[lastNote_].layers) {
        if (l.isActive()) {
            int idx = fitom_->findDeviceIndex(l.dev);
            if (idx >= 0) return static_cast<uint8_t>(idx);
        }
    }
    return 0xFF;
}

uint8_t CRhythmCh::getLastDevCh() const
{
    if (lastNote_ >= 128) return 0xFF;
    for (const auto& l : noteSlots_[lastNote_].layers) {
        if (l.isActive()) return l.devCh;
    }
    return 0xFF;
}

// ----------------------------------------------------------------
//  timerCallback: ゲートタイムのカウントダウン + VoiceProcessor tick
// ----------------------------------------------------------------
void CRhythmCh::timerCallback(uint32_t /*tick*/)
{
    for (int note = 0; note < 128; ++note) {
        auto& slots = noteSlots_[note];
        if (!slots.anyActive()) continue;

        // ゲートタイムのデクリメント
        if (slots.gateRem > 0) {
            --slots.gateRem;
            if (slots.gateRem == 0) {
                slots.stopAll();
                continue;
            }
        }

        // VoiceProcessor tick (ソフト LFO / トレモロ)
        // 各レイヤーの ChState を更新
        for (auto& ls : slots.layers) {
            if (!ls.isActive()) continue;
            auto* st = ls.dev->getChState(ls.devCh);
            if (!st) continue;

            // キャッシュから対応する HwPatch を取得
            const auto& cache = noteCache_[note];
            if (!cache.isValid()) continue;
            const auto* rl = (ls.layerIdx < cache.resolved.layerCount)
                ? &cache.resolved.layers[ls.layerIdx] : nullptr;
            if (!rl || !rl->hwPatch) continue;

            FmVoice fv;
            fv.hw = rl->hwPatch->hw;
            for (int i = 0; i < 4; ++i) fv.hwOp[i] = rl->hwPatch->hwOp[i];
            // layer[0]はDrumNote.swBank/swProg上書き解決結果
            // (effectiveSwPatch0)を使う。それ以外のレイヤーは
            // 各々が参照するHwPatch自身のswPatchをそのまま使う
            // (applyNoteOnと同じロジック)。
            const SwPatch* effectiveSwPatch =
                (ls.layerIdx == 0) ? cache.effectiveSwPatch0 : rl->swPatch;
            if (effectiveSwPatch) {
                fv.sw = effectiveSwPatch->sw;
                for (int i = 0; i < 4; ++i)
                    fv.swOp[i] = effectiveSwPatch->swOp[i];
            }

            auto result = st->proc.onTick(fv);

            if (result.needsFreqUpdate)
                ls.dev->setNoteFine(ls.devCh, ls.dev->getCurrentNote(ls.devCh),
                                    st->proc.channelLfoValue());

            for (int op = 0; op < 4; ++op) {
                if (result.tlUpdateMask & (1u << op))
                    ls.dev->updateTL(ls.devCh, static_cast<uint8_t>(op),
                                     st->proc.effectiveTL(op));
            }
        }
    }
}

void CRhythmCh::setRPNRegister(uint16_t /*reg*/, uint16_t /*val*/) {}

void CRhythmCh::setNRPNRegister(uint16_t reg, uint16_t val)
{
    // GM2 Drum Per-Part NRPN。GM2規格上、Pitch/TVA Level/Panのいずれも
    // Data Entry MSB(0-127、中央値64)のみを使う仕様のため、valの上位
    // 7bit(MSB)を取り出す(2026年7月、下位7bitを取り違えていたバグを
    // 修正。合成後の14bit値をそのまま使っていたため、LSB(CC#38)を
    // 明示的に送らない一般的なDAWでは事実上機能していなかった)。
    uint8_t note = static_cast<uint8_t>(reg & 0x7F);
    if (note >= 128) return;
    const int16_t msb = static_cast<int16_t>(val >> 7);
    switch (reg & 0xFF00) {
    case 0x1800: noteAdj_[note].pitch = msb - 64; break;
    case 0x1A00: noteAdj_[note].vel   = msb - 64; break;
    case 0x1C00: noteAdj_[note].pan   = msb - 64; break;
    default: break;
    }
}

// CC#96 / #97 (Data Increment / Decrement): GM2 Drum Per-Part NRPNのみ対応
// (RPNはリズムチャンネルでは使用しないため常に無視)。
void CRhythmCh::dataIncrement(uint16_t reg, bool isNrpn)
{
    if (!isNrpn) return;
    uint8_t note = static_cast<uint8_t>(reg & 0x7F);
    if (note >= 128) return;
    switch (reg & 0xFF00) {
    case 0x1800: noteAdj_[note].pitch = static_cast<int16_t>(std::min<int>(noteAdj_[note].pitch + 1, 63)); break;
    case 0x1A00: noteAdj_[note].vel   = static_cast<int16_t>(std::min<int>(noteAdj_[note].vel + 1, 63));   break;
    case 0x1C00: noteAdj_[note].pan   = static_cast<int16_t>(std::min<int>(noteAdj_[note].pan + 1, 63));   break;
    default: break;
    }
}

void CRhythmCh::dataDecrement(uint16_t reg, bool isNrpn)
{
    if (!isNrpn) return;
    uint8_t note = static_cast<uint8_t>(reg & 0x7F);
    if (note >= 128) return;
    switch (reg & 0xFF00) {
    case 0x1800: noteAdj_[note].pitch = static_cast<int16_t>(std::max<int>(noteAdj_[note].pitch - 1, -64)); break;
    case 0x1A00: noteAdj_[note].vel   = static_cast<int16_t>(std::max<int>(noteAdj_[note].vel - 1, -64));   break;
    case 0x1C00: noteAdj_[note].pan   = static_cast<int16_t>(std::max<int>(noteAdj_[note].pan - 1, -64));   break;
    default: break;
    }
}

} // namespace fitom
