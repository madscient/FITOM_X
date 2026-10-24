// apps/fitom_render/main.cpp
//
// 指定したプロファイルでSMFを演奏し、結果をWAVファイルへ書き出す。
// 音声デバイスは使わず、HWプラグインにオフライン描画(HWPlugin_InitOffline/
// HWPlugin_Render)をさせる。プロファイルの全HWプラグインがオフライン描画に
// 対応していなければ変換しない(実チップの音は取り込む手段が無いため)。
//
// 使い方:
//   fitom_render <profile.json> <input.mid> <output.wav>
//                [--rate 44100] [--format s16|s24|f32] [--tail 3.0]

#include "SmfReader.h"
#include "WavWriter.h"
#include "OfflineRenderer.h"

#include "fitom/CFITOM.h"
#include "fitom/Config.h"
#include "fitom/HWPort.h"
#include "fitom/Log.h"
#include "fitom/PatchManager.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <unistd.h>
#  include <climits>
#endif

using namespace fitom;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kMinRate = 8000;
constexpr uint32_t kMaxRate = 384000;

fs::path exeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return fs::path(buf).parent_path(); }
#endif
    return fs::current_path();
}

struct Options {
    fs::path         profile;
    fs::path         input;
    fs::path         output;
    uint32_t         rate   = 44100;
    render::WavFormat format = render::WavFormat::S16;
    double           tailSec = 3.0;
};

void printUsage(const char* argv0)
{
    std::cerr << "使い方: " << argv0
              << " <profile.json> <input.mid> <output.wav>"
                 " [--rate 44100] [--format s16|s24|f32] [--tail 3.0]\n"
                 "  --rate   出力のサンプルレート(Hz、" << kMinRate << "〜" << kMaxRate << ")。既定 44100\n"
                 "  --format WAVの形式。s16=16bit整数 / s24=24bit整数 / f32=32bit浮動小数。既定 s16\n"
                 "  --tail   SMFの最後のイベントの後に描画を続ける秒数。既定 3.0\n";
}

std::optional<Options> parseArgs(int argc, char** argv)
{
    Options o;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rate" || a == "--format" || a == "--tail") {
            if (i + 1 >= argc) {
                std::cerr << a << " に値がありません\n";
                return std::nullopt;
            }
            const std::string v = argv[++i];
            if (a == "--rate") {
                char* end = nullptr;
                const unsigned long r = std::strtoul(v.c_str(), &end, 10);
                if (*end != '\0' || r < kMinRate || r > kMaxRate) {
                    std::cerr << "--rate の値が不正です: " << v << "\n";
                    return std::nullopt;
                }
                o.rate = static_cast<uint32_t>(r);
            } else if (a == "--format") {
                auto f = render::parseWavFormat(v);
                if (!f) {
                    std::cerr << "--format の値が不正です: " << v << "\n";
                    return std::nullopt;
                }
                o.format = *f;
            } else {
                char* end = nullptr;
                const double t = std::strtod(v.c_str(), &end);
                if (*end != '\0' || !std::isfinite(t) || t < 0.0) {
                    std::cerr << "--tail の値が不正です: " << v << "\n";
                    return std::nullopt;
                }
                o.tailSec = t;
            }
            continue;
        }
        if (a.rfind("--", 0) == 0) {
            std::cerr << "不明なオプションです: " << a << "\n";
            return std::nullopt;
        }
        switch (positional++) {
        case 0: o.profile = a; break;
        case 1: o.input   = a; break;
        case 2: o.output  = a; break;
        default:
            std::cerr << "引数が多すぎます: " << a << "\n";
            return std::nullopt;
        }
    }
    if (positional < 3) return std::nullopt;
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    auto opt = parseArgs(argc, argv);
    if (!opt) {
        printUsage(argv[0]);
        return 1;
    }

    // SMF と出力先は、プラグインを読み込む前に確かめる(失敗が早く分かるように)。
    render::SmfSong song;
    try {
        song = render::readSmf(opt->input);
    } catch (const std::exception& e) {
        std::cerr << "SMFの読み込みに失敗しました (" << opt->input.string() << "): " << e.what() << "\n";
        return 1;
    }

    auto config   = std::make_unique<FITOMConfig>();
    auto patchMgr = std::make_unique<PatchManager>();
    PatchManager* pmPtr = patchMgr.get();

    // コンソールには、このツール自身のメッセージだけを出す。fitom.conf.json の
    // log.level は他のアプリと共有で、コアの詳細なログが標準出力へ流れると
    // 変換の結果が読めなくなるため、log.console の既定を false にしている。
    fs::path sysConfPath = exeDir() / "fitom.conf.json";
    if (fs::exists(sysConfPath)) config->loadSystemConf(sysConfPath);
    Log::init(config->getLogLevel("info"),
              config->getLogFile("fitom_render.log"),
              config->getLogConsole(false));

    HWPluginRegistry& registry = config->getHWPluginRegistry();
    registry.setOfflineSampleRate(opt->rate);
    if (!config->loadProfile(opt->profile, pmPtr)) {
        std::cerr << "プロファイルの読み込みに失敗しました: " << opt->profile.string() << "\n";
        return 1;
    }
    const auto failed = registry.failedPlugins();
    if (!failed.empty()) {
        std::cerr << "オフライン描画に使えないHWプラグインがあります:";
        for (const auto& n : failed) std::cerr << " " << n;
        std::cerr << "\n(実チップのプラグイン、オフライン描画に未対応のプラグイン、または"
                     "読み込み・初期化に失敗したもの。詳細はログを見てください)\n";
        return 1;
    }
    if (registry.registeredCount() == 0) {
        std::cerr << "プロファイルにHWプラグインがありません: " << opt->profile.string() << "\n";
        return 1;
    }

    std::unique_ptr<render::WavWriter> wav;
    try {
        wav = std::make_unique<render::WavWriter>(opt->output, opt->rate, opt->format);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    CFITOM& fitom = CFITOM::instance();
    if (fitom.init(std::move(config), std::move(patchMgr)) != 0) {
        std::cerr << "FITOMの初期化に失敗しました\n";
        return 1;
    }

    // タイマースレッドは起動しない。時間は描画したサンプル数で進める。
    uint32_t tick = 0;
    render::OfflineTarget target;
    target.sendMidi = [&fitom](const uint8_t* d, std::size_t n) { fitom.receiveMpuByte(0, d, n, 0); };
    target.tick     = [&fitom, &tick]() { fitom.timerCallback(++tick); };
    target.render   = [&registry](float* out, uint32_t frames) {
        return registry.renderOffline(out, frames) == HW_OK;
    };

    int rc = 0;
    try {
        const auto tailUs = static_cast<uint64_t>(std::llround(opt->tailSec * 1e6));
        render::renderSong(song, opt->rate, tailUs, target,
                           [&wav](const float* buf, uint32_t frames) { wav->write(buf, frames); });
        wav->close();
        std::cerr << opt->output.string() << ": "
                  << static_cast<double>(wav->framesWritten()) / opt->rate << " 秒, "
                  << opt->rate << " Hz\n";
    } catch (const std::exception& e) {
        std::cerr << "変換に失敗しました: " << e.what() << "\n";
        rc = 1;
    }

    fitom.exit();
    return rc;
}
