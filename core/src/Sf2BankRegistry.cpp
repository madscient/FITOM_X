// fitom/Sf2BankRegistry.cpp
#include "fitom/Sf2BankRegistry.h"
#include "fitom/Log.h"

namespace fitom {

namespace fs = std::filesystem;

void Sf2BankRegistry::load(const nlohmann::json& arr, const fs::path& baseDir)
{
    for (const auto& e : arr) {
        int bank        = e.value("bank", -1);
        std::string file = e.value("file", "");
        int sf2Bank     = e.value("sf2_bank", -1);

        if (bank < 0 || bank > 127 || file.empty() || sf2Bank < 0 || sf2Bank > 128) {
            FITOM_LOG_WARN("sf2_banks: invalid entry (bank/file/sf2_bank), skipping");
            continue;
        }

        fs::path path = file;
        if (path.is_relative()) path = baseDir / path;
        std::string absStr = path.lexically_normal().string();

        int fileIdx;
        auto fit = fileIndex_.find(absStr);
        if (fit != fileIndex_.end()) {
            fileIdx = fit->second;
        } else {
            fileIdx = static_cast<int>(files_.size());
            files_.push_back(absStr);
            fileIndex_[absStr] = fileIdx;
        }

        if (byBank_.count(bank)) {
            FITOM_LOG_WARN("sf2_banks: bank=" << bank << " が重複しています。後のエントリで上書きします");
        }
        byBank_[bank] = Entry{fileIdx, sf2Bank};
    }
}

bool Sf2BankRegistry::resolve(uint8_t cc32Bank, Resolved& out) const
{
    auto it = byBank_.find(static_cast<int>(cc32Bank));
    if (it == byBank_.end()) return false;
    out.soundfontIndex = it->second.fileIndex;
    out.sf2Bank        = it->second.sf2Bank;
    return true;
}

} // namespace fitom
