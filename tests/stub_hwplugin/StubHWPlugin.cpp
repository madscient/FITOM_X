// tests/stub_hwplugin/StubHWPlugin.cpp
// HWPort / HWPluginInstance のテスト用 HW プラグイン。音は出さず、
// 部位ごとのゲインを保持して返すだけ。
//
// 同じソースを、部位ゲインの組のエクスポート状態だけ変えて3通りにビルドする
// (tests/CMakeLists.txt):
//   定義なし                 : 4 関数をすべてエクスポートする
//   STUB_HWPLUGIN_NO_PARTS   : 4 関数を1つもエクスポートしない
//   STUB_HWPLUGIN_HEAD_ONLY  : HWPlugin_GetPartCount だけをエクスポートする
//
// 既定値は 1.0 以外を混ぜてある。HWPort 側が値を中継せずに 1.0 を返しても
// テストが通ってしまわないようにするため。

#include <fitom/IHWPlugin.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Part {
    std::string name;
    float       l;
    float       r;
};

std::vector<Part> partsForChip(const std::string& chip)
{
    if (chip == "OPNA") return { {"FM", 1.0f, 1.0f}, {"SSG", 0.25f, 0.25f} };
    if (chip == "OPL3") return { {"AB", 1.0f, 1.0f}, {"CD",  0.0f,  0.0f} };
    return {};
}

} // namespace

struct HWDeviceOpaque {
    std::vector<Part> parts;
    int               clock = 0;
};

#if !defined(STUB_HWPLUGIN_NO_PARTS) && !defined(STUB_HWPLUGIN_HEAD_ONLY)
namespace {

Part* findPart(HWHandle handle, const char* name)
{
    for (auto& p : handle->parts)
        if (p.name == name) return &p;
    return nullptr;
}

} // namespace
#endif

extern "C" {

FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_GetName() { return "StubHW"; }

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Init(const char*) { return HW_OK; }

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_Shutdown() {}

// auto_devices の経路を通すため、OPNA を1台だけ列挙する。
FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_Enumerate()
{
    static const char kDevices[] = R"([{"chip":"OPNA","index":0}])";
    char* s = static_cast<char*>(std::malloc(sizeof(kDevices)));
    if (s) std::memcpy(s, kDevices, sizeof(kDevices));
    return s;
}

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_FreeString(const char* str)
{
    std::free(const_cast<char*>(str));
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Open(
    const char* params_json, HWHandle* out_handle)
{
    if (!params_json || !out_handle) return HW_ERR_INVALID_ARG;
    auto j = nlohmann::json::parse(params_json, nullptr, false);
    if (!j.is_object()) return HW_ERR_INVALID_ARG;

    auto* dev = new HWDeviceOpaque();
    if (j.contains("chip") && j["chip"].is_string())
        dev->parts = partsForChip(j["chip"].get<std::string>());
    if (j.contains("clock") && j["clock"].is_number_integer())
        dev->clock = j["clock"].get<int>();
    *out_handle = dev;
    return HW_OK;
}

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_Close(HWHandle handle) { delete handle; }

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Write(HWHandle, uint16_t, uint8_t)
{
    return HW_OK;
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_WriteBlock(
    HWHandle, uint8_t, const uint8_t*, size_t)
{
    return HW_OK;
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Reset(HWHandle, unsigned int)
{
    return HW_OK;
}

FITOM_HWP_API int  FITOM_HWP_CALL HWPlugin_GetClock(HWHandle handle) { return handle ? handle->clock : 0; }
FITOM_HWP_API int  FITOM_HWP_CALL HWPlugin_GetPanpot(HWHandle) { return 0; }
FITOM_HWP_API bool FITOM_HWP_CALL HWPlugin_IsOpen(HWHandle handle) { return handle != nullptr; }

#if !defined(STUB_HWPLUGIN_NO_PARTS)

FITOM_HWP_API uint32_t FITOM_HWP_CALL HWPlugin_GetPartCount(HWHandle handle)
{
    return handle ? static_cast<uint32_t>(handle->parts.size()) : 0;
}

#if !defined(STUB_HWPLUGIN_HEAD_ONLY)

FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_GetPartName(
    HWHandle handle, uint32_t index)
{
    if (!handle || index >= handle->parts.size()) return nullptr;
    return handle->parts[index].name.c_str();
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_SetPartGain(
    HWHandle handle, const char* part, float gain_l, float gain_r)
{
    if (!handle || !part) return HW_ERR_INVALID_ARG;
    Part* p = findPart(handle, part);
    if (!p) return HW_ERR_INVALID_ARG;
    p->l = gain_l;
    p->r = gain_r;
    return HW_OK;
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_GetPartGain(
    HWHandle handle, const char* part, float* out_gain_l, float* out_gain_r)
{
    if (!handle || !part || !out_gain_l || !out_gain_r) return HW_ERR_INVALID_ARG;
    const Part* p = findPart(handle, part);
    if (!p) return HW_ERR_INVALID_ARG;
    *out_gain_l = p->l;
    *out_gain_r = p->r;
    return HW_OK;
}

#endif // !STUB_HWPLUGIN_HEAD_ONLY
#endif // !STUB_HWPLUGIN_NO_PARTS

} // extern "C"
