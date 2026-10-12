#pragma once
#include "wowcore/character.hpp"
#include <memory>
namespace wowcore {
// The client's PlayerModel over an assembled look, using the same M2 skin, geosets and attachments as Avatar.
class PlayerModel {
public:
    explicit PlayerModel(const Mpq& mpq);
    ~PlayerModel();
    void setLook(const Look& look);
    bool ready() const;
    Image render(float rotation, int size = 256);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
