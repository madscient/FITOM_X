// fitom/IPort.cpp
// IPort 基底クラスの非純粋仮想メソッド実装

#include "fitom/IPort.h"

namespace fitom {

// writeBurst デフォルト実装: write() の繰り返し
void IPort::writeBurst(uint16_t addr, const uint8_t* data, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i) {
        write(static_cast<uint16_t>(addr + i), data[i]);
    }
}

// OffsetPort
std::string OffsetPort::getDesc()
{
    return parent_ ? (parent_->getDesc() + "+0x" + std::to_string(offset_)) : "OffsetPort";
}

// MappedPort
MappedPort::MappedPort(IPort* pt, uint32_t addr, uint32_t range)
{
    map(pt, addr, range);
}

IPort* MappedPort::findPort(uint32_t addr)
{
    for (auto& pm : ports_) {
        if (addr >= pm.addr && addr < pm.addr + pm.range)
            return pm.port;
    }
    return nullptr;
}

int MappedPort::findIndex(uint32_t addr)
{
    for (int i = 0; i < static_cast<int>(ports_.size()); ++i) {
        if (addr >= ports_[i].addr && addr < ports_[i].addr + ports_[i].range)
            return i;
    }
    return -1;
}

uint32_t MappedPort::nextAddress() const
{
    uint32_t next = 0;
    for (const auto& pm : ports_) {
        uint32_t end = pm.addr + pm.range;
        if (end > next) next = end;
    }
    return next;
}

uint32_t MappedPort::append(IPort* pt, uint32_t size)
{
    uint32_t addr = nextAddress();
    return map(pt, addr, size);
}

uint32_t MappedPort::map(IPort* pt, uint32_t addr, uint32_t size)
{
    PortMap pm{ pt, addr, size };
    ports_.push_back(pm);
    return addr;
}

void MappedPort::write(uint16_t addr, uint16_t data)
{
    if (auto* p = findPort(addr)) p->write(addr, data);
}

uint8_t MappedPort::read(uint16_t addr)
{
    if (auto* p = findPort(addr)) return p->read(addr);
    return 0;
}

uint8_t MappedPort::status()
{
    return ports_.empty() ? 0 : ports_[0].port->status();
}

void MappedPort::reset()
{
    for (auto& pm : ports_) pm.port->reset();
}

int MappedPort::getPanpot()
{
    return ports_.empty() ? 0 : ports_[0].port->getPanpot();
}

int MappedPort::getClock()
{
    return ports_.empty() ? 0 : ports_[0].port->getClock();
}

IPort* MappedPort::getSubPort(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(ports_.size())) return nullptr;
    return ports_[idx].port;
}

std::string MappedPort::getInterfaceDesc()
{
    return ports_.empty() ? "" : ports_[0].port->getInterfaceDesc();
}

std::string MappedPort::getDesc()
{
    return "MappedPort(" + std::to_string(ports_.size()) + " ports)";
}

} // namespace fitom
