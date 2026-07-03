// fitom/MultiDev_new.cpp
// マルチデバイス — ISoundDevice ベース移行版
//
// CMultiDevice / CSpanDevice / CUnison の実装本体は
// fitom/MultiDevice.h に移動した (他のチップドライバから
// CSpanDevice 等を継承できるようにするため)。
// このファイルにはファクトリ関数のみを残す。

#include "fitom/MultiDevice.h"
#include "fitom/Log.h"

namespace fitom {

std::unique_ptr<ISoundDevice> createCSpanDevice(ISoundDevice* c1, ISoundDevice* c2) {
    return std::make_unique<CSpanDevice>(c1, c2);
}
std::unique_ptr<ISoundDevice> createCUnison(ISoundDevice* c1, ISoundDevice* c2) {
    return std::make_unique<CUnison>(c1, c2);
}
std::unique_ptr<ISoundDevice> createCLinearPanDevice(ISoundDevice* left, ISoundDevice* right) {
    return std::make_unique<CLinearPanDevice>(left, right);
}

} // namespace fitom
