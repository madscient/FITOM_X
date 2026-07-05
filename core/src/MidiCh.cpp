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
void PortaCtrl::update()
{
    if (state_ != State::Running || !enabled_) return;

    ++count_;
    uint32_t threshold = static_cast<uint32_t>(speed_) + 1;
    if (count_ < threshold) return;
    count_ = 0;

    if (current_ == end_) {
        state_ = State::Idle;
        return;
    }
    if (current_ < end_) ++current_;
    else                  --current_;
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

    FITOM_LOG_DEBUG("CInstCh ch=" << static_cast<int>(ch_)
        << ": ProgChange " << static_cast<int>(prog)
        << " '" << resolved.patch->name << "'"
        << " layers=" << resolved.layerCount
        << " poly=" << static_cast<int>(poly_));

    // Program Change は発音中のノートに一切作用しない (全モード共通の仕様)。
    // 新しいパッチは次の NoteOn から適用される。
}

// ----------------------------------------------------------------
//  NoteOn
// ----------------------------------------------------------------
void CInstCh::noteOn(uint8_t note, uint8_t vel)
{
    if (resolver_.layerCount() == 0) return;

    for (int li = 0; li < resolver_.layerCount(); ++li) {
        const auto* rl = resolver_.layer(li);
        if (!rl || !rl->layer || !rl->layer->isActive()) continue;
        if (!rl->layer->inRange(note)) continue;

        int transposed = rl->layer->transposedNote(note);
        if (transposed < 0 || transposed > 127) continue;

        ISoundDevice* dev = fitom_->getDevice(rl->deviceIndex);
        if (!dev) continue;

        const HwPatch* patch = rl->hwPatch;
        // サンプルベース音源系 (VOICE_PATCH_AWM等) の場合のみ非nullptr。
        // patchとは排他 (PatchManager::resolve()が保証する)。
        const SampleZonePatch* samplePatch = rl->samplePatch;

        // ────────────────────────────────────────────────────────
        // ハードチャンネル割り当て
        // ────────────────────────────────────────────────────────
        // ポルタメントのグライド元候補 (モノ時、同一レイヤーで直前に
        // 鳴っていたノート番号)。0xFF = 該当なし (このレイヤー初のノート)。
        uint8_t prevNote = 0xFF;

        uint8_t devCh = 0xFF;
        if (phyCh_ != 127 && phyCh_ < dev->getChCount()) {
            devCh = dev->assignCh(phyCh_, this, patch, samplePatch);
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
            if (devCh == 0xFF) devCh = dev->allocCh(this, patch, samplePatch);
        } else {
            devCh = dev->allocCh(this, patch, samplePatch);
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

        // HW LFO (Modulation CC)
        if (pmDepth_ > 0) {
            dev->enablePM(devCh, true);
            dev->setPMDepth(devCh, pmDepth_);
            dev->setPMRate(devCh, pmRate_);
        } else if (amDepth_ > 0) {
            dev->enableAM(devCh, true);
            dev->setAMDepth(devCh, amDepth_);
            dev->setAMRate(devCh, amRate_);
        } else {
            dev->enablePM(devCh, false);
            dev->enableAM(devCh, false);
        }

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
        int16_t fine = 0;
        if (!portaActive) {
            int16_t bendCents = static_cast<int16_t>(bendRange_)
                              * (static_cast<int16_t>(pitchBend_ >> 7) - 64);
            int16_t tuneCents = static_cast<int16_t>(tuning_ >> 7) - 64;
            fine = bendCents + tuneCents;
        }
        dev->setNoteFine(devCh, static_cast<uint8_t>(transposed), fine, !portaActive);

        // VoiceProcessor に SW パッチを適用
        // (samplePatchのみ設定されている場合=AWM系レイヤーは、FmVoiceが
        //  FMオペレータ専用の構造体でありサンプルベース音源には適用できない
        //  ため、VoiceProcessorによるソフトLFO/ベロシティ感度処理自体を
        //  スキップする。patch==nullptrのままpatch->hwにアクセスすると
        //  クラッシュするため、必ずpatchの非nullptrチェックを先に行う)。
        if (patch && resolver_.swPatch()) {
            const auto* sp = resolver_.swPatch();
            FmVoice fv;
            fv.hw = patch->hw;
            for (int i = 0; i < 4; ++i) fv.hwOp[i] = patch->hwOp[i];
            fv.sw = sp->sw;
            for (int i = 0; i < 4; ++i) fv.swOp[i] = sp->swOp[i];
            auto* st = dev->getChState(devCh);
            if (st) st->proc.onNoteOn(static_cast<uint8_t>(adjVol), expression_, vel, fv);
        }

        // NoteOn
        if (!mono_ || !legato_) {
            // 通常の NoteOn: エンベロープを再トリガーする
            dev->noteOn(devCh, vel);
        } else {
            // レガート: エンベロープは維持したまま、ベロシティのみ更新する
            // (ピッチは上記の setNoteFine / ポルタメントで既に反映済み)
            dev->setVelocity(devCh, vel);
        }

        enterNote(li, devCh, note, dev);
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
    pmDepth_ = amDepth_ = pmRate_ = amRate_ = 0;
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

void CInstCh::setFootCtrl(uint8_t dep)
{
    amDepth_ = dep;
}

void CInstCh::setBreathCtrl(uint8_t dep)
{
    amDepth_ = dep;
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
    // MSB が変わったら prog を再ロード (旧挙動)
    // progChange(programNo_);  // 必要に応じて有効化
}

void CInstCh::bankSelLSB(uint8_t lsb)
{
    bankSelL_ = lsb;
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

void CInstCh::setRPNRegister(uint16_t reg, uint16_t val)
{
    switch (reg) {
    case 0x0000: setBendRange(static_cast<uint8_t>(val >> 7)); break;
    case 0x0001: setFineTune(val); break;
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
    // 旧 NRPN 定義を踏襲
    switch (reg) {
    case 0x3001: phyCh_ = static_cast<uint8_t>(val & 0x7F); break;
    default: break;
    }
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

void CInstCh::enterNote(int layerIdx, uint8_t devCh, uint8_t note, ISoundDevice* dev)
{
    // 空きスロットを探す
    for (auto& h : notes_) {
        if (!h.isValid()) {
            h.layerIdx = static_cast<uint8_t>(layerIdx);
            h.devCh    = devCh;
            h.note     = note;
            h.dev      = dev;
            ++timbres_;
            return;
        }
    }
    // 空きなし: 最も古いエントリを上書き (ポリフォニーオーバー)
    notes_[0].layerIdx = static_cast<uint8_t>(layerIdx);
    notes_[0].devCh    = devCh;
    notes_[0].note     = note;
    notes_[0].dev      = dev;
    FITOM_LOG_DEBUG("CInstCh ch=" << (int)ch_ << ": note history overflow");
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
    int16_t bendCents = static_cast<int16_t>(bendRange_)
                      * (static_cast<int16_t>(pitchBend_ >> 7) - 64);
    int16_t tuneCents = static_cast<int16_t>(tuning_ >> 7) - 64;
    int16_t fine = bendCents + tuneCents;

    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        h.dev->setNoteFine(h.devCh, h.note, fine);
    }
}

void CInstCh::applyLFOToAll()
{
    // HW LFO (Modulation) をすべての発音中チャンネルに適用
    bool pmOn = (pmDepth_ > 0);
    bool amOn = (amDepth_ > 0);
    for (int hi = 0; hi < MAX_NOTES; ++hi) {
        auto& h = notes_[hi];
        if (!h.isValid() || !h.dev) continue;
        h.dev->enablePM(h.devCh, pmOn);
        h.dev->enableAM(h.devCh, amOn);
        if (pmOn) { h.dev->setPMDepth(h.devCh, pmDepth_); h.dev->setPMRate(h.devCh, pmRate_); }
        if (amOn) { h.dev->setAMDepth(h.devCh, amDepth_); h.dev->setAMRate(h.devCh, amRate_); }
    }
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
        uint8_t devCh = 0xFF;
        if (li == 0 && dn.fixedCh >= 0) {
            // layer[0] のみ fixedCh を適用
            devCh = dev->assignCh(static_cast<uint8_t>(dn.fixedCh), this, patch, samplePatch);
        } else {
            devCh = dev->allocCh(this, patch, samplePatch);
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
        // (ドラムはトランスポーズより play_note の絶対指定が自然)
        int playNote = static_cast<int>(dn.playNote) + adj.pitch;
        playNote = std::clamp(playNote, 0, 127);
        int16_t fine = static_cast<int16_t>(dn.fineTune);
        dev->setNoteFine(devCh, static_cast<uint8_t>(playNote), fine, false);

        // SwPatch を VoiceProcessor に適用
        // (samplePatchのみ設定されている場合=AWM系レイヤーは、FmVoiceが
        //  FMオペレータ専用の構造体でありサンプルベース音源には適用できない
        //  ため、この処理自体をスキップする。patch==nullptrのまま
        //  patch->hwにアクセスするとクラッシュするため、必ずpatchの
        //  非nullptrチェックを先に行う)。
        if (patch && rp->swPatch) {
            const auto* sp = rp->swPatch;
            FmVoice fv;
            fv.hw  = patch->hw;
            for (int i = 0; i < 4; ++i) fv.hwOp[i] = patch->hwOp[i];
            fv.sw  = sp->sw;
            for (int i = 0; i < 4; ++i) fv.swOp[i] = sp->swOp[i];
            auto* st = dev->getChState(devCh);
            if (st) st->proc.onNoteOn(static_cast<uint8_t>(adjVol),
                                      127, vel, fv);
        }

        // ベロシティ (vol + NRPN)
        int adjVel = static_cast<int>(vel) + adj.vel;
        adjVel = std::clamp(adjVel, 1, 127);

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
    if (cache.patchBank == dn.patchBank &&
        cache.patchProg == dn.patchProg &&
        cache.isValid()) {
        return &cache.resolved;
    }
    // キャッシュミス → 解決
    cache.patchBank = dn.patchBank;
    cache.patchProg = dn.patchProg;
    auto& pm = fitom_->getPatchManager();
    cache.resolved = pm.resolve(dn.patchBank, dn.patchProg,
                                fitom_->getConfig());
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
            if (cache.resolved.swPatch) {
                fv.sw = cache.resolved.swPatch->sw;
                for (int i = 0; i < 4; ++i)
                    fv.swOp[i] = cache.resolved.swPatch->swOp[i];
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
    // GM2 Drum Per-Part NRPN
    uint8_t note = static_cast<uint8_t>(reg & 0x7F);
    if (note >= 128) return;
    switch (reg & 0xFF00) {
    case 0x1800: noteAdj_[note].pitch = static_cast<int16_t>(val) - 64; break;
    case 0x1A00: noteAdj_[note].vel   = static_cast<int16_t>(val) - 64; break;
    case 0x1C00: noteAdj_[note].pan   = static_cast<int16_t>(val) - 64; break;
    default: break;
    }
}

} // namespace fitom
