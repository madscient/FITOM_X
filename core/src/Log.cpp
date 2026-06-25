// fitom/Log.cpp
// Boost.Log グローバルロガーの実体と初期化

#include "fitom/Log.h"
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/expressions/formatters/date_time.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <unordered_map>

namespace fitom {

// グローバルロガーの実体
BOOST_LOG_GLOBAL_LOGGER_DEFAULT(fitomLogger,
    boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level>)

void Log::init(const std::string& minSeverity, const std::filesystem::path& logFile)
{
    namespace log   = boost::log;
    namespace expr  = boost::log::expressions;
    namespace kw    = boost::log::keywords;
    namespace sinks = boost::log::sinks;
    using sev_t = log::trivial::severity_level;

    // minSeverity 文字列 → enum
    static const std::unordered_map<std::string, sev_t> sevMap {
        {"trace",   log::trivial::trace},
        {"debug",   log::trivial::debug},
        {"info",    log::trivial::info},
        {"warning", log::trivial::warning},
        {"error",   log::trivial::error},
        {"fatal",   log::trivial::fatal},
    };
    auto it = sevMap.find(minSeverity);
    sev_t minSev = (it != sevMap.end()) ? it->second : log::trivial::info;

    log::add_common_attributes();

    // フォーマット: [HH:MM:SS.fff] [LEVEL] message
    auto fmt = expr::format("[%1%] [%2%] %3%")
        % expr::format_date_time<boost::posix_time::ptime>(
            "TimeStamp", "%H:%M:%S.%f")
        % log::trivial::severity
        % expr::smessage;

    // コンソール出力
    log::add_console_log(
        std::cout,
        kw::format = fmt,
        kw::filter = log::trivial::severity >= minSev);

    // ファイル出力 (オプション)
    if (!logFile.empty()) {
        log::add_file_log(
            kw::file_name       = logFile.string(),
            kw::rotation_size   = 10 * 1024 * 1024,  // 10MB
            kw::auto_flush      = true,
            kw::format          = fmt,
            kw::filter          = log::trivial::severity >= minSev);
    }

    FITOM_LOG_INFO("FITOM Log initialized (min=" << minSeverity << ")");
}

} // namespace fitom
