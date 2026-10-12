// WoWBridge compat: WoWee reads pre-extracted loose files; WoWBridge reads the client's MPQs directly.
#pragma once
#include "wowcore/mpq.hpp"
#include <memory>
#include <string>
#include <vector>
namespace wowee::pipeline {
class AssetManager {
public:
    explicit AssetManager(std::shared_ptr<wowcore::Mpq> mpq) : mpq_(std::move(mpq)) {}
    std::vector<uint8_t> readFile(const std::string& path) const {
        auto d = mpq_ ? mpq_->read(path) : std::nullopt;
        return d ? std::move(*d) : std::vector<uint8_t>{};
    }
    bool fileExists(const std::string& path) const { return mpq_ && mpq_->exists(path); }
    const std::shared_ptr<wowcore::Mpq>& mpq() const { return mpq_; }
private:
    std::shared_ptr<wowcore::Mpq> mpq_;
};
}  // namespace wowee::pipeline
