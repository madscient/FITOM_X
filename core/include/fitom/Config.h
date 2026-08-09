#pragma once
// fitom/Config.h  (改訂版)
// FITOMConfig — ISoundDevice を直接保持するよう拡張

#include "fitom/IPort.h"
#include "fitom/HWPort.h"
#include "fitom/PatchData.h"
#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/Sf2BankRegistry.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

namespace fitom {

class PatchManager; // 前方宣言 (loadDrumBanks の引数用)

class ISoundDevice;

// リニアステレオ化 (CLinearPanDevice) におけるL/R役割の宣言方法。
// docs/chip-driver-architecture.md「3.1 リニアステレオ化」参照。
enum class StereoSide : uint8_t {
    None = 0,  // 未指定 (プロファイルの stereo_pair:true)。IPort::getPanpot()
               // の 1(L)/2(R) から役割を判定する = プラグイン側の出力
               // ルーティングでL/Rを分離する従来方式。
    Left,      // stereo_pair:"L"。チップ自身のL/R出力ビットでL側に固定する
    Right,     // stereo_pair:"R"。同上、R側
};

// 「モノラル1ポート」または「ステレオペア2ポート」のどちらかを表す単位。
// 同種デバイス自動束ね (spanGroups) の各要素として使う。
struct PortGroup {
    std::shared_ptr<IPort> primary;
    std::shared_ptr<IPort> stereoPair;  // nullptr = モノラル単体
    // stereoPair が non-null のとき、そのペアがチップ内L/R分離方式
    // (stereo_pair:"L"/"R") かどうか。DeviceEntry::stereoPairChipLevel と同義。
    bool                     stereoPairChipLevel = false;
    // このポートグループの本来のdeviceType (DEVICE_*)。VoicePatchType が
    // 同じでも実装クラスが異なる場合がある(例: OPNB=COPNBはOPN2/OPNA=COPNA
    // と同じVOICE_PATCH_OPN2だが、ch0/ch3を無効化した別クラス)ため、
    // 代表デバイス(devices_[i].deviceType)を流用せず、このポートが元々
    // 属していたDeviceEntry自身のdeviceTypeを保持する
    // (CFITOM::initDevices()が各サブチップ生成時に参照する)。
    uint32_t                deviceType = 0;
    // このポートグループ本来のrhythm_mode。代表デバイス(devices_[i].
    // rhythmMode)を流用せず、このポートが元々属していたDeviceEntry
    // 自身の値を保持する(deviceTypeと同じ理由。2026年8月、OPL/OPLL系で
    // rhythm_mode:trueのチップが別のrhythm_mode:false chipとspanGroup
    // として束ねられた場合に、束ねられた側のch6-8無効化が代表デバイスの
    // 値で上書きされ効かなくなっていたバグの修正で追加)。
    bool                     rhythmMode = false;
};

struct DeviceEntry {
    std::string                    label;
    // shared_ptr: sub-device 自動生成 (例: OPNA→FM+SSG+ADPCMB) で
    // 複数の DeviceEntry が同一の物理/エミュレーターポートを共有するため。
    std::shared_ptr<IPort>         port;
    std::unique_ptr<ISoundDevice>  device;
    uint32_t                       deviceType  = 0;
    int                            sampleRate  = 44100;
    // B-2: 2ポートチップ用の2番目のポート（HW SPFM extra_slot）
    std::shared_ptr<IPort>         port2;        // nullptr = 1ポート
    int                            extraSlot   = -1; // -1 = 未使用
    // リズムモード (OPLL/OPL/OPL2/OPL3 等、チップ内蔵リズム音源を持つ
    // デバイス共通のオプション)。特定チップに限定しない汎用フィールド。
    bool                           rhythmMode  = false;
    // sub-device 自動生成で同一チップ指定から生成された兄弟エントリを
    // 識別するためのグループID (-1 = 単独デバイス)。同じ物理ポートを
    // 共有するデバイス群は同じ compositeGroup を持つ。
    int                            compositeGroup = -1;

    // ─── リニアステレオ化 (CLinearPanDevice) ────────────────────────────
    // プロファイルで stereo_pair:true が明示指定されたエントリ同士のうち、
    // 同一VoicePatchType・同一InterfaceDesc・Panpot(L=1,R=2)の組み合わせを
    // mergeStereoPairDevices() が検出し、代表エントリの stereoPairPort に
    // 相手側(R側)のポートを設定する。自動検出はせず、明示指定が無ければ
    // 発動しない。non-null なら「port(L) + stereoPairPort(R)」で
    // CLinearPanDevice を構成する。
    bool                            stereoPairRequested = false; // プロファイル指定
    std::shared_ptr<IPort>          stereoPairPort;               // 統合後に設定
    // L/R役割の宣言方法 (StereoSide参照)。None なら従来どおり
    // IPort::getPanpot() の 1/2 から判定する。
    StereoSide                      stereoSide = StereoSide::None;
    // 統合後に設定。true なら「チップ自身のL/R出力ビットで分離する」方式
    // (L/R両側とも stereo_pair:"L"/"R" で宣言された場合)。
    // CLinearPanDevice が束ねた2チップの ChState::panpot を左右端へ固定する。
    bool                            stereoPairChipLevel = false;

    // 同種デバイス自動束ね (CSpanDevice) 用の追加ポートグループ群。
    // 各要素は「モノラル1ポート」または「ステレオペア2ポート
    // (CLinearPanDeviceとして束ねられたユニット)」のいずれか。
    // 空なら単独デバイス。CFITOM::initDevices() がこの一覧を見て、
    // port(+stereoPairPort) と spanGroups それぞれに ISoundDevice を
    // 生成し CSpanDevice で束ねる。
    std::vector<PortGroup>          spanGroups;

    // SF2直行パス(docs/sf2-fluidsynth-integration.md参照)用のラッパー
    // デバイス(chip:"SF2")かどうか。deviceType は他の未知チップ文字列と
    // 同じくDEVICE_NONEになるが、これは意図したスキップであるため、
    // CFITOM::initDevices()が「deviceType unknown」警告を出さずに区別
    // できるようにするためのフラグ(2026年7月新設)。
    bool                            isSf2       = false;
};

class IPortFactory {
public:
    virtual ~IPortFactory() = default;
    virtual std::unique_ptr<IPort> createPort(const nlohmann::json& cfg) { return nullptr; }
};

class FITOMConfig {
public:
    explicit FITOMConfig(std::unique_ptr<IPortFactory> factory = nullptr);
    virtual ~FITOMConfig();

    bool loadSystemConf(const std::filesystem::path& path);
    bool loadProfile(const std::filesystem::path& path, PatchManager* patchMgr = nullptr);
    bool loadLegacyIni(const std::filesystem::path& path);

    // 現在のプロファイル状態をpathへ書き戻す(GUIのMIDIポート設定/
    // システム設定ダイアログのOK確定用、2026年7月新設)。loadProfile()で
    // 読み込んだJSON(profileJson_)をベースに、GUIから変更されうる
    // フィールド(midi_inputs/master_volume/master_pitch)のみ現在値で
    // 上書きする。devices/hw_plugins/banks等、他のフィールドはロード時の
    // 内容がそのまま維持される。loadProfile()を一度も呼んでいない場合
    // (profileJson_が空)は空のオブジェクトをベースに書き出す。
    bool saveProfile(const std::filesystem::path& path) const;

    // fitom.conf.json の log.* 設定を取り出す。loadSystemConf() 未実行、
    // または該当フィールドが省略されている場合は fallback を返す
    // (呼び出し側の従来デフォルト値をそのまま維持できるようにするため)。
    std::string getLogLevel(const std::string& fallback)   const;
    std::string getLogFile(const std::string& fallback)    const;
    bool        getLogConsole(bool fallback)                const;

    // HWプラグイン(実機/エミュレータ問わず、IHWPluginを実装するDLL)を
    // 複数登録できる。実機かエミュレータかはFITOM本体では区別しない。
    HWPluginRegistry& getHWPluginRegistry();

    int              getDeviceCount()              const;
    IPort*           getDevicePort(int index)      const;
    // B-2: 2ポートチップ (OPN2/OPNA/OPL3等) の2番目のポート。単一ポート
    // デバイスや未使用の場合は nullptr。
    IPort*           getDevicePort2(int index)     const;
    ISoundDevice*    getDevice(int index)          const;
    uint32_t         getDeviceType(int index)      const;
    // devices[i] のサンプルレート (旧audio_output.sample_rate相当。
    // 廃止に伴い、デバイスごとに保持していた値をそのまま使う。
    // HW経由の場合、実際の値はHWプラグイン側が管理するため、この値は
    // Fnumber計算等の目安として使われる)。
    int              getDeviceSampleRate(int index) const;
    // リズムモード (OPLL/OPL系等、チップ内蔵リズム音源の有効/無効)
    bool             getDeviceRhythmMode(int index) const;
    std::string      getDeviceLabel(int index)     const;
    // SF2直行パス用ラッパーデバイス(chip:"SF2")かどうか。
    bool             isSf2Device(int index)        const;

    // ─── SF2直行パス (docs/sf2-fluidsynth-integration.md参照、2026年7月新設) ───
    // banks.sf2_banks[]から構築されたレジストリ(CC#32解決・soundfonts一覧)。
    const Sf2BankRegistry& getSf2BankRegistry() const { return sf2Banks_; }

    // トップレベルsf2_channel_windows[]の静的設定一覧。CFITOM::init()が
    // これを初期状態として窓テーブルへ反映する。
    struct Sf2ChannelWindow {
        uint8_t mpu;
        uint8_t ch;
        uint8_t fluidsynthChan;
    };
    const std::vector<Sf2ChannelWindow>& getSf2ChannelWindows() const { return sf2ChannelWindows_; }

    // 同種デバイス自動束ね: このデバイスと束ねる追加ポートグループ数、
    // および k番目 (0-indexed) の追加ポートグループの主/ステレオペアポートを返す。
    int              getDeviceSpanGroupCount(int index) const;
    IPort*           getDeviceSpanGroupPrimary(int index, int k) const;
    IPort*           getDeviceSpanGroupStereoPair(int index, int k) const; // nullptr=モノラル
    // k番目の追加ポートグループ本来のdeviceType (DEVICE_*)。VoicePatchType
    // が代表デバイスと同じでも実装クラスが異なりうる(COPNB等)ため、
    // CFITOM::initDevices() はサブチップ生成時に代表のdeviceTypeではなく
    // こちらを使う。
    uint32_t         getDeviceSpanGroupDeviceType(int index, int k) const;
    // k番目の追加ポートグループ本来のrhythm_mode。代表デバイスと
    // rhythm_modeが異なりうる(OPL/OPLL系でrhythm_mode:trueのチップと
    // falseのチップが同一VoicePatchTypeでspanGroupとして束ねられる場合)
    // ため、CFITOM::initDevices() はサブチップ生成時に代表の
    // getDeviceRhythmMode(index)ではなくこちらを使う(2026年8月新設)。
    bool             getDeviceSpanGroupRhythmMode(int index, int k) const;
    // k番目の追加ポートグループがステレオペアの場合、それがチップ内L/R分離方式
    // (stereo_pair:"L"/"R") かどうか。getDeviceStereoPairChipLevel()と同じ意味。
    bool             getDeviceSpanGroupStereoPairChipLevel(int index, int k) const;

    // リニアステレオ化 (CLinearPanDevice): このデバイス自身がステレオペア化
    // されている場合、相手(R側)のポートを返す。nullptr = モノラル単体。
    IPort*           getDeviceStereoPairPort(int index) const;
    // 上記ステレオペアが「チップ自身のL/R出力ビットで分離する」方式
    // (stereo_pair:"L"/"R") かどうか。false ならプラグイン側の出力ルーティング
    // (pan:1/2) でL/Rを分離する従来方式。
    bool             getDeviceStereoPairChipLevel(int index) const;

    // ─── VoicePatchType (音色パッチ互換性分類) ──────────────────────────────
    // devices_[index] の deviceType (DEVICE_*) から対応する VoicePatchType
    // (VOICE_PATCH_* 、0x10〜0x74) を返す。未対応の場合は VOICE_PATCH_NONE(0)。
    uint8_t getVoicePatchType(int deviceIndex) const;

    // voicePatchType に一致する最初のデバイスのインデックスを返す。
    // 見つからない場合は -1。旧FITOMの CFITOMConfig::GetLogDeviceFromID 相当
    // (完全一致のみ、互換フォールバックは将来実装)。
    int findDeviceIndexByVoicePatchType(uint8_t voicePatchType) const;

    // 生のdeviceType(DEVICE_*)に一致する最初のデバイスのインデックスを
    // 返す。見つからない場合は-1。COPNARhythm/COPLLRhythm等、
    // deviceTypeToVoicePatchType()がVOICE_PATCH_NONEを返す(通常の
    // VoicePatchTypeベースルーティングでは到達できない)特殊デバイスを
    // 検索するために使う (PatchManager::resolveBuiltinRhythm()参照)。
    int findDeviceIndexByDeviceType(uint32_t deviceType) const;

    // sourceVoicePatchType(+HwPatchの内容)をフォールバックとして受け入れ
    // 可能な、接続済み全デバイスのインデックスをdevices[]の順序で列挙する。
    // (DeviceFactory::acceptsFallback()参照)。Program Change時の
    // findFallbackDeviceIndex()相当の判定を使うが、こちらは「最初の1件」
    // ではなく全候補を返す。DVA (発音時のチャンネル動的割り当て) 中に、
    // 一次候補デバイスの空きチャンネルが無い場合、他にハンドオフできる
    // デバイスがあるかを探すために使う (CInstCh::noteOn参照)。
    // AWM等HwPatchを持たないVoicePatchTypeでは使えない(空配列を返す)。
    std::vector<int> findAllFallbackDeviceIndices(uint8_t sourceVoicePatchType,
                                                    const HwPatch& patch) const;

    // deviceType (DEVICE_*) → VoicePatchType (VOICE_PATCH_*) の静的変換。
    // インスタンス状態に依存しないため static。
    static uint8_t deviceTypeToVoicePatchType(uint32_t deviceType) noexcept;

    // VoicePatchType (VOICE_PATCH_*) → VoiceGroup (HwBankRegistry検索キー) の静的変換。
    static uint32_t voicePatchTypeToVoiceGroup(uint8_t voicePatchType) noexcept;

    // プロファイル/バンクJSONの "group" 文字列 → VoicePatchType の静的変換。
    // 対応する文字列が無い場合は VOICE_PATCH_NONE(0) を返す。
    static uint8_t stringToVoicePatchType(const std::string& s) noexcept;

    // stringToVoicePatchType() の逆変換(ログ表示用)。同関数が複数の別名を
    // 受け付ける値(OPN2/OPNA/OPNB等)については、profile.schema.json の
    // enum に載っている代表名のみを返す。未知の値は "?" を返す。
    static const char* voicePatchTypeToString(uint8_t voicePatchType) noexcept;

    // ─── Sub-device 自動生成 (composite chip) ──────────────────────────────
    // 1つの物理/エミュレーターチップ指定 (例: "OPNA") から、内部的に複数の
    // ISoundDevice インスタンス (例: FM本体 + SSG + ADPCM-B) を自動生成する。
    // 各サブデバイスは同一の物理ポートを共有するが、独立した devices_[] の
    // エントリとして登録され、それぞれ別の VoicePatchType で識別可能になる。
    struct SubDeviceSpec {
        uint32_t    deviceType;    // このサブデバイスの DEVICE_*
        const char* labelSuffix;   // ラベルに付与する接尾辞 (例: "-SSG")
        bool        usesExtraPort; // 2ポート目 (extraPort) を必要とするか
        bool        rhythmCapable; // rhythm_mode をこのサブデバイスに適用するか
    };

    // baseDeviceType (プロファイルの "chip" から解決した DEVICE_*) が
    // 複数サブデバイスへの展開を必要とするかどうか。
    // 展開が必要なら true を返し、outSpec に構成一覧を書き込む。
    // 展開不要 (単独デバイスのまま) なら false を返す。
    // rhythmModeFromProfile: そのデバイスインスタンスの"rhythm_mode"設定値。
    // OPL系/OPLL系は内蔵リズムがFM本体側のch6-8とハードウェアレジスタを
    // 共有するため、trueの場合のみ`DEVICE_OPL_RHY`/`DEVICE_OPLL_RHY`
    // サブデバイスをoutSpecに追加する(falseならFM本体のみのまま=ch6-8を
    // 通常の楽音chとして使う。2026年7月、ビルトインリズムと通常楽音chを
    // またいだDVAは未実装のため、両者を同時に有効化しないようにする形で
    // 修正。同一チップ種別の複数インスタンス中、rhythm_mode:trueが1つも
    // 無ければリズムサブデバイス自体が生成されない)。OPNA系の内蔵リズムは
    // FM本体と完全に独立したレジスタ空間を持つためこの制約の対象外で、
    // rhythmModeFromProfileの値に関わらず常に生成する。
    static bool resolveCompositeSpec(uint32_t baseDeviceType, bool rhythmModeFromProfile,
                                      std::vector<SubDeviceSpec>& outSpec);

    // composite展開されたサブデバイスが、リニアステレオ化
    // (CLinearPanDevice、プロファイルの stereo_pair:true) の対象になりうるか。
    // false を返すのは、チャンネルごとのパンポットをハードウェアで持っており
    // L/R2台の束ねによる代替が不要かつ有害なサブデバイス(現状はOPL4の
    // AWM/PCM部 = DEVICE_OPL4AWM のみ)。false の場合、その物理チップの
    // 他のサブデバイス(OPL4ならFM部)がステレオ化されても、当該サブデバイスは
    // 独立したモノラルデバイスとして devices_ に残る。
    static bool subDeviceAcceptsStereoPair(uint32_t deviceType) noexcept;

    // このデバイスのドライバが、チャンネルごとのパンポットをチップの
    // レジスタへどう書けるか。各ドライバの updatePanpot() 実装が真の情報源で、
    // ここはその deviceType 側の索引にあたる(updatePanpot() を実装/変更したら
    // 必ずこの分類も見直すこと)。リニアステレオ化の方式判定
    // (deviceHasChipLevelPanpot()) と、チャンネルレベルメーターのL/R分割表示
    // (CFITOM::getPhysicalChipChannelStates()) の両方がこれを参照する。
    enum class ChipPanType {
        Mono,        // 左右の作り分け不可 (OPLL系・OPL/OPL2・SSG/PSG・YM2203等)
        ThreeWay,    // L/Rイネーブルビットのみ (L / R / 両方の3値)
        Continuous,  // 連続的なパンポットレジスタ (OPL4 AWM・YMZ280B・SAA1099等)
    };
    static ChipPanType getChipPanType(uint32_t deviceType) noexcept;

    // このデバイスのドライバが、チャンネルごとのL/R出力ビット(またはそれと
    // 等価な左右独立音量)をチップのレジスタへ書けるか。true なら
    // stereo_pair:"L"/"R" のチップ内L/R分離方式が実効性を持つ。
    // false のチップ(モノラル出力しか持たないもの)では updatePanpot() が
    // no-op のため、"L"/"R" を指定しても左右に分離されない
    // (プラグイン側の pan:1/2 による従来方式が必要)。
    static bool deviceHasChipLevelPanpot(uint32_t deviceType) noexcept {
        return getChipPanType(deviceType) != ChipPanType::Mono;
    }

    // ─── リニアステレオ化 (CLinearPanDevice) のペアリング本体 ────────────
    // mergeStereoPairDevices() の実装本体。devices_ の状態のみに依存する
    // 純粋な変換のため、staticにしてユニットテストから直接呼べるようにして
    // いる (resolveCompositeSpec と同じ方針)。仕様は
    // mergeStereoPairDevices() の宣言部コメント参照。
    static void pairStereoDevices(std::vector<DeviceEntry>& devices);

    int                getMidiInputCount()          const;
    const std::string& getMidiInputName(int index)  const;
    // 実行中にMIDI入力ポートの割り当てを丸ごと差し替える(GUIのMIDIポート
    // 設定ダイアログ用、2026年7月新設)。namesの各要素がMPU 0,1,2...に
    // 対応する。空文字列の要素は「未設定」を表す。呼び出し側(FITOMBridge)
    // が実際のMIDIポートの開閉を行う責務を持ち、ここではConfigが保持する
    // 名前一覧を更新するのみ。
    void               setMidiInputNames(const std::vector<std::string>& names);
    // MIDIバックエンドDLLのパス (profile の midi_backend.dll)。
    // 未指定なら空文字列 (呼び出し側がプラットフォーム既定を使う)。
    const std::string& getMidiBackendDll()          const { return midiBackendDll_; }

    void    setMasterVolume(uint8_t vol);
    uint8_t getMasterVolume()   const;
    double  getMasterPitch()    const { return masterPitch_; }
    void    setMasterPitch(double p)  { masterPitch_ = p; }
    // PSG系共有バンクのフォールバック先(2026年7月新設)。
    uint8_t getPsgFallbackChip() const { return psgFallbackChip_; }

    using ProgressCb = std::function<void(const std::string&)>;
    void setProgressCallback(ProgressCb cb) { progressCb_ = std::move(cb); }

protected:
    virtual bool buildFromProfile(const nlohmann::json& j, PatchManager* patchMgr = nullptr,
                                   const std::filesystem::path& baseDir = {});
    virtual bool buildFromLegacyIni(const nlohmann::json& ini);
    virtual void buildDevice(const nlohmann::json& dev);

    // buildDevice() の FMENGINE/HW 両ブランチ共通処理。
    // composite chip (resolveCompositeSpec が true を返す場合) なら
    // 複数の DeviceEntry を、そうでなければ単独の DeviceEntry を devices_ に追加する。
    void pushDeviceEntries(const std::string& baseLabel, uint32_t baseDeviceType,
                            std::shared_ptr<IPort> port, std::shared_ptr<IPort> port2,
                            int sampleRate, int extraSlot, bool rhythmModeFromProfile,
                            bool stereoPairRequested = false,
                            StereoSide stereoSide = StereoSide::None);
    int nextCompositeGroupId_ = 0;

    // 【Step1: 最初に実行】全 buildDevice() 完了後に1回呼ぶ。プロファイルで
    // stereo_pair が明示指定されたエントリ同士のうち、同一deviceType・
    // 同一InterfaceDesc・L/R役割の組み合わせを検出し、L側エントリの
    // stereoPairPort に R側のポートを設定した上で R側エントリを devices_ から
    // 削除する。自動検出はせず、両エントリとも明示指定が無ければ発動しない。
    // L/R役割は stereo_pair:"L"/"R" (DeviceEntry::stereoSide) の明示指定を
    // 優先し、stereo_pair:true (StereoSide::None) なら Panpot(L=1,R=2) から
    // 判定する。両側とも明示指定だった場合のみ stereoPairChipLevel を立てる。
    // composite展開されたチップ(OPLL+内蔵リズム、OPNA+SSG+ADPCM等)は、
    // グループ内の「stereo_pair対象サブデバイス」(上記
    // subDeviceAcceptsStereoPair()参照)のdeviceTypeが1対1で対応する場合に限り
    // compositeGroup単位でまとめてペアリングする(2026年8月対応。それ以前は
    // composite展開対象のチップではstereo_pairが無視されていた)。
    void mergeStereoPairDevices();

    // 【Step2: Step1の後に実行】全 buildDevice() 完了後に1回呼ぶ。同一
    // VoicePatchType・同一 IPort::getInterfaceDesc() を持つ
    // (かつ compositeGroup が異なる = 別の物理ポートに由来する) エントリ群を
    // 検出し、代表エントリ1つに統合する (他は devices_ から削除し、
    // 代表エントリの spanGroups に追加する)。stereoPairPort が設定済みの
    // エントリ (Step1でステレオ化済み) は Panpot をグループ化キーから除外する
    // (ステレオユニットはもはや単一のL/Rパンという概念を持たないため)。
    void mergeSpannableDevices();
    // banks.drum_banks[] を PatchManager に登録する。引数banksは
    // resolveBanksSection()(Config.cpp)で解決済みの実体を渡すこと
    // (banksが外部参照文字列の場合、参照先ファイルの内容に展開済み)。
    void loadDrumBanks(const nlohmann::json& banks, PatchManager& pm,
                        const std::filesystem::path& baseDir);

    // ─── SF2直行パス (2026年7月新設) ──────────────────────────────────────
    // banks.sf2_banks[]をパースしsf2Banks_を構築する。devices[]のビルド
    // (buildDevice()、chip:"SF2"のparams_json組み立て)より前に、
    // buildFromProfile()の先頭付近で1回だけ呼ぶ(Sf2BankRegistryが
    // soundfonts一覧を確定させるため)。PatchManagerに依存しないため
    // patchMgr==nullptrでも常に実行する。引数banksはloadDrumBanks同様、
    // resolveBanksSection()で解決済みの実体を渡すこと。
    void loadSf2Banks(const nlohmann::json& banks, const std::filesystem::path& baseDir);
    // トップレベルsf2_channel_windows[]をパースし、sf2ChannelWindows_を
    // 構築する。fluidsynth_chanの重複・エントリ数(<=16)を検証し、違反時は
    // 空にしてfalseを返す(呼び出し元はプロファイル読み込み全体を失敗させる)。
    bool loadSf2ChannelWindows(const nlohmann::json& j);

    virtual void validateProfile();
    virtual void loadLegacyManualDevices(const nlohmann::json& ini);

    HWPluginRegistry hwPluginRegistry_;

    std::vector<DeviceEntry>     devices_;
    std::vector<std::string>     midiInputNames_;
    std::string                  midiBackendDll_;

    // PSG系共有バンク(voice_patch_type=0x40固定)における、HwPatch側の
    // targetVoicePatchTypeが未設定(0)の場合のフォールバック先。
    // プロファイルのpsg_fallback_chipから設定される(2026年7月新設、
    // 省略時はVOICE_PATCH_SSG)。
    uint8_t     psgFallbackChip_ = VOICE_PATCH_SSG;

    uint8_t     masterVolume_    = 100;
    double      masterPitch_     = 440.0;

    // ─── SF2直行パス (2026年7月新設) ──────────────────────────────────────
    Sf2BankRegistry               sf2Banks_;
    std::vector<Sf2ChannelWindow> sf2ChannelWindows_;

    nlohmann::json systemConf_;
    nlohmann::json profileJson_;

    std::unique_ptr<IPortFactory> factory_;
    ProgressCb progressCb_;
};

} // namespace fitom
