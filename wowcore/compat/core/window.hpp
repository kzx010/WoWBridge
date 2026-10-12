// WoWBridge compat: the render-target size the UI lays out against (GetScreenWidth/Height).
#pragma once
namespace wowee::core {
class Window {
public:
    Window(int w, int h) : w_(w), h_(h) {}
    int getWidth() const { return w_; }
    int getHeight() const { return h_; }
    void setSize(int w, int h) { w_ = w; h_ = h; }
private:
    int w_, h_;
};
}  // namespace wowee::core
