// fitom/gui/bridge/FITOMBridge.h
// GUI → FITOM コア ブリッジ(UIフレームワーク非依存)
//
// GUI側コード(apps/fitom_gui 等)は、このブリッジを経由してのみ
// コアに触れる。特定のUIツールキット(Dear ImGui/Qt/MFC等)への
// 依存は一切持たない。コアへの直接アクセスは一切しない。

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// ─── デバイス情報 (GUI 表示用) ─────────────────────────────────────────────
struct FITOMDeviceInfo {
    int         index;
    std::string label;
    std::string descriptor;  // "OPN (YM2203) 3ch" 等
    uint32_t    deviceType;
    int         chCount;
    bool        isEmulator;
};

// ─── MIDI 入力デバイス情報 ──────────────────────────────────────────────────
struct FITOMMidiInfo {
    int         index;
    std::string name;
};

// ─── パッチ情報 (GUI の音色一覧用) ────────────────────────────────────────
struct FITOMPatchInfo {
    int         bank;
    int         prog;
    std::string name;
    int         layerCount;
};

// ─── ブリッジクラス ─────────────────────────────────────────────────────────
class FITOMBridge {
public:
    // シングルトン
    static FITOMBridge& instance();

    // ─── 初期化 ──────────────────────────────────────────────────────────
    // systemConfPath: fitom.conf.json のパス
    // profilePath:    起動時に読み込むプロファイルのパス
    bool init(const std::string& systemConfPath,
              const std::string& profilePath);
    void exit();

    // ─── プロファイル切り替え ────────────────────────────────────────────
    bool loadProfile(const std::string& path);
    std::string currentProfilePath() const;

    // ─── デバイス情報 ────────────────────────────────────────────────────
    std::vector<FITOMDeviceInfo> getDevices() const;
    std::vector<FITOMMidiInfo>   getMidiInputs() const;

    // ─── パッチ一覧 ──────────────────────────────────────────────────────
    std::vector<FITOMPatchInfo> getPatches(int bankNo) const;
    std::vector<std::string>    getPatchBankNames() const;

    // ─── 音色エディタ連携 ────────────────────────────────────────────────
    // HwPatch を JSON 文字列として取得 / 設定
    std::string getHwPatchJson(int bankNo, int prog) const;
    bool        setHwPatchJson(int bankNo, int prog, const std::string& json);
    std::string getSwPatchJson(int bankNo, int prog) const;
    bool        setSwPatchJson(int bankNo, int prog, const std::string& json);

    // ─── マスターボリューム ──────────────────────────────────────────────
    void    setMasterVolume(uint8_t vol);
    uint8_t getMasterVolume() const;

    // ─── マスターピッチ ──────────────────────────────────────────────────
    void   setMasterPitch(double hz);
    double getMasterPitch() const;

    // ─── 直接操作 ────────────────────────────────────────────────────────
    void allNoteOff();
    void resetAllCtrl();

    // ─── タイマーコールバック (MFC タイマーから呼ぶ) ───────────────────
    void onTimer(uint32_t tick);

    // ─── ステータスコールバック (GUI ステータスバー更新用) ──────────────
    using StatusCb = std::function<void(const std::string&)>;
    void setStatusCallback(StatusCb cb);

    // ─── オーディオデバイス列挙 (エミュレーター用) ─────────────────────
    std::vector<std::string> enumerateAudioDevices() const;

    // ─── バンクファイル I/O ──────────────────────────────────────────────
    bool loadHwBankFile(const std::string& path, int bankNo);
    bool saveHwBankFile(const std::string& path, int bankNo) const;
    bool loadPatchBankFile(const std::string& path, int bankNo);
    bool savePatchBankFile(const std::string& path, int bankNo) const;

private:
    FITOMBridge() = default;
    ~FITOMBridge() = default;
    FITOMBridge(const FITOMBridge&) = delete;
    FITOMBridge& operator=(const FITOMBridge&) = delete;

    bool initialized_ = false;
    std::string currentProfile_;
    StatusCb statusCb_;
};
