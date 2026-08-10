#pragma once
// fitom/CFITOM.h
// FITOM コアシングルトン — モダナイズ版
//
// 旧 FITOM.h からの変更:
//   - boost::thread → std::thread / std::mutex
//   - FMVOICE* → HwPatch* / PatchManager
//   - CPort* / CSoundDevice* → fitom::IPort* / fitom::ISoundDevice*
//   - TCHAR → std::string
//   - CMidiInst → MidiProcessor
//   - MFCダイアログ依存を排除

#include "fitom/FITOMdefine.h"
#include "fitom/Config.h"
#include "fitom/HWPort.h"
#include "fitom/PatchData.h"
#include "fitom/PatchManager.h"
#include "fitom/MidiManager.h"
#include "fitom/ISoundDevice.h"
#include "fitom/MidiCh.h"
#include "fitom/DrumData.h"
#include "fitom/Log.h"
#include "fitom/IPort.h"
#include "fitom/DeviceFactory.h"

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

namespace fitom {


// ================================================================
//  MidiProcessor: MIDI バイト列 → CMidiCh の MIDI 処理エンジン
//  旧 CMidiInst を std::thread ベースでリプレース
// ================================================================
class MidiProcessor {
public:
    // mpuIndex: SF2直行パス(docs/sf2-fluidsynth-integration.md参照)の窓
    // テーブルが対象とするMPU番号(0-3)。窓の対象にならない系統
    // (内部用MIDIパイプ等)は既定値-1のままにし、SF2直行パスの判定自体を
    // 行わない(2026年7月新設)。
    MidiProcessor(std::array<std::unique_ptr<IMidiCh>, 16>& channels,
                  CFITOM* parent, bool clockEnabled, int mpuIndex = -1);
    ~MidiProcessor() = default;

    // MidiManager から受け取った生バイトを処理する
    // コールバックスレッドから呼ばれる
    void receiveByte(const uint8_t* data, size_t len, uint64_t timestampNs);

    void timerCallback(uint32_t tick);
    void pollingCallback();
    void midiClockCallback(uint32_t tick);

    IMidiCh* getChannel(uint8_t ch) {
        return (ch < 16) ? channels_[ch].get() : nullptr;
    }

    // GM2規格: Bank Select MSB(CC#0)=DEVICE_RHYTHM(0x78)受信でリズムチャンネルに、
    // 0以外かつDEVICE_RHYTHM以外の値受信でメロディチャンネルに、そのチャンネルの
    // 役割を動的に切り替える (旧FITOMのCMidiInst::Control()と同じロジック)。
    // 既存の発音は全て停止してからチャンネルオブジェクト自体を差し替える。
    void switchChannelRole(uint8_t ch, bool toRhythm);

    // GUI等から、生MIDIバイト列(receiveByte())を経由せず、構造化された
    // 値で直接コントロールチェンジ/プログラムチェンジを送出する
    // (2026年7月新設)。receiveByte()側のランニングステータス状態機械を
    // 一切変更しないため、実際のMIDI入力と混在させても安全。
    // processControl()と同じ経路(CC#0特殊値によるリズム/メロディ
    // 動的切替を含む)を通るため、MIDI受信時と全く同じ挙動になる。
    // (mpuIndex_, ch)がSF2直行パスの窓に含まれる場合は、processMessage()
    // と同様routeSf2ChannelMessage()へ振り分ける(2026年8月新設。CH設定
    // ダイアログのSF2パッチピッカーがCC#32/Prog.chg/Note On/Offをこの
    // 経路で送るため、実MIDI入力と同じ挙動に揃える必要がある)。CFITOM
    // (parent_)がこの時点では前方宣言のみで未完成のため、定義はCFITOM.cpp
    // 側(CFITOM本体の定義より後ろ)に置く。
    void sendControlChange(uint8_t ch, uint8_t cc, uint8_t val);
    void sendProgramChange(uint8_t ch, uint8_t prog);
    // GUI等からの音色試聴用(2026年7月新設、パッチピッカーのプログラム
    // 選択時のプレビュー再生)。processMessage()のNote On/Off分岐と同じ
    // 呼び出しをそのまま行う。
    void sendNoteOn(uint8_t ch, uint8_t note, uint8_t vel);
    void sendNoteOff(uint8_t ch, uint8_t note);

private:
    std::array<std::unique_ptr<IMidiCh>, 16>& channels_;
    CFITOM* parent_;
    bool clockEnabled_;
    int mpuIndex_;

    // MIDI 状態機械
    enum class State { Ready, Wait1, Wait2, SysEx } state_ = State::Ready;
    uint8_t msgBuf_[4] = {};
    uint8_t msgPt_     = 0;
    uint8_t sysexBuf_[8192] = {};
    uint16_t sysexPt_  = 0;
    uint16_t currentStatus_ = 0;

    void processMessage();
    void processSysEx();
    // プライベート(メーカー固有)SysEx。manufacturer ID = 00H 48H 01H
    // (拡張ID形式、3バイト)。将来実装用のスタブ。processSysEx()から
    // manufacturer ID一致時に呼ばれる。呼び出し時点でsysexBuf_[0..2]が
    // 00H 48H 01Hであることが確定しており、sysexBuf_[3]以降が
    // メーカー固有のペイロード。
    void processPrivateSysEx();
    void processControl(uint8_t ch, uint8_t cc, uint8_t val);
    void processRPN(uint8_t ch);
    void processNRPN(uint8_t ch);
    // CC#98/99/100/101受信時: 保持中のデータエントリー値を破棄する。
    void selectRpnParam(uint8_t ch);
    // CC#6/CC#38受信時: 現在のMSB/LSBを14bit値へ合成して適用する。
    void applyDataEntry(uint8_t ch, IMidiCh* midicch);

    // RPN / NRPN ステート
    //
    // データエントリーはCC#6(MSB)→CC#38(LSB)の順に送られるのが通常の
    // MIDI送信順のため、CC#38受信時にも「MSBと合成した14bit値」で
    // 再適用する必要がある(CC#6受信時にしか適用しないと、LSBが常に
    // 1メッセージ分遅れて次のCC#6に紛れ込む)。そのためMSBも保持する。
    //
    // msb/lsbはパラメータ番号(CC#98/99/100/101)を選び直した時点で
    // クリアする。クリアしないと、直前に別のパラメータへ書いた
    // データエントリーの残骸が次のパラメータへ混入する。
    // msbReceived_は「現在のreg選択後にCC#6を受け取ったか」。
    // CC#38単独(MSB未受信)では適用しない — MSBを0とみなして適用すると
    // 中央値0x2000が前提のRPN#1等でピッチが飛ぶため。
    struct RpnState {
        uint16_t reg  = 0x3FFF;
        uint8_t  msb  = 0;
        uint8_t  lsb  = 0;
        bool     msbReceived = false;
        bool     isNrpn = false;
    };
    std::array<RpnState, 16> rpn_;
};

// ================================================================
//  PhysicalChipSubDevice: 物理チップ1個を構成するサブデバイス1個分
//  (チャンネルレベルメーター用、2026年7月新設)
//
//  サブデバイス自動生成(例: OPNA→FM+SSG+ADPCM-B、OPL3→4OP+2OP)で、
//  1つの物理チップは複数の論理ISoundDeviceの集まりとして実装される。
//  各要素がそのうちの1つを表す(表示順は生成順=devices[]の記述順)。
// ================================================================
struct PhysicalChipSubDevice {
    ISoundDevice* device     = nullptr;
    uint32_t      deviceType = 0;   // このサブデバイス自身のdeviceType (DEVICE_*)
    uint8_t       chCount    = 0;   // このサブデバイスのチャンネル数
};

// ================================================================
//  PhysicalChipInfo: レジスタダンプモニター用、物理チップ単位の情報
//  (2026年7月新設)
//
//  サブデバイス自動生成(例: OPNA→FM+SSG+ADPCM-B+Rhythm)で複数の論理
//  ISoundDeviceが同一物理チップ(同一HWPort)を共有する場合も1エントリに
//  まとめる。「どのHWPortの組が1つの物理チップか」は、FITOMConfigが
//  保持するport/port2/stereoPairPort/spanGroupsの各ポートポインタの
//  同一性から判定する(プロファイルのdevices[]/hw_plugins[]の記述内容が
//  そのまま反映される。CFITOM::buildPhysicalChipList()参照)。
// ================================================================
struct PhysicalChipInfo {
    std::string label;             // 代表デバイスのラベル(プロファイルのlabel)
    uint32_t    deviceType = 0;    // 代表デバイスのdeviceType (DEVICE_*)
    // hwifのparams_json(HWPlugin_Open()に渡した値)から組み立てた、実際の
    // 物理/エミュレーターチップの識別名(例: "SPFM_TOWER COM3 slot0"、
    // "YMEngine/OPNA")。labelとは異なりFITOM_X側で自由に付けられるもの
    // ではなく、hwif接続情報そのものに由来する(HWPort::getPhysicalChipName()参照)。
    std::string physicalName;
    HWPort*     port  = nullptr;   // 1ポート目 (レジスタダンプの0x000-0x0FF)
    HWPort*     port2 = nullptr;   // 2ポート目 (0x100-0x1FF)。nullptr = 1ポートチップ
    // レジスタダンプの表示サイズ [byte] (16byte境界)。実チップが実際に
    // 使うレジスタ空間に合わせるため、getDeviceRegSize()(deviceType別の
    // レジスタ空間サイズ)とgetHighBankOffset()(高位バンクへのオフセット量)
    // を併用して決める(buildPhysicalChipList()参照)。0x100より小さいチップ
    // (OPLLの0x40等)も、0x100を超えるチップ(OPNA/OPN2/OPL3)も等しく扱う。
    // port2がある場合(分離した物理ポート2つ)のみ常に0x200固定。
    // 同一物理ポートを共有する複数サブデバイスのうち、最後に登録される
    // ものが最も高位のバンクを使うとは限らない(例: OPL4はFM部
    // [DEVICE_OPL3、0x000-0x1FF]より後にAWM部[DEVICE_OPL4AWM、
    // 0x200-0x2FF]が登録される)ため、登録順に関わらず全サブデバイスの
    // 必要サイズの最大値を採用する。
    uint32_t    dumpSize = 0;
    // チャンネルレベルメーター用のサブデバイス内訳。initDevices()が
    // span/stereo展開“前”に記録したpendingSubDevices_を、buildPhysicalChipList()
    // が物理ポート単位に突き合わせて構築する(2026年7月)。同種デバイス
    // 自動束ね(spanGroups)で他デバイスに吸収されたデバイスも、吸収先とは
    // 別の物理チップとして正しく内訳が復元される。リニアステレオ化
    // (CLinearPanDevice)されたペアも、L側・R側それぞれの物理チップに
    // 各自の内訳が入る(2026年8月対応)。
    std::vector<PhysicalChipSubDevice> subDevices;
};

// ================================================================
//  PhysicalChipChannelState: チャンネルレベルメーター用、1チャンネル分の
//  現在の発音状態(2026年7月新設)。
//
//  FITOM_Xは音声合成を行わないため実際の音量信号は存在しない。
//  発音中か否か(ChState::isActive())とベロシティ(ChState::velocity)を
//  組み合わせた疑似メーターとして扱う(既存のGUI鍵盤ビューの発光
//  エフェクトと同じ考え方)。
// ================================================================
struct PhysicalChipChannelState {
    bool    sounding = false;
    uint8_t velocity = 0;
    // false = このチャンネルは実チップ上恒常的に無効(例: COPNBのch0/ch3
    // [実機に存在しない]、OPL/OPLL系リズムモード時のch6-8[リズム専用に
    // 転用され通常のFM割り当て対象外])。ChState::isEnabled()由来
    // (2026年7月新設。チャンネルレベルメーターで非活性表示に使う)。
    bool    enabled  = true;
    // ChState::noteOnSeq由来。ノートオンのたびに単調増加する。sounding/
    // velocityだけでは同一ベロシティでの再トリガー(ボイススチール)を
    // GUI側が検出できないため、フレーム間差分による「本当にノートオンが
    // 起きたか」の判定に使う(2026年7月新設)。
    uint32_t noteOnSeq = 0;

    // ─── ステレオ定位 (2026年8月新設) ────────────────────────────────────
    // stereo = true なら、このチャンネルは左右の作り分けができる。
    // GUIはバー1本を左右half-widthの2本に分割し、それぞれ gainL/gainR を
    // 掛けた高さで描く。false のチャンネル(モノラル出力チップ)は従来どおり
    // 1本のバーで描く。
    //
    // gainL/gainR は「定位」を表す係数 (0.0-1.0) で、大きい側が必ず1.0に
    // なるよう正規化されている。バーの高さそのもの(音量・ベロシティ由来の
    // エンベロープ)はGUI側が別途持っており、それに乗算する使い方を想定して
    // いるため、ここで音量を二重に反映させない。
    //   中央 → (1.0, 1.0) / 左端 → (1.0, 0.0) / 右端 → (0.0, 1.0)
    bool  stereo = false;
    float gainL  = 1.0f;
    float gainR  = 1.0f;
};

// ================================================================
//  CFITOM: コアシングルトン
// ================================================================
class CFITOM {
public:
    // シングルトンアクセス
    static CFITOM& instance() {
        static CFITOM inst;
        return inst;
    }

    // ─── 初期化・終了 ──────────────────────────────────────────────
    // config: FITOMConfig 所有権を移譲する
    int  init(std::unique_ptr<FITOMConfig> config,
              std::unique_ptr<PatchManager> patchMgr);
    void exit(bool save = false);

    // ─── デバイスアクセス ─────────────────────────────────────────
    ISoundDevice* getDevice(int index) const;
    int           getDeviceCount() const;
    // dev の devices[] 内インデックスを逆引きする (モニタリング用、
    // CRhythmChはISoundDevice*のみ保持しdeviceIndexを持たないため)。
    // 見つからなければ -1。
    int           findDeviceIndex(const ISoundDevice* dev) const;

    // CRhythmCh::NoteOn から呼ばれるドラムノート解決 (bankNo = CC#0 値)
    const DrumNote* getDrum(int bankNo, uint8_t prog, uint8_t midiNote) const;

    // ─── 物理チップ列挙 (レジスタダンプモニター用、2026年7月新設) ─────────
    int getPhysicalChipCount() const { return static_cast<int>(physicalChips_.size()); }
    const PhysicalChipInfo* getPhysicalChipInfo(int index) const {
        return (index >= 0 && index < static_cast<int>(physicalChips_.size()))
            ? &physicalChips_[index] : nullptr;
    }
    // 指定物理チップの現在のレジスタ値を生バイト列で返す。1ポートなら
    // 0x00-0xFF(256byte)、2ポートなら0x000-0x1FF(512byte、port2のレジスタを
    // 0x100番地以降にpackする)。実チップからの読み出しAPIは存在しないため、
    // FITOM_Xが最後に書き込んだ値をそのまま返す(HWPort::getShadowRegRange参照)。
    // indexが範囲外の場合は空配列を返す。
    std::vector<uint8_t> getPhysicalChipRegisterDump(int index) const;

    // 指定物理チップの全チャンネル分の現在の発音状態を、subDevices内の
    // 並び順(サブデバイスの生成順→サブデバイス内はチャンネル番号順)で
    // 返す。要素数は該当物理チップの全サブデバイスのchCount合計と一致する。
    // indexが範囲外の場合は空配列を返す。
    std::vector<PhysicalChipChannelState> getPhysicalChipChannelStates(int index) const;

    // 指定deviceType(サブデバイス自身のdeviceType、PhysicalChipSubDevice::
    // deviceType)のチャンネルch(0始まり)の表示名を返す(チャンネルレベル
    // メーターのラベル用)。通常は「接頭辞+(ch+1)」形式(例: DEVICE_SSG→
    // "SSG1"、DEVICE_OPL3→"Q1"、DEVICE_OPL4AWM→"A1")。内蔵リズム音源
    // (DEVICE_OPNA_RHY/DEVICE_OPL_RHY/DEVICE_OPLL_RHY)だけは、chが実機の
    // 固定パート(打楽器)に一対一対応するため番号ではなく楽器名の省略表記
    // ("BD"/"SD"/"CY"/"HH"/"Tom"/"Rim")を返す。未登録のdeviceTypeは
    // "CH"+(ch+1)を返す(汎用フォールバック)。
    static std::string getSubDeviceChannelName(uint32_t deviceType, int ch);

    // ─── 論理チップ単位のチャンネル状態 (チャンネルレベルメーターの
    //     物理/論理表示切替用、2026年7月新設) ──────────────────────────
    // getPhysicalChipChannelStates()が「同一物理ポートを共有する全サブ
    // デバイスをまとめた」状態を返すのに対し、こちらはdevices[]の1エントリ
    // (=1論理デバイス、サブデバイス自動生成で分割された場合は分割後の
    // 1つ)単独のチャンネル状態をそのまま返す。deviceIndexはgetDevice()/
    // getDeviceCount()と同じ添字(既存のgetDevices()の列挙と一致)。
    // 範囲外・デバイス無しの場合は空配列を返す。
    std::vector<PhysicalChipChannelState> getLogicalDeviceChannelStates(int deviceIndex) const;

    FITOMConfig& getConfig() const { return *config_; }
    PatchManager& getPatchManager() { return *patchMgr_; }

    // ─── MIDIインスタンスアクセス ─────────────────────────────────
    MidiProcessor* getMidiProcessor(uint8_t idx) {
        return (idx < MAX_MPUS && processors_[idx]) ? processors_[idx].get() : nullptr;
    }
    // MPU(16chの処理単位)の総数。GUI等、外部から件数を知る必要がある
    //箇所向け(2026年7月新設)。
    static constexpr int getMpuCount() { return MAX_MPUS; }

    // 内部用MIDIパイプ(backends/midi_pipe)専用の1系統(16ch)へアクセスする。
    // MAX_MPUS本のMPU(getMidiProcessor()/getMpuCount())とは完全に独立して
    // おり、GUIのMIDIモニター・MIDIポート設定ダイアログ等、実MIDI入力ポート
    // 前提のUIには意図的に一切現れない。init()で常に生成するため、実際に
    // このパイプへ外部プロセスが接続するかどうか(=アプリ層がbackends/
    // midi_pipeをロードするかどうか)とは無関係にnullptrにはならない。
    MidiProcessor* getInternalPipeProcessor() { return internalPipeProcessor_.get(); }

    // MidiInPortのコールバック(バックエンドDLL固有のスレッド上で呼ばれる)
    // から、指定MPUのMidiProcessor::receiveByte()へprocessMutex_のロックを
    // 取った上で中継する(2026年8月新設)。timerCallback()等、他の全ての
    // processors_[]/channels_[]アクセス経路は既にprocessMutex_でロック
    // されているが、実MIDI入力の生バイト列だけがreceiveByte()を未ロックで
    // 直接呼んでおり、タイマースレッド(CFITOM::timerCallback()、
    // CRhythmCh::timerCallback()がnoteSlots_を反復処理中)とのデータ競合で
    // LayerSlot::devが反復処理中にnullptrへクリアされ、nullptr参照クラッシュ
    // する不具合があった。GUI等からの構造化メッセージ送出
    // (sendChannelControlChange()等、直上参照)と同じロック方針に統一する。
    void receiveMpuByte(uint8_t mpuIndex, const uint8_t* data, size_t len, uint64_t timestampNs) {
        std::lock_guard<std::mutex> lk(processMutex_);
        if (mpuIndex < MAX_MPUS && processors_[mpuIndex]) processors_[mpuIndex]->receiveByte(data, len, timestampNs);
    }
    // 内部用MIDIパイプ(getInternalPipeProcessor())専用の同経路。
    void receiveInternalPipeByte(const uint8_t* data, size_t len, uint64_t timestampNs) {
        std::lock_guard<std::mutex> lk(processMutex_);
        if (internalPipeProcessor_) internalPipeProcessor_->receiveByte(data, len, timestampNs);
    }

    // GUI等から、指定MPU/chへ直接コントロールチェンジ/プログラムチェンジを
    // 送出する(2026年7月新設、GUIのCH設定ダイアログ用)。timerCallback()
    // 等と同じprocessMutex_でロックする(MidiProcessor::sendControlChange/
    // sendProgramChange参照)。
    void sendChannelControlChange(uint8_t mpuIndex, uint8_t ch, uint8_t cc, uint8_t val) {
        std::lock_guard<std::mutex> lk(processMutex_);
        if (mpuIndex < MAX_MPUS && processors_[mpuIndex]) processors_[mpuIndex]->sendControlChange(ch, cc, val);
    }
    void sendChannelProgramChange(uint8_t mpuIndex, uint8_t ch, uint8_t prog) {
        std::lock_guard<std::mutex> lk(processMutex_);
        if (mpuIndex < MAX_MPUS && processors_[mpuIndex]) processors_[mpuIndex]->sendProgramChange(ch, prog);
    }
    // GUI等からの音色試聴用(2026年7月新設)。
    void sendChannelNoteOn(uint8_t mpuIndex, uint8_t ch, uint8_t note, uint8_t vel) {
        std::lock_guard<std::mutex> lk(processMutex_);
        if (mpuIndex < MAX_MPUS && processors_[mpuIndex]) processors_[mpuIndex]->sendNoteOn(ch, note, vel);
    }
    void sendChannelNoteOff(uint8_t mpuIndex, uint8_t ch, uint8_t note) {
        std::lock_guard<std::mutex> lk(processMutex_);
        if (mpuIndex < MAX_MPUS && processors_[mpuIndex]) processors_[mpuIndex]->sendNoteOff(ch, note);
    }

    // ─── SF2直行パス (docs/sf2-fluidsynth-integration.md参照、2026年7月新設) ───
    // MidiProcessor::processMessage()から、生MIDI入力の(mpu, ch)が窓
    // テーブルに含まれるかどうかの判定・実際の転送処理を委譲される。
    // 窓に含まれていれば、CInstCh/CRhythmChへのディスパッチを行わず true
    // を返す(呼び出し元は以降の通常処理をスキップする)。含まれていなければ
    // false を返し、呼び出し元は通常どおりchannels_[ch]へディスパッチする。
    // status: MIDIステータスバイト(0x80-0xEF)。d1/d2: データバイト
    // (1byteメッセージの場合d2は無視される)。
    bool routeSf2ChannelMessage(int mpu, uint8_t ch, uint8_t status, uint8_t d1, uint8_t d2);

    // プライベートSysEx sub-cmd 0x04(F0 00 48 01 04 <mpu> <ch>
    // <fluidsynth_chan|0x7F> F7)による窓の動的割り当て/解除。
    // MidiProcessor::processPrivateSysEx()から呼ばれる。
    void setSf2ChannelWindow(uint8_t mpu, uint8_t ch, uint8_t fluidsynthChanOr7F);

    // MIDIモニター表示用、指定(mpu,ch)の現在のSF2直行パス窓の状態を
    // 読み取り専用スナップショットで返す(2026年8月新設、FITOMBridge::
    // getChannelMonitors()から呼ばれる)。assigned==falseの場合はその
    // (mpu,ch)が窓に含まれていないことを意味し、他フィールドは無視すること。
    struct Sf2WindowInfo {
        bool    assigned         = false;
        uint8_t fluidsynthChan   = 0;
        bool    bankResolved     = false;
        int     soundfontIndex   = 0;
        int     sf2Bank          = 0;
        int     lastCc32Bank     = -1;  // 最後に解決に成功したCC#32の生値(sf2_banks[].bank相当)
        int     lastProg         = -1;
        bool    hasLastNoteOn    = false;
        uint8_t lastNoteOnStatus = 0;
        uint8_t lastNoteOnNote   = 0;
        uint8_t lastNoteOnVel    = 0;
    };
    Sf2WindowInfo getSf2ChannelWindowInfo(uint8_t mpu, uint8_t ch) const {
        if (mpu >= MAX_MPUS || ch >= 16) return {};
        const Sf2WindowState& w = sf2Windows_[mpu][ch];
        Sf2WindowInfo info;
        info.assigned         = w.assigned;
        info.fluidsynthChan   = w.fluidsynthChan;
        info.bankResolved     = w.bankResolved;
        info.soundfontIndex   = w.soundfontIndex;
        info.sf2Bank          = w.sf2Bank;
        info.lastCc32Bank     = w.lastCc32Bank;
        info.lastProg         = w.lastProg;
        info.hasLastNoteOn    = w.hasLastNoteOn;
        info.lastNoteOnStatus = w.lastNoteOnStatus;
        info.lastNoteOnNote   = w.lastNoteOnNote;
        info.lastNoteOnVel    = w.lastNoteOnVel;
        return info;
    }

    // CH設定ダイアログ用(2026年8月新設): 現在fluid_synth_chanとして
    // 使用中の(mpu, ch, fluidsynthChan)一覧を返す。窓の新規割り当て時に
    // 未使用のchanをGUI側が提案できるようにするため
    // (setSf2ChannelWindow()自体も重複割り当てを拒否するが、GUIでは
    // 送信前に候補を絞り込みたい)。
    struct Sf2WindowAssignment {
        uint8_t mpu;
        uint8_t ch;
        uint8_t fluidsynthChan;
    };
    std::vector<Sf2WindowAssignment> listAssignedSf2Windows() const {
        std::vector<Sf2WindowAssignment> out;
        for (uint8_t p = 0; p < MAX_MPUS; ++p) {
            for (uint8_t c = 0; c < 16; ++c) {
                if (sf2Windows_[p][c].assigned) {
                    out.push_back(Sf2WindowAssignment{p, c, sf2Windows_[p][c].fluidsynthChan});
                }
            }
        }
        return out;
    }

    // ─── タイマー・ポーリング ────────────────────────────────────
    void timerCallback(uint32_t tick);
    int  pollingCallback();
    void midiClockCallback();

    // ─── グローバル操作 ───────────────────────────────────────────
    void allNoteOff();
    void resetAllCtrl();
    void setMasterVolume(uint8_t vol);

    // マスターピッチを設定する (430〜450Hz, デフォルト 440Hz)
    // 呼び出し後、全デバイスの発音中チャンネルの F-number が即時更新される。
    void setMasterPitch(double pitchHz);
    double getMasterPitch() const;
    uint8_t getMasterVolume() const;

    // ─── Universal SysEx: マスターチューニング/スケールオクターブチューニング ───
    // setMasterPitch()(config由来の絶対Hz基準)とは別に、SysEx由来の相対
    // オフセットを保持する。実効マスターピッチ = 基準Hz × 2^((fineCents/100
    // + coarseSemitones)/12) として合成し、FnumRegistryに反映する。
    void setMasterFineTune(int16_t cents);       // Universal RT 04/03
    void setMasterCoarseTune(int8_t semitones);  // Universal RT 04/04
    int16_t getMasterFineTuneCents() const     { return masterFineTuneCents_; }
    int8_t  getMasterCoarseTuneSemitones() const { return masterCoarseTuneSemitones_; }

    // Scale/Octave Tuning (MIDI Tuning Standard, Universal RT 08/08 1byte形式)。
    // 半音(C,C#,D...B、12音)ごとのセントオフセット。全オクターブに一律適用。
    // ノートオン時、各CInstCh/CRhythmChがgetScaleTuningCents(note)を
    // fine計算に加算する (CInstCh::applyPitchBendToAll参照)。
    void setScaleTuning(const std::array<int8_t, 12>& table);
    int8_t getScaleTuningCents(uint8_t note) const { return scaleTuning_[note % 12]; }

    // ─── 静的ユーティリティ (旧 CFITOM static メソッド) ─────────
    static uint32_t         getDeviceVoiceType(uint32_t deviceId);
    static uint32_t         getDeviceVoiceGroupMask(uint32_t deviceId);
    static uint32_t         getDeviceIdFromName(const std::string& name);
    static const std::string getDeviceNameFromId(uint32_t deviceId);
    static uint32_t         getDeviceRegSize(uint32_t deviceId);

    // ─── コールバック登録 ────────────────────────────────────────
    using StatusCallback = std::function<void(const std::string&)>;
    void setStatusCallback(StatusCallback cb) { statusCb_ = std::move(cb); }

    // ─── スレッド制御 ────────────────────────────────────────────
    // タイマースレッドを開始する（MFC以外の環境用）
    // MFC環境ではタイマーはシステムから呼ばれるため不要
    void startTimerThread(uint32_t intervalMs = 1);
    void stopTimerThread();

private:
    CFITOM() = default;
    ~CFITOM() { exit(); }
    CFITOM(const CFITOM&) = delete;
    CFITOM& operator=(const CFITOM&) = delete;

    static constexpr int MAX_MPUS = 4;

    // ─── 所有リソース ─────────────────────────────────────────────
    std::unique_ptr<FITOMConfig>   config_;
    std::unique_ptr<PatchManager>  patchMgr_;
    std::unique_ptr<MidiManager>   midiMgr_;

    // MIDI プロセッサとチャンネル (入力ポートごと)
    std::array<std::array<std::unique_ptr<IMidiCh>, 16>, MAX_MPUS> channels_;
    std::array<std::unique_ptr<MidiProcessor>, MAX_MPUS> processors_;

    // ─── 内部用MIDIパイプ専用チャンネル (MAX_MPUSとは完全に独立、2026年7月新設) ───
    // プロファイルのmidi_backend/midi_inputs設定を一切関知せず、init()で
    // 常に生成する(実際に外部プロセスがbackends/midi_pipe経由で接続
    // するかどうかはアプリ層[FITOMBridge/fitom_cli]の責務)。
    std::array<std::unique_ptr<IMidiCh>, 16> internalPipeChannels_;
    std::unique_ptr<MidiProcessor>           internalPipeProcessor_;

    // ─── デバイスリスト (CFITOM が ISoundDevice を所有) ──────────
    // Config は IPort を所有し、CFITOM は ISoundDevice を所有する
    std::vector<std::unique_ptr<ISoundDevice>> devices_;

    // Universal SysEx: マスターチューニング/スケールオクターブチューニング
    // (setMasterPitchの絶対Hz基準とは別の、相対オフセット)
    int16_t masterFineTuneCents_       = 0;
    int8_t  masterCoarseTuneSemitones_ = 0;
    std::array<int8_t, 12> scaleTuning_ = {}; // 半音(C,C#,D...B)ごとのcentsオフセット
    void updateEffectiveMasterPitch();        // config基準Hz×相対オフセットを合成

    // ─── SplitPort の寿命管理 ─────────────────────────────────────
    // OPNA/OPN2 等の HW 2 ポート構成時に生成する SplitPort。
    // IPort* として CSoundDevice に渡すが、所有権はここで管理する。
    std::vector<std::unique_ptr<SplitPort>> splitPorts_;

    // ─── OffsetPort の寿命管理 (2026年7月新設) ─────────────────────
    // ADPCM-A(YM2610/2610B)・ADPCM-B(YM2608=OPNA)は実チップ上
    // 「port2」(SplitPort/OffsetPortでアドレス0x100以降にマップされる側)
    // に配置されるレジスタ体系のため(ADPCM-B(YM2610/2610B)は逆に低位
    // ポートのままで正しく対象外)、resolveHighBankPort()がinitDevices()
    // 内でこのデバイスのportを高位ポート側へ差し替える。プロファイルの
    // extra_port等で明示された物理ポートが無い場合、ここでOffsetPortを
    // 自前生成して所有する(ユーザー指摘により発覚: 以前はSSG等と同じ
    // 低位ポートにそのまま割り当てており、レジスタアドレスが衝突していた)。
    // 同様にDEVICE_OPL4AWM(OPL4のAWM/PCM部)は実チップ上FM部の2バンク
    // (port1/port2、アドレス0x000-0x1FF)とは独立した3つ目のレジスタ
    // バンク(アドレス0x200以降)に配置されるため、0x200オフセットで
    // 同じ仕組みに乗せる(2026年7月、ユーザー指摘により発覚: 以前はFM部と
    // 同じ低位ポートにそのまま割り当てておりレジスタアドレスが衝突していた)。
    std::vector<std::unique_ptr<OffsetPort>> offsetPorts_;
    // deviceTypeが高位バンクへオフセットされるべきなら、そのオフセット量
    // (0x100/0x200等)を返す。resolveHighBankPort()と
    // buildPhysicalChipList()(レジスタダンプモニターの表示サイズ算出)の
    // 双方から参照される単一の真実の情報源(2026年7月新設)。
    static uint16_t getHighBankOffset(uint32_t deviceType);
    IPort* resolveHighBankPort(uint32_t deviceType, IPort* port, IPort* configuredPort2);

    // ─── SF2直行パス (docs/sf2-fluidsynth-integration.md参照、2026年7月新設) ───
    // chip:"SF2"のdevices[]エントリのIPort(HWPort)。initDevices()が
    // FITOMConfig::isSf2Device()で見つけて記録する。プロファイル検証
    // (FITOMConfig::buildFromProfile())により、devices[]内にchip:"SF2"は
    // 高々1つしか存在しないことが保証されている。nullptr = SF2ラッパー
    // デバイス未接続(窓は割り当てられていてもメッセージは単純に読み捨てる)。
    IPort* sf2Port_ = nullptr;

    // MPU×ch単位の窓テーブル。fluid_synth_tのchanは複数MPU間で共有できない
    // 状態そのものを持つため(⑤節参照)、明示的な対応表として管理する。
    struct Sf2WindowState {
        bool    assigned       = false; // この(mpu,ch)は現在窓に含まれるか
        uint8_t fluidsynthChan = 0;     // 割り当て先fluidsynth chan(0-15)。assigned時のみ有効
        // MIDIモニター表示専用のキャッシュ(2026年8月新設)。SF2直行パスは
        // 状態を持たない「CH変換付きMIDIパッチベイ」に徹する設計だが
        // (⑤節)、GUI表示のためだけに「最後に送出した値」を保持することは
        // レジスタダンプモニターのシャドウレジスタと同じ考え方であり、
        // ノート所有権の追跡・クリーンアップとは無関係(=設計方針に反しない)。
        int     lastCc32Bank   = -1;    // 最後に解決に成功したCC#32の生値(GUIのCH設定ダイアログ表示用)
        int     lastProg       = -1;    // 最後にsub-cmd 0x05で送出したprog(-1=未送出)
        bool    hasLastNoteOn  = false; // Note On(velocity>0)を一度でも送出したか
        uint8_t lastNoteOnStatus = 0;   // 送出した生ステータスバイト(0x90|fluidsynthChan)
        uint8_t lastNoteOnNote   = 0;
        uint8_t lastNoteOnVel    = 0;
        // CC#32解決結果のキャッシュ(CC#0を除く生MIDI転送とは別に、
        // 次のプログラムチェンジと組み合わせてsub-cmd 0x05を構築するために
        // 必要な最小限の状態。有効なCC#32が一度も解決されていない間は
        // プログラムチェンジ自体を読み捨てる、docs④節参照)。
        bool    bankResolved   = false;
        int     soundfontIndex = 0;
        int     sf2Bank        = 0;
    };
    std::array<std::array<Sf2WindowState, 16>, MAX_MPUS> sf2Windows_;

    // 同種デバイス自動束ね (CSpanDevice) で生成される個々のサブチップ。
    // devices_[i] が CSpanDevice の場合、その内部で束ねられる実体
    // (unique_ptr<ISoundDevice>) をここで保持し続ける必要がある
    // (CSpanDevice/CMultiDevice は生ポインタしか持たないため)。
    std::vector<std::unique_ptr<ISoundDevice>> spanSubChips_;

    // 物理チップ単位の一覧 (レジスタダンプモニター用、2026年7月新設)。
    // buildPhysicalChipList() が initDevices() の最後に構築する。
    // HWPort自体の所有権はConfig側(shared_ptr<IPort>)にあるため、
    // ここでは生ポインタのみを保持する。
    std::vector<PhysicalChipInfo> physicalChips_;
    void buildPhysicalChipList();

    // チャンネルレベルメーター用、buildPhysicalChipList()がsubDevicesを
    // 組み立てる元データ(2026年7月新設)。initDevices()のデバイス構築
    // ループが、devices_[i]がCSpanDevice/CLinearPanDeviceへラップされる
    // “前”の、単一物理ポートに対応する生のISoundDeviceを都度ここへ記録する
    // (同種デバイス自動束ねでspanGroupsへ吸収されたデバイスも、devices_[]
    // からは辿れなくなるが、ここには独立して記録が残る)。
    struct PendingSubDevice {
        HWPort*       port       = nullptr; // buildPhysicalChipList()のdedupキーと同じ生ポート
        ISoundDevice* device     = nullptr;
        uint32_t      deviceType = 0;
    };
    std::vector<PendingSubDevice> pendingSubDevices_;
    void registerPendingSubDevice(HWPort* port, ISoundDevice* device, uint32_t deviceType);

    // 1つの物理ポート(+extraPort)から ISoundDevice を生成する。
    // stereoPairPort が指定されていれば、そのポート用にもう1つ生成し、
    // CLinearPanDevice でラップしてステレオデバイスとして返す
    // (旧FITOM CLinearPan 相当)。生成した中間チップの寿命は spanSubChips_ で管理する。
    // stereoPairChipLevel: そのステレオペアがチップ内L/R分離方式
    // (プロファイルの stereo_pair:"L"/"R") かどうか。MultiDevice.h の
    // CLinearPanDevice のコメント参照。
    // outLeftRaw/outRightRaw: ステレオペア時、CLinearPanDeviceへラップする
    // 前のL側/R側の生ISoundDeviceを返す(所有権はspanSubChips_側にあるため
    // 生ポインタ)。チャンネルレベルメーターが物理チップ単位のch構成を
    // 取り出すのに使う。モノラル時はどちらも変更しない(戻り値のunique_ptr
    // 自身が唯一のデバイスであり、呼び出し元が .get() で取得できるため)。
    std::unique_ptr<ISoundDevice> createLeveledDevice(
        uint32_t deviceType, IPort* port, IPort* stereoPairPort,
        int sampleRate, IPort* extraPort, bool rhythmMode,
        bool stereoPairChipLevel = false,
        ISoundDevice** outLeftRaw = nullptr, ISoundDevice** outRightRaw = nullptr);

    // ─── タイマースレッド ─────────────────────────────────────────
    std::thread         timerThread_;
    std::atomic<bool>   timerRunning_{false};
    bool                exited_ = false; // exit()の冪等性ガード(二重exit防止)
    uint32_t            timerTick_ = 0;

    // ─── 排他制御 ────────────────────────────────────────────────
    // プロセス間排他: boost::interprocess::named_mutex を使用
    // (std::mutex はプロセス間不可のため Boost のまま維持)
    mutable std::mutex  processMutex_;

    // ─── コールバック ─────────────────────────────────────────────
    StatusCallback statusCb_;

    // 内部ヘルパー
    int  setupMidiInputs();
    void initDevices();
    void syncDeviceLatency();  // initDevices() から呼ばれる
    void timerThreadFunc(uint32_t intervalMs);
};

} // namespace fitom
