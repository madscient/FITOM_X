// apps/fitom_gui/main.cpp
//
// Dear ImGuiベースのGUIアプリケーション エントリポイント。
// GLFW(ウィンドウ/入力) + OpenGL3(描画)バックエンドを使用する。
// fitom_core には直接触れず、gui/bridge (FITOMBridge) 経由でのみ
// コアにアクセスする。
//
// 使い方: fitom_gui [profile.json]
// プロファイルを省略した場合、コアは未初期化のまま画面のみ表示する
// (デバイス一覧・MIDI入力一覧は空)。

#include "FITOMBridge.h"
#include "ChSettingsDialog.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h> // システムOpenGLヘッダーも引き込む

#include <cstdio>
#include <string>
#include <filesystem>
#include <array>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{

    void glfwErrorCallback(int error, const char *description)
    {
        std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
    }

    // 実行ファイル自身のディレクトリを取得する (フォントファイルの相対パス
    // 解決に使う。fitom_cli の同名ヘルパーと同じ実装)。取得できなければ
    // カレントディレクトリを返す。
    fs::path exeDir()
    {
#if defined(_WIN32)
        char buf[MAX_PATH] = {};
        DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return fs::path(buf).parent_path();
#elif defined(__linux__)
        char buf[PATH_MAX] = {};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            return fs::path(buf).parent_path();
        }
#elif defined(__APPLE__)
        char buf[PATH_MAX] = {};
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0)
            return fs::path(buf).parent_path();
#endif
        return fs::current_path();
    }

    // ─── エラーポップアップ ─────────────────────────────────────────────────
    // ネイティブのメッセージボックス(WindowsのみのAPI)ではなく、GLFW+OpenGL3
    // 上で完結するImGuiモーダルとして実装する(他プラットフォームでも同じ
    // コードで動く。Windows専用APIをここで増やさないため)。showErrorPopup()
    // で内容をセットし、メインループから毎フレームrenderErrorPopup()を呼ぶ。
    std::string g_errorPopupMessage;
    bool g_errorPopupPending = false; // OpenPopup()をまだ呼んでいない

    void showErrorPopup(const std::string &message)
    {
        g_errorPopupMessage = message;
        g_errorPopupPending = true;
    }

    void renderErrorPopup()
    {
        if (g_errorPopupPending)
        {
            ImGui::OpenPopup("FITOM_X エラー");
            g_errorPopupPending = false;
        }
        ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("FITOM_X エラー", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("%s", g_errorPopupMessage.c_str());
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120.0f, 0.0f)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ─── 外部プロセス起動 ───────────────────────────────────────────────────
    // exeをargs付きで子プロセスとして起動する(終了を待たない、
    // fire-and-forget)。呼び出し自体(CreateProcess/fork)が失敗した場合のみ
    // falseを返す。子プロセス起動後の内部エラー(引数不正等)はここでは
    // 検出できない(パッチエディタ側のキオスクモードは標準エラー出力+
    // 終了コードで失敗を伝える設計のため、同期的に監視するには終了を
    // 待つ必要があり、それは「エディタウィンドウを開いたまま操作を待つ」
    // という主用途とは相容れない)。
    bool launchProcess(const fs::path &exe, const std::vector<std::string> &args,
                       std::string &errorOut)
    {
#if defined(_WIN32)
        std::string cmdLine = "\"" + exe.string() + "\"";
        for (const auto &a : args)
            cmdLine += " \"" + a + "\"";

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // CreateProcessAはコマンドライン文字列を書き換えるため、
        // 書き込み可能なバッファにコピーしてから渡す。
        std::vector<char> buf(cmdLine.begin(), cmdLine.end());
        buf.push_back('\0');
        std::string workDir = exe.parent_path().string();
        BOOL ok = CreateProcessA(
            exe.string().c_str(), buf.data(),
            nullptr, nullptr, FALSE, 0, nullptr,
            workDir.empty() ? nullptr : workDir.c_str(),
            &si, &pi);
        if (!ok)
        {
            errorOut = "CreateProcess失敗 (エラーコード " + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
#else
        pid_t pid = fork();
        if (pid < 0)
        {
            errorOut = "fork()失敗";
            return false;
        }
        if (pid == 0)
        {
            // 子プロセス側。execvに渡すargv配列を組み立てる。
            std::vector<std::string> argvStrings;
            argvStrings.push_back(exe.string());
            for (const auto &a : args)
                argvStrings.push_back(a);
            std::vector<char *> argv;
            argv.reserve(argvStrings.size() + 1);
            for (auto &s : argvStrings)
                argv.push_back(s.data());
            argv.push_back(nullptr);
            execv(exe.c_str(), argv.data());
            _exit(127); // execvが返ってきた = 失敗
        }
        // 親プロセス側。forkが成功すれば、execv自体の失敗(実行権限無し等)は
        // ここからは検出できない(fire-and-forgetのため子の終了を待たない)。
        return true;
#endif
    }

    // 日本語グリフを含むフォントを読み込む。ImGuiのデフォルトフォントは
    // ASCII相当のみで、日本語文字列を描画すると全て「?」になってしまう
    // ため、apps/fitom_gui/assets/fonts/ に同梱したNoto Sans JPを読み込む。
    // 見つからない場合は警告を出し、デフォルトフォントのまま続行する
    // (日本語部分は文字化けするが、起動自体は継続できるようにする)。
    void loadJapaneseFont(ImGuiIO &io)
    {
        // 実行ファイルからの相対位置(インストール後のレイアウト)と、
        // ビルドディレクトリから見たソースツリー上の位置の両方を試す
        // (開発中、fitom_gui をビルドディレクトリから直接実行するケースに
        // 対応するため)。
        const fs::path candidates[] = {
            exeDir() / "assets" / "fonts" / "NotoSansJP-Regular.ttf",
            exeDir() / ".." / ".." / "apps" / "fitom_gui" / "assets" / "fonts" / "NotoSansJP-Regular.ttf",
        };

        for (const auto &path : candidates)
        {
            if (!fs::exists(path))
                continue;
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = true;
            ImFont *font = io.Fonts->AddFontFromFileTTF(
                path.string().c_str(), 18.0f, &cfg, io.Fonts->GetGlyphRangesJapanese());
            if (font)
            {
                std::fprintf(stderr, "Loaded Japanese font: %s\n", path.string().c_str());
                return;
            }
        }
        std::fprintf(stderr,
                     "Warning: Japanese font not found (apps/fitom_gui/assets/fonts/NotoSansJP-Regular.ttf). "
                     "Japanese text will not render correctly.\n");
    }

    // FITOMBridge::getDevices/getMidiInputs の内容をそれぞれ描画する。
    [[maybe_unused]] void renderDeviceList(const FITOMBridge &bridge)
    {
        if (!ImGui::CollapsingHeader("デバイス一覧", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        auto devices = bridge.getDevices();
        if (devices.empty())
        {
            ImGui::TextDisabled("(デバイスがありません)");
            return;
        }
        if (ImGui::BeginTable("devices", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("ラベル");
            ImGui::TableSetupColumn("種別");
            ImGui::TableSetupColumn("ch数");
            ImGui::TableHeadersRow();
            for (const auto &dev : devices)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", dev.index);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(dev.label.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(dev.descriptor.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", dev.chCount);
            }
            ImGui::EndTable();
        }
    }

    [[maybe_unused]] void renderMidiInputList(const FITOMBridge &bridge)
    {
        if (!ImGui::CollapsingHeader("MIDI入力一覧", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        auto inputs = bridge.getMidiInputs();
        if (inputs.empty())
        {
            ImGui::TextDisabled("(MIDI入力がありません)");
            return;
        }
        for (const auto &m : inputs)
        {
            ImGui::BulletText("[%d] %s", m.index, m.name.c_str());
        }
    }

    [[maybe_unused]] void renderMasterControls(FITOMBridge &bridge)
    {
        if (!ImGui::CollapsingHeader("マスターコントロール", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        int vol = bridge.getMasterVolume();
        if (ImGui::SliderInt("マスターボリューム", &vol, 0, 127))
        {
            bridge.setMasterVolume(static_cast<uint8_t>(vol));
        }

        double pitch = bridge.getMasterPitch();
        if (ImGui::InputDouble("マスターピッチ(Hz)", &pitch, 1.0, 10.0, "%.2f"))
        {
            bridge.setMasterPitch(pitch);
        }

        if (ImGui::Button("オールノートオフ"))
            bridge.allNoteOff();
        ImGui::SameLine();
        if (ImGui::Button("リセットオールコントローラー"))
            bridge.resetAllCtrl();
    }

    // ─── MIDIモニター(試作): CH1のみ、列構成の確認用 ────────────────────
    // ImGui::Tableではなく固定X座標で列を並べる方式にしている。理由:
    // キーボードビュー行(次段階で実装)がテーブル幅いっぱいに独自描画
    // (ImDrawList)する必要があり、ImGui::Tableの自動列管理とは相性が
    // 悪いため。列幅はここで確定させ、以後全チャンネル分に展開する。
    // 与えられた文字列を、maxWidth(ピクセル)に収まるよう末尾を"..."で
    // 省略して描画する。全体がホバーされたときは、省略前の全文をツール
    // チップで表示する。
    void textEllipsis(const std::string &text, float maxWidth)
    {
        const ImVec2 fullSize = ImGui::CalcTextSize(text.c_str());
        if (fullSize.x <= maxWidth)
        {
            ImGui::TextUnformatted(text.c_str());
            return;
        }

        const float ellipsisWidth = ImGui::CalcTextSize("...").x;
        const float avail = maxWidth - ellipsisWidth;

        // 収まる文字数を先頭から1文字ずつ増やして探す(バイト単位ではなく
        // UTF-8の文字境界を壊さないよう、ImGuiのCalcTextSize自体は
        // バイト列に対して幅を返すため、マルチバイト文字の途中で切らない
        // ようにUTF-8のリード文字境界でのみ区切る)。
        size_t cut = 0;
        for (size_t i = 0; i <= text.size();)
        {
            // 次のUTF-8文字境界へ進める
            size_t next = i;
            if (next < text.size())
            {
                unsigned char c = static_cast<unsigned char>(text[next]);
                size_t len = (c < 0x80) ? 1 : (c >> 5 == 0x6) ? 2
                                          : (c >> 4 == 0xE)   ? 3
                                          : (c >> 3 == 0x1E)  ? 4
                                                              : 1;
                next += len;
            }
            else
            {
                next = text.size() + 1; // ループ終了用
            }
            if (next > text.size())
                break;

            float w = ImGui::CalcTextSize(text.c_str(), text.c_str() + next).x;
            if (w > avail)
                break;
            cut = next;
            i = next;
        }

        std::string truncated = text.substr(0, cut) + "...";
        ImGui::TextUnformatted(truncated.c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", text.c_str());
        }
    }

    struct MonitorColumns
    {
        // 左端の余白。文字がウィンドウ端にぴったり付いて見えるのを避ける
        // (およそ半角2文字分)。
        static constexpr float leftMargin = 24.0f;

        static constexpr float ch = leftMargin;
        static constexpr float bank = leftMargin + 34.0f;
        static constexpr float program = leftMargin + 254.0f;
        static constexpr float volume = leftMargin + 474.0f;
        static constexpr float expression = leftMargin + 514.0f;
        static constexpr float panpot = leftMargin + 554.0f;
        static constexpr float note = leftMargin + 594.0f;
        static constexpr float device = leftMargin + 644.0f;
        static constexpr float fnumber = leftMargin + 824.0f;
        static constexpr float keyboardWidth = 980.0f;             // キーボードビュー自体の幅(左マージンを含まない)
        static constexpr float total = leftMargin + keyboardWidth; // 行全体の幅(ヘッダ右端・ボタン位置等の基準)

        // 各列の表示可能幅(次の列の開始位置との差分から、余白分を引く)
        static constexpr float bankWidth = program - bank - 8.0f;
        static constexpr float programWidth = volume - program - 8.0f;
        static constexpr float deviceWidth = fnumber - device - 8.0f;
    };

    void renderMonitorHeader()
    {
        using C = MonitorColumns;
        ImGui::SetCursorPosX(C::ch);
        ImGui::TextUnformatted("CH");
        ImGui::SameLine(C::bank);
        ImGui::TextUnformatted("Bank");
        ImGui::SameLine(C::program);
        ImGui::TextUnformatted("Program");
        ImGui::SameLine(C::volume);
        ImGui::TextUnformatted("Vol.");
        ImGui::SameLine(C::expression);
        ImGui::TextUnformatted("Exp.");
        ImGui::SameLine(C::panpot);
        ImGui::TextUnformatted("Pan.");
        ImGui::SameLine(C::note);
        ImGui::TextUnformatted("Note");
        ImGui::SameLine(C::device);
        ImGui::TextUnformatted("Device");
        ImGui::SameLine(C::fnumber);
        ImGui::TextUnformatted("Fnumber");
    }

    // MIDIモニターのBank/Program表示をダブルクリックしたときに、外部の
    // パッチエディタ(FITOM_patch_editor、実行ファイルはfitom_guiと同じ
    // ディレクトリに配置されている想定)をキオスクモードで起動する。
    // 起動引数の仕様(<profile.json> <hwbank-file> <prog>)は
    // FITOM_patch_editor側 docs/DESIGN.md の D-026 参照。
    void launchPatchEditorForChannel(FITOMBridge &bridge, int mpuIndex, int ch)
    {
        std::string hwBankFile;
        int progNo = 0;
        if (!bridge.resolveChannelHwPatch(mpuIndex, ch, hwBankFile, progNo))
        {
            showErrorPopup(
                "このチャンネルには現在編集可能なパッチがありません。\n"
                "(リズムチャンネル、またはAWM等HwBankを使わない音色の可能性があります)");
            return;
        }

        const std::string profilePath = bridge.currentProfilePath();
        if (profilePath.empty())
        {
            showErrorPopup("プロファイルが読み込まれていません。");
            return;
        }

#if defined(_WIN32)
        const fs::path editorExe = exeDir() / "fitom_patch_editor_gui.exe";
#else
        const fs::path editorExe = exeDir() / "fitom_patch_editor_gui";
#endif
        if (!fs::exists(editorExe))
        {
            showErrorPopup("パッチエディタが見つかりません:\n" + editorExe.string());
            return;
        }

        const std::vector<std::string> args = {profilePath, hwBankFile, std::to_string(progNo)};
        std::string launchError;
        if (!launchProcess(editorExe, args, launchError))
        {
            showErrorPopup("パッチエディタの起動に失敗しました:\n" + launchError);
        }
    }

    // 1チャンネル分のデータ行(バンク/プログラム/ボリューム/ノート/デバイス/
    // Fnumber)を描画する。発音していない間はNote以降を空欄にする。
    // CH列はシングルクリックでCH設定ダイアログを開く。
    // Bank/Program列はダブルクリックで外部パッチエディタを起動できる。
    void renderMonitorDataRow(FITOMBridge &bridge, int mpuIndex, const FITOMChannelMonitor &mon,
                              ChSettingsDialog &chDialog)
    {
        using C = MonitorColumns;
        char buf[64];

        // CH列: 不可視Selectableでヒット領域を確保し、実テキストを重ねて
        // 描画する(Bank/Program列と同じパターン)。
        ImGui::SetCursorPosX(C::ch);
        const ImVec2 chAreaPos(ImGui::GetCursorPos());
        const ImVec2 chAreaSize(C::bank - C::ch - 4.0f, ImGui::GetTextLineHeight());
        const std::string chSelId = "##chsel" + std::to_string(mon.ch);
        if (ImGui::Selectable(chSelId.c_str(), false, 0, chAreaSize))
        {
            chDialog.open(bridge, mpuIndex, mon.ch);
        }
        ImGui::SetCursorPos(chAreaPos);
        ImGui::Text("%d", mon.ch + 1);

        // Bank+Program列をまとめてダブルクリック検出領域にする。まず
        // 不可視のSelectableでヒットテスト用の領域を確保し、その後
        // カーソル位置を戻して同じ場所に実際のテキストを重ねて描画する
        // (Selectableのホバーハイライトはテキストの下に自然に表示される)。
        ImGui::SameLine(C::bank);
        const ImVec2 patchAreaPos(ImGui::GetCursorPos());
        const ImVec2 patchAreaSize(C::volume - C::bank - 8.0f, ImGui::GetTextLineHeight());
        const std::string selId = "##patch" + std::to_string(mon.ch);
        if (ImGui::Selectable(selId.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, patchAreaSize))
        {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                launchPatchEditorForChannel(bridge, mpuIndex, mon.ch);
            }
        }
        ImGui::SetCursorPos(patchAreaPos);

        if (!mon.bankName.empty())
        {
            std::snprintf(buf, sizeof(buf), "%d:%d %s", mon.bankNo, mon.progNo, mon.bankName.c_str());
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%d:%d <Bank name>", mon.bankNo, mon.progNo);
        }
        textEllipsis(buf, C::bankWidth);

        ImGui::SameLine(C::program);
        if (!mon.progName.empty())
        {
            std::snprintf(buf, sizeof(buf), "%d: %s", mon.progNo, mon.progName.c_str());
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%d: <Prog name>", mon.progNo);
        }
        textEllipsis(buf, C::programWidth);

        ImGui::SameLine(C::volume);
        ImGui::Text("%d", mon.volume);
        ImGui::SameLine(C::expression);
        ImGui::Text("%d", mon.expression);
        ImGui::SameLine(C::panpot);
        ImGui::Text("%d", mon.panpot);

        // Note以降: 発音中のみ表示する。
        ImGui::SameLine(C::note);
        if (mon.sounding && mon.lastNote != 0xFF)
        {
            ImGui::TextUnformatted(mon.noteName.c_str());
        }

        ImGui::SameLine(C::device);
        if (mon.sounding && !mon.deviceName.empty())
        {
            std::snprintf(buf, sizeof(buf), "%d %s", mon.deviceIndex, mon.deviceName.c_str());
            textEllipsis(buf, C::deviceWidth);
        }

        ImGui::SameLine(C::fnumber);
        if (mon.sounding)
        {
            ImGui::Text("%d:%d", mon.fnumBlock, mon.fnum);
        }

        // 行の末尾を常にSameLine()で終える。ImGui::ItemSize()は呼ばれる
        // たびにwindow->DC.CurrLineSize.yを0にリセットし、SameLine()が
        // PrevLineSizeから復元する仕組みのため、この行の最後の要素が
        // 実際のアイテム(上のText、発音中のみ)かSameLine()呼び出しのみ
        // (非発音時)かによって、直後にNewLine()を呼んだときの挙動が
        // 変わってしまう(CurrLineSize==0の場合、NewLine()は「空行」と
        // 誤認してFontSize分の余計な空行を挿入する)。ここで無条件に
        // SameLine()を呼び、常に「復元済み」の状態で関数を抜けることで、
        // 呼び出し側のNewLine()が発音状態に関わらず安定した挙動になる
        // ようにする(発音中のみキーボードビューとの間に空行が挿入され、
        // 以降の全チャンネルの表示位置がずれ下がる不具合の原因だった)。
        ImGui::SameLine();
    }

    // ─── キーボードビュー(128ノート)+発光エフェクト ────────────────────
    // 本物の鍵盤と同じく、白鍵の幅が鍵盤全体のピッチを支配し、黒鍵は白鍵の
    // 隙間に重ねて描く(黒鍵の下にも白鍵の地色が続くため、発光していない
    // 範囲が不自然に暗くならない)。ノートオン中の全ノートを同時に発光させる
    // (和音を弾いても、後から鳴らしたノートだけが光る、ということはない)。
    struct KeyGlowState
    {
        float brightness = 0.0f;  // 0-1、ノートオン時のベロシティ由来。0=消灯
        float releasedAt = -1.0f; // 発音が止まった時刻(ImGui::GetTime()基準)。
                                  // -1の間はまだ発音中(フェード開始前)。
    };
    using ChannelGlow = std::array<KeyGlowState, 128>;

    bool isBlackKeyNote(int note)
    {
        int p = note % 12;
        return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
    }

    // 半音(0=C ... 11=B)から、その音が属する/直前の白鍵のオクターブ内
    // インデックス(C=0,D=1,E=2,F=3,G=4,A=5,B=6)を引く表。黒鍵の場合は
    // 直前の白鍵(例: C#→C、D#→D)のインデックスを返す。
    constexpr int kWhiteIndexInOctave[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    constexpr int kWhiteKeysPerOctave = 7;

    // 白鍵の通し番号(0始まり)。黒鍵の場合は「直前の白鍵」の通し番号を返す
    // (黒鍵はこの値+1の白鍵との境界に重ねて描く)。
    int whiteKeyOrdinal(int note)
    {
        return (note / 12) * kWhiteKeysPerOctave + kWhiteIndexInOctave[note % 12];
    }

    void renderKeyboardView(const FITOMChannelMonitor &mon, ChannelGlow &glow, float now)
    {
        using C = MonitorColumns;
        constexpr float kHeight = 20.0f;
        constexpr int kNumNotes = 128;
        constexpr float kFadeSec = 0.25f;             // ノートオフ後、発光が消えるまでの時間
        constexpr float kBlackKeyHeightRatio = 0.62f; // 白鍵に対する黒鍵の高さ比
        constexpr float kBlackKeyWidthRatio = 0.62f;  // 白鍵に対する黒鍵の幅比

        // 現在発音中の全ノートを直接反映する。activeNotesに含まれるノートは
        // 発音継続中としてフェードをキャンセルし、含まれなくなった時点で
        // フェードを開始する(和音中の1音だけがノートオフされた場合も、
        // そのノートだけが個別にフェードする)。
        std::array<int, kNumNotes> activeVelocity;
        activeVelocity.fill(-1);
        for (const auto &an : mon.activeNotes)
        {
            if (an.note < kNumNotes)
                activeVelocity[an.note] = an.velocity;
        }
        for (int n = 0; n < kNumNotes; ++n)
        {
            KeyGlowState &g = glow[n];
            if (activeVelocity[n] >= 0)
            {
                g.brightness = static_cast<float>(activeVelocity[n]) / 127.0f;
                g.releasedAt = -1.0f;
            }
            else if (g.brightness > 0.0f && g.releasedAt < 0.0f)
            {
                g.releasedAt = now;
            }
        }

        // 発光の現在アルファ値を計算する。フェードが完了したノートは
        // brightnessを0に戻し、以後は消灯状態として扱う。
        auto glowAlphaFor = [&](int n) -> float
        {
            KeyGlowState &g = glow[n];
            if (g.brightness <= 0.0f)
                return 0.0f;
            if (g.releasedAt < 0.0f)
                return g.brightness;
            const float t = (now - g.releasedAt) / kFadeSec;
            if (t >= 1.0f)
            {
                g.brightness = 0.0f;
                g.releasedAt = -1.0f;
                return 0.0f;
            }
            return g.brightness * (1.0f - t);
        };

        auto colorForNote = [&](int n, ImU32 baseCol) -> ImU32
        {
            const float a = glowAlphaFor(n);
            if (a <= 0.001f)
                return baseCol;
            // ベロシティ(明るさ)に応じて赤→オレンジ→黄色寄りに変化させる。
            const int g = static_cast<int>(120 + 135 * a);
            const int b = static_cast<int>(40 * (1.0f - a));
            return IM_COL32(255, g, b, 255);
        };

        ImGui::SetCursorPosX(C::leftMargin);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        // 白鍵の総数を数え、その幅を鍵盤全体のピッチの基準にする。
        int whiteKeyCount = 0;
        for (int n = 0; n < kNumNotes; ++n)
        {
            if (!isBlackKeyNote(n))
                ++whiteKeyCount;
        }
        const float whiteKeyWidth = C::keyboardWidth / static_cast<float>(whiteKeyCount);

        // 白鍵を先に描画する(黒鍵の下敷きになる位置関係)。
        for (int n = 0; n < kNumNotes; ++n)
        {
            if (isBlackKeyNote(n))
                continue;
            const float x0 = origin.x + static_cast<float>(whiteKeyOrdinal(n)) * whiteKeyWidth;
            const float x1 = x0 + whiteKeyWidth - 1.0f; // 1px の隙間で列を区切る
            dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + kHeight),
                              colorForNote(n, IM_COL32(220, 220, 214, 255)));
        }

        // 黒鍵は白鍵同士の境界に重ねて描く(実物の鍵盤と同じ配置)。
        const float blackKeyWidth = whiteKeyWidth * kBlackKeyWidthRatio;
        for (int n = 0; n < kNumNotes; ++n)
        {
            if (!isBlackKeyNote(n))
                continue;
            const float boundary = origin.x + static_cast<float>(whiteKeyOrdinal(n) + 1) * whiteKeyWidth;
            const float x0 = boundary - blackKeyWidth * 0.5f;
            const float x1 = x0 + blackKeyWidth;
            dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + kHeight * kBlackKeyHeightRatio),
                              colorForNote(n, IM_COL32(24, 24, 28, 255)));
        }

        ImGui::Dummy(ImVec2(C::keyboardWidth, kHeight));
    }

    // MIDIモニター バンド。ルート画面に常時表示する主要コンテンツ。
    // MPU(16chの処理単位、現状最大4面)を`<`/`>`ボタンで切り替えられる。
    void renderMidiMonitorBand(FITOMBridge &bridge)
    {
        using C = MonitorColumns;

        static int mpuIndex = 0;
        static std::array<ChannelGlow, 16> keyGlow{};
        static ChSettingsDialog chDialog;

        auto midiInputs = bridge.getMidiInputs();

        // ページ数はMAX_MPUS(コア側の固定上限、現状4)ではなく、実際に
        // 設定されているMIDI入力ポート数に合わせる。未設定のMPUには
        // 対応するMidiProcessorが存在せず(CFITOM::init参照)、空の
        // プレースホルダー行しか表示できないため、意味の無いページに
        // なってしまう。
        int mpuCount = bridge.getMpuCount();
        if (!midiInputs.empty() && static_cast<int>(midiInputs.size()) < mpuCount)
        {
            mpuCount = static_cast<int>(midiInputs.size());
        }
        if (mpuCount <= 0)
            mpuCount = 1;
        if (mpuIndex >= mpuCount)
            mpuIndex = 0;

        // ポート名ラベルと切替ボタン(右上)。
        std::string portName;
        for (const auto &m : midiInputs)
        {
            if (m.index == mpuIndex)
            {
                portName = m.name;
                break;
            }
        }
        ImGui::SetCursorPosX(C::leftMargin);
        if (!portName.empty())
        {
            ImGui::Text("<%s>", portName.c_str());
        }
        else
        {
            ImGui::TextDisabled("<MIDI port name>");
        }
        ImGui::SameLine(C::total - 44.0f);
        ImGui::BeginDisabled(mpuCount <= 1);
        if (ImGui::ArrowButton("##mpu_prev", ImGuiDir_Left))
        {
            mpuIndex = (mpuIndex - 1 + mpuCount) % mpuCount;
            keyGlow.fill(ChannelGlow{}); // 別ポートの残像を持ち越さない
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##mpu_next", ImGuiDir_Right))
        {
            mpuIndex = (mpuIndex + 1) % mpuCount;
            keyGlow.fill(ChannelGlow{});
        }
        ImGui::EndDisabled();

        renderMonitorHeader();
        ImGui::Separator();

        auto monitors = bridge.getChannelMonitors(mpuIndex);
        const float now = static_cast<float>(ImGui::GetTime());

        if (monitors.empty())
        {
            FITOMChannelMonitor placeholder;
            renderMonitorDataRow(bridge, mpuIndex, placeholder, chDialog);
            ImGui::NewLine(); // SameLine()チェーンの末尾のままだと次の描画が
                              // 同じ行に重なってしまうため、明示的に改行する
            renderKeyboardView(placeholder, keyGlow[0], now);
        }
        else
        {
            for (size_t i = 0; i < monitors.size() && i < keyGlow.size(); ++i)
            {
                renderMonitorDataRow(bridge, mpuIndex, monitors[i], chDialog);
                ImGui::NewLine();
                renderKeyboardView(monitors[i], keyGlow[i], now);
            }
        }

        // CH設定ダイアログ本体(モーダル)。どのチャンネルがクリックされても
        // ここで一箇所だけ描画する。パッチピッカー/ドラムキット選択は
        // render()内部で入れ子描画される(ChSettingsDialog.h参照)。
        chDialog.render(bridge);
    }

} // namespace

int main(int argc, char **argv)
{
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit())
        return 1;

#if defined(__APPLE__)
    const char *glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char *glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    // 高さ900: MIDIモニターバンド(16ch分、1chあたりデータ行+
    // キーボードビューで約46px)が初期状態でスクロールバー無しに
    // 収まる高さ(ヘッダ部分込みで約806px)に余裕を持たせた値。
    GLFWwindow *window = glfwCreateWindow(1280, 900, "FITOM_X", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    loadJapaneseFont(io);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // FITOMBridge初期化。プロファイルはコマンドライン第1引数で指定する
    // (省略時はコア未初期化のまま、ウィンドウのみ表示する)。
    // fitom.conf.json は実行ファイルと同じディレクトリにあれば読み込む
    // (省略可能なシステム設定)。
    FITOMBridge &bridge = FITOMBridge::instance();
    const std::string profilePath = (argc >= 2) ? argv[1] : std::string();
    const fs::path sysConfPath = exeDir() / "fitom.conf.json";
    const std::string systemConfArg = fs::exists(sysConfPath) ? sysConfPath.string() : std::string();
    bool coreReady = false;
    if (!profilePath.empty())
    {
        coreReady = bridge.init(systemConfArg, profilePath);
    }

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // コアのタイマーコールバック(1ms相当)。フレームごとに1回呼ぶ簡易実装
        // (正確な1msキックが必要になったら別スレッド化を検討する)。
        if (coreReady)
            bridge.onTimer(1);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // OSウィンドウ(GLFW)いっぱいに、タイトルバー/枠/リサイズハンドル
        // 無しでImGuiのルートウィンドウを敷き詰める。単一画面のアプリ
        // なので、GLFWのウィンドウ枠と別にImGui側の入れ子ウィンドウ枠
        // (いわゆるMDI風の二重フレーム)を表示する必要が無いため。
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags rootFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("FITOM_X_Root", nullptr, rootFlags);
        if (!coreReady)
        {
            if (profilePath.empty())
            {
                ImGui::TextUnformatted(
                    "プロファイル未指定です。コマンドライン引数で指定してください:");
                ImGui::TextUnformatted("  fitom_gui <profile.json>");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "初期化に失敗しました: %s", profilePath.c_str());
            }
        }
        else
        {
            // ルート画面はMIDIモニターのバンドのみを表示する。
            // デバイス一覧・MIDI入力一覧・マスターコントロール等、他の
            // ビューへの導線は今後実装する(renderDeviceList/
            // renderMidiInputList/renderMasterControlsは[[maybe_unused]]
            // として温存済み)。
            renderMidiMonitorBand(bridge);
        }
        ImGui::End();

        renderErrorPopup();

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.10f, 0.10f, 0.12f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (coreReady)
        bridge.exit();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
