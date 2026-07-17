#pragma once
// fitom/Config.h  (改訂版)
// FITOMConfig — ISoundDevice を直接保持するよう拡張

#include "fitom/IPort.h"
#include "fitom/HWPort.h"
#include "fitom/PatchData.h"
#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"

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

// 「モノラル1ポート」または「ステレオペア2ポート」のどちらかを表す単位。
// 同種デバイス自動束ね (spanGroups) の各要素として使う。
struct PortGroup {
    std::shared_ptr<IPort> primary;
    std::shared_ptr<IPort> stereoPair;  // nullptr = モノラル単体
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

    // 同種デバイス自動束ね (CSpanDevice) 用の追加ポートグループ群。
    // 各要素は「モノラル1ポート」または「ステレオペア2ポート
    // (CLinearPanDeviceとして束ねられたユニット)」のいずれか。
    // 空なら単独デバイス。CFITOM::initDevices() がこの一覧を見て、
    // port(+stereoPairPort) と spanGroups それぞれに ISoundDevice を
    // 生成し CSpanDevice で束ねる。
    std::vector<PortGroup>          spanGroups;
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

    // 同種デバイス自動束ね: このデバイスと束ねる追加ポートグループ数、
    // および k番目 (0-indexed) の追加ポートグループの主/ステレオペアポートを返す。
    int              getDeviceSpanGroupCount(int index) const;
    IPort*           getDeviceSpanGroupPrimary(int index, int k) const;
    IPort*           getDeviceSpanGroupStereoPair(int index, int k) const; // nullptr=モノラル

    // リニアステレオ化 (CLinearPanDevice): このデバイス自身がステレオペア化
    // されている場合、相手(R側)のポートを返す。nullptr = モノラル単体。
    IPort*           getDeviceStereoPairPort(int index) const;

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
    static bool resolveCompositeSpec(uint32_t baseDeviceType,
                                      std::vector<SubDeviceSpec>& outSpec);

    int                getMidiInputCount()          const;
    const std::string& getMidiInputName(int index)  const;
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
                            bool stereoPairRequested = false);
    int nextCompositeGroupId_ = 0;

    // 全 buildDevice() 完了後に1回呼ぶ。同一 VoicePatchType・同一
    // IPort::getInterfaceDesc()・同一 IPort::getPanpot() を持つ
    // (かつ compositeGroup が異なる = 別の物理ポートに由来する) エントリ群を
    // 【Step1: 最初に実行】プロファイルで stereo_pair:true が明示指定された
    // エントリ同士のうち、同一VoicePatchType・同一InterfaceDesc・
    // Panpot(L=1,R=2)の組み合わせを検出し、L側エントリの stereoPairPort に
    // R側のポートを設定した上で R側エントリを devices_ から削除する。
    // 自動検出はせず、両エントリとも明示指定が無ければ発動しない。
    void mergeStereoPairDevices();

    // 【Step2: Step1の後に実行】全 buildDevice() 完了後に1回呼ぶ。同一
    // VoicePatchType・同一 IPort::getInterfaceDesc() を持つ
    // (かつ compositeGroup が異なる = 別の物理ポートに由来する) エントリ群を
    // 検出し、代表エントリ1つに統合する (他は devices_ から削除し、
    // 代表エントリの spanGroups に追加する)。stereoPairPort が設定済みの
    // エントリ (Step1でステレオ化済み) は Panpot をグループ化キーから除外する
    // (ステレオユニットはもはや単一のL/Rパンという概念を持たないため)。
    void mergeSpannableDevices();
    // profile の banks.drum_banks[] を PatchManager に登録する
    void loadDrumBanks(const nlohmann::json& j, PatchManager& pm,
                        const std::filesystem::path& baseDir);
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

    nlohmann::json systemConf_;
    nlohmann::json profileJson_;

    std::unique_ptr<IPortFactory> factory_;
    ProgressCb progressCb_;
};

} // namespace fitom
