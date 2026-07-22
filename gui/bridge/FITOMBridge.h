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
#include <memory>

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

// ─── バンク情報 (GUI のパッチピッカー、CC#32階層の選択肢用) ────────────────
struct FITOMBankInfo {
    int         bankNo;
    std::string name;
};

// ─── 物理チップ情報 (レジスタダンプモニター用、2026年7月新設) ──────────────
// サブデバイス自動生成(OPNA→FM+SSG+ADPCM-B等、同一物理チップを共有)や
// 同種デバイス自動束ね(spanGroups)・リニアステレオ化(stereoPairPort)で
// 生成される複数の論理デバイスは、物理チップ単位で1エントリにまとめられる
// (CFITOM::buildPhysicalChipList()参照)。
struct FITOMChipInfo {
    int         index;
    std::string label;         // 代表デバイスのラベル(プロファイルのlabel)
    // hwif接続情報(HWPlugin_Open()のparams_json)から組み立てた物理チップ名
    // (例: "SPFM_TOWER COM3 slot0"、"YMEngine/OPNA")。FITOM_X内部の
    // チップドライバ分類(deviceType)ではなく、実際に接続されている
    // 物理/エミュレーターチップそのものの識別名。
    std::string physicalName;
    bool        twoPort;       // true = レジスタダンプは0x000-0x1FF、false = 0x00-0xFF
};

// ─── 発音中ノート情報 (キーボードビューのポリフォニー表示用) ──────────────
struct FITOMActiveNote {
    uint8_t note;
    uint8_t velocity;
};

// ─── チャンネルモニター情報 (MIDIモニター画面用) ──────────────────────────
// Bank/Prog/Volumeは発音の有無に関わらず常に現在値を反映する
// (CC#0/#32、プログラムチェンジ受信のたびに更新される)。
// Note以降は、そのチャンネルが現在発音中の場合のみ意味を持つ
// (sounding==falseの間は直前の発音内容が残っている可能性があるため、
//  GUI側はsoundingを見て表示を切り替えること)。
struct FITOMChannelMonitor {
    int         ch = 0;              // 0-15
    bool        isRhythm = false;    // GM2リズムチャンネルかどうか

    int         bankNo = 0;
    int         progNo = 0;
    std::string bankName;            // 解決できない場合は空文字
    std::string progName;            // 同上
    uint8_t     volume = 0;
    uint8_t     expression = 0;
    uint8_t     panpot = 0;

    bool        sounding = false;    // 現在発音中か
    uint8_t     lastNote = 0xFF;     // 0xFF=無し。発音中の場合、直近に
                                      // 鳴らしたノート(グリッド内は
                                      // 後発優先になる)
    uint8_t     velocity = 0;
    std::string noteName;            // "C4"等。lastNote==0xFFの場合は空文字
    std::string deviceName;          // 解決できない場合は空文字
    int         deviceIndex = -1;
    uint8_t     fnumBlock = 0;
    uint16_t    fnum = 0;

    // 現在発音中の全ノート(キーボードビューでの同時発光表示用)。
    // Note/Fnumber等の他フィールドとは異なり、lastNoteだけでなく
    // 同時に鳴っている全ノートを含む。soundingがfalseの間は空。
    std::vector<FITOMActiveNote> activeNotes;
};

// ─── チャンネル設定情報 (CH設定ダイアログの初期値取得用) ────────────────────
// getChannelMonitors()は毎フレーム全16ch分呼ばれるホットパスのため、
// ダイアログを開いた時にだけ呼ぶ、この専用の軽量な取得関数を分ける
// (2026年7月新設)。
struct FITOMChannelSettings {
    uint8_t  volume     = 100;
    uint8_t  expression = 127;
    uint8_t  panpot     = 64;
    bool     isRhythm   = false;
    bool     monoMode   = false;
    uint8_t  bankSelMSB = 0;   // CC#0 (0=通常モード, 1-0x6F=直接デバイス選択モード)
    uint16_t bankNo     = 0;   // getBankNo()相当 (CC#32が意味する値)
    uint8_t  progNo     = 0;
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
    // 現在のプロファイル状態(MIDIポート設定/マスターボリューム・
    // マスターピッチ)を、現在ロード中のプロファイルファイルへ書き戻す
    // (MIDIポート設定ダイアログ/システム設定ダイアログのOK確定用、
    // 2026年7月新設)。currentProfilePath()が空(プロファイル未指定で起動)
    // の場合は何もせずfalseを返す。
    bool saveCurrentProfile();

    // ─── デバイス情報 ────────────────────────────────────────────────────
    std::vector<FITOMDeviceInfo> getDevices() const;

    // ─── レジスタダンプモニター (2026年7月新設) ────────────────────────
    // 物理チップ単位の一覧。サブデバイス自動生成等で複数の論理デバイスが
    // 同一物理チップを共有する場合も1エントリにまとまる。
    std::vector<FITOMChipInfo> getHwChips() const;
    // 指定物理チップの現在のレジスタ値を生バイト列で返す。1ポートなら
    // 0x00-0xFF(256byte)、2ポートなら0x000-0x1FF(512byte、port2を0x100番地
    // 以降にpack)。実チップからの読み出しAPIは存在しないため、FITOM_Xが
    // 最後に書き込んだ値をそのまま返す。chipIndexが範囲外の場合は空配列。
    std::vector<uint8_t> getHwChipRegisterDump(int chipIndex) const;
    // MPU(getMpuCount()、現状4)分を常に返す(indexはMPU番号と一致)。
    // 未割り当てのMPUはnameが空文字列になる(2026年7月、MIDIポート設定
    // ダイアログ対応のため「設定済みの件数分だけ返す」旧仕様から変更)。
    std::vector<FITOMMidiInfo>   getMidiInputs() const;

    // ─── MIDIポート設定 (MIDIモニターのポート名クリックで開くダイアログ用、
    //     2026年7月新設) ──────────────────────────────────────────────────
    // システムが現在列挙するMIDI入力ポート名の一覧。MIDIバックエンドDLLが
    // 未ロード(プロファイルのmidi_inputsが元々0件だった等)の場合はここで
    // 遅延ロードするため非const。ロード自体に失敗した場合は空配列を返す。
    std::vector<std::string> getAvailableMidiInputPorts();
    // MPU 0..getMpuCount()-1 に対応する、現在のMIDI入力ポート割り当て名
    // (空文字列=未設定)を常にgetMpuCount()件で返す。
    std::vector<std::string> getMidiInputPortAssignments() const;
    // MIDI入力ポートの割り当てを丸ごと差し替える。既存の全ポートを一旦
    // 閉じたうえで、namesで指定された名前のポートを開き直す(即時反映)。
    // 併せてConfig側の設定(現在のプロファイル状態)も更新する。namesの
    // 要素数はgetMpuCount()に満たなくてもよい(残りは未設定として扱う)。
    // 重複チェック等のバリデーションは呼び出し側(GUI)の責務とする。
    void setMidiInputPorts(const std::vector<std::string>& names);

    // ─── MIDIモニター (MPU=16chの処理単位。現状最大4面) ────────────────
    int getMpuCount() const;
    // mpuIndexが範囲外の場合は空配列を返す。常に16要素(ch0-15)。
    std::vector<FITOMChannelMonitor> getChannelMonitors(int mpuIndex) const;

    // 指定チャンネルの現在設定値(CH設定ダイアログの初期値用、
    // 2026年7月新設)。範囲外・未初期化の場合は既定値のFITOMChannelSettingsを返す。
    FITOMChannelSettings getChannelSettings(int mpuIndex, int ch) const;

    // ─── MIDI送信 (CH設定ダイアログのOKで使う、2026年7月新設) ────────────
    // 生バイト列(receiveByte())を経由せず、コアのMIDI処理経路
    // (MidiProcessor::processControl/IMidiCh::progChange)を直接呼ぶ。
    // MIDI受信時と全く同じ挙動になる(CC#0特殊値によるリズム/メロディ
    // 動的切替等も含む)。
    void sendControlChange(int mpuIndex, int ch, uint8_t cc, uint8_t val);
    void sendProgramChange(int mpuIndex, int ch, uint8_t prog);
    // パッチピッカーの音色試聴用(2026年7月新設)。
    void sendNoteOn(int mpuIndex, int ch, uint8_t note, uint8_t vel);
    void sendNoteOff(int mpuIndex, int ch, uint8_t note);

    // ─── パッチ一覧 ──────────────────────────────────────────────────────
    std::vector<FITOMPatchInfo> getPatches(int bankNo) const;
    std::vector<std::string>    getPatchBankNames() const;

    // ─── パッチピッカー用階層列挙 (2026年7月新設) ────────────────────────
    // 通常モード(CC#0=0)のCC#32階層: 登録済みPatchBank番号一覧
    std::vector<FITOMBankInfo>  getPatchBankList() const;
    // 直接デバイス選択モードのCC#32階層: 指定VoicePatchType用HwBank番号一覧
    std::vector<FITOMBankInfo>  getHwBankList(uint8_t voicePatchType) const;
    // 直接デバイス選択モードのProg.chg階層: 指定HwBank内の有効パッチ一覧
    std::vector<FITOMPatchInfo> getHwBankPatches(uint8_t voicePatchType, int hwBank) const;
    // リズムチャンネルのProg.chg階層(バンク0固定、CC#0/#32は無関係): 有効なドラムキット一覧
    std::vector<FITOMPatchInfo> getDrumPatches() const;

    // ─── 音色エディタ連携 ────────────────────────────────────────────────
    // HwPatch を JSON 文字列として取得 / 設定
    std::string getHwPatchJson(int bankNo, int prog) const;
    bool        setHwPatchJson(int bankNo, int prog, const std::string& json);
    std::string getSwPatchJson(int bankNo, int prog) const;
    bool        setSwPatchJson(int bankNo, int prog, const std::string& json);

    // 指定チャンネル(mpuIndex/ch)が現在選択している(CC#0/#32/プログラム
    // チェンジの値、未受信なら初期値=PatchBank0/Prog0扱い)HwPatchに
    // ついて、その元になった *.hwbank.json のファイルパスとprog番号を
    // 解決する。発音履歴(ノートオン)には依存しない。外部パッチエディタ
    // (FITOM_patch_editor)をキオスクモードで起動する際の引数として使う
    // 想定。解決できない場合(リズムチャンネル、AWM等HwBankを使わない
    // 音色、対応するHwBankが見つからない等)はfalseを返す。
    bool resolveChannelHwPatch(int mpuIndex, int ch,
                                std::string& outHwBankFile, int& outProgNo) const;

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
    FITOMBridge();
    ~FITOMBridge();
    FITOMBridge(const FITOMBridge&) = delete;
    FITOMBridge& operator=(const FITOMBridge&) = delete;

    bool initialized_ = false;
    std::string currentProfile_;
    StatusCb statusCb_;

    // MIDI入力の実体(MidiPluginInstance/MidiInPort)は、このヘッダを
    // UIフレームワーク非依存・コア型非依存に保つため、PIMPLで隠蔽する
    // (定義はFITOMBridge.cpp側)。
    struct MidiState;
    std::unique_ptr<MidiState> midiState_;

    // Config側の現在のMIDI入力ポート設定(getMidiInputCount()/
    // getMidiInputName())に基づき、既存ポートを全て閉じてから開き直す
    // (init()とsetMidiInputPorts()の共通処理、2026年7月新設)。
    void reopenMidiPorts();
};
