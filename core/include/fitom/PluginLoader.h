#pragma once
// fitom/PluginLoader.h
// HW / MIDI 各バックエンド DLL の動的ロード共通ユーティリティ
//
// ─── 使用例 ──────────────────────────────────────────────────────────────────
//   auto loader = PluginLoader::load("YMEngine.dll");
//   auto fn = loader.sym<PFN_HWPlugin_Open>("HWPlugin_Open");
//
// ─── RAII ────────────────────────────────────────────────────────────────────
//   PluginLoader はムーブ可能、コピー不可。
//   デストラクタで DLL をアンロードする。

#include <string>
#include <stdexcept>
#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace fitom {

class PluginLoader {
public:
    // path: DLL/so/dylib のパス
    static PluginLoader load(const std::filesystem::path& path) {
        PluginLoader pl;
        pl.path_ = path;
#ifdef _WIN32
        pl.handle_ = LoadLibraryW(path.wstring().c_str());
        if (!pl.handle_) {
            DWORD e = GetLastError();
            char buf[256] = {};
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, e, 0, buf, sizeof(buf), nullptr);
            throw std::runtime_error("PluginLoader: LoadLibrary(\"" +
                path.string() + "\") failed: " + buf);
        }
#else
        pl.handle_ = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!pl.handle_) {
            const char* e = dlerror();
            throw std::runtime_error("PluginLoader: dlopen(\"" +
                path.string() + "\") failed: " + (e ? e : "unknown"));
        }
#endif
        return pl;
    }

    PluginLoader() = default;
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    PluginLoader(PluginLoader&& other) noexcept
        : handle_(other.handle_), path_(std::move(other.path_)) {
        other.handle_ = nullptr;
    }
    PluginLoader& operator=(PluginLoader&& other) noexcept {
        if (this != &other) {
            unload();
            handle_ = other.handle_;
            path_   = std::move(other.path_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~PluginLoader() { unload(); }

    bool isLoaded() const noexcept { return handle_ != nullptr; }

    const std::filesystem::path& path() const { return path_; }

    // シンボルを取得。見つからない場合は nullptr を返す (例外なし)
    void* symRaw(const char* name) const noexcept {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
        return dlsym(handle_, name);
#endif
    }

    // 型付きシンボル取得。見つからない場合は nullptr を返す
    template<typename FnPtr>
    FnPtr sym(const char* name) const noexcept {
        return reinterpret_cast<FnPtr>(symRaw(name));
    }

    // 型付きシンボル取得。見つからない場合は例外を投げる
    template<typename FnPtr>
    FnPtr symRequired(const char* name) const {
        auto fn = sym<FnPtr>(name);
        if (!fn) throw std::runtime_error(
            "PluginLoader: symbol \"" + std::string(name) +
            "\" not found in \"" + path_.string() + "\"");
        return fn;
    }

    // オプション関数: 存在しない場合は nullptr を返す (旧 DLL との互換)
    template<typename FnPtr>
    FnPtr symOptional(const char* name) const noexcept {
        return sym<FnPtr>(name);   // sym は失敗時に nullptr を返す
    }

private:
    void unload() noexcept {
        if (!handle_) return;
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }

#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void*   handle_ = nullptr;
#endif
    std::filesystem::path path_;
};

} // namespace fitom
