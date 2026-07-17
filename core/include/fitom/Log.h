#pragma once
// fitom/Log.h
// Boost.Log の薄いラッパー
//
// ─── 方針 ────────────────────────────────────────────────────────────────────
//   Boost.Log (boost/log/trivial.hpp) を直接使うと各翻訳単位で
//   ヘッダが重くなるため、FITOM 専用マクロで一箇所に集約する。
//   将来 spdlog 等に切り替える場合もこのヘッダだけ変更すればよい。
//
// ─── 使い方 ──────────────────────────────────────────────────────────────────
//   #include "fitom/Log.h"
//   FITOM_LOG_INFO("FITOM initialized, devices=" << devCount);
//   FITOM_LOG_WARN("Voice bank not found: " << bankName);
//   FITOM_LOG_ERR("Port open failed: " << e.what());
//
// ─── 初期化 ──────────────────────────────────────────────────────────────────
//   アプリ起動時に fitom::Log::init() を呼ぶ。
//   フィルタや出力先はそこで設定する。

#include <boost/log/trivial.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <string>
#include <filesystem>

namespace fitom {

// グローバルロガー宣言 (Log.cpp で実体化)
BOOST_LOG_GLOBAL_LOGGER(fitomLogger,
    boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level>)

class Log {
public:
    // アプリ起動時に一度だけ呼ぶ
    // logFile: 空文字列でファイル出力なし
    // minSeverity: "trace" / "debug" / "info" / "warning" / "error" / "fatal"
    // console: false でコンソール(標準出力)へのログ出力を無効化する
    //          (fitom.conf.json の log.console に対応)
    static void init(const std::string& minSeverity = "info",
                     const std::filesystem::path& logFile = {},
                     bool console = true);
};

} // namespace fitom

// ─── ログマクロ ──────────────────────────────────────────────────────────────
#define FITOM_LOG(sev, msg) \
    BOOST_LOG_SEV(fitom::fitomLogger::get(), boost::log::trivial::sev) << msg

#define FITOM_LOG_TRACE(msg) FITOM_LOG(trace,   msg)
#define FITOM_LOG_DEBUG(msg) FITOM_LOG(debug,   msg)
#define FITOM_LOG_INFO(msg)  FITOM_LOG(info,    msg)
#define FITOM_LOG_WARN(msg)  FITOM_LOG(warning, msg)
#define FITOM_LOG_ERR(msg)   FITOM_LOG(error,   msg)
#define FITOM_LOG_FATAL(msg) FITOM_LOG(fatal,   msg)
