#pragma once

#include "Theme.h"

namespace vox
{
/** The small help / settings card that drops down from the top bar. */
class Popover : public juce::Component
{
public:
    enum class Kind { settings, help };

    explicit Popover (Theme& theme);
    ~Popover() override;

    std::function<void (float)> onScale; // 0.7, 0.85 or 1.0
    std::function<void()> onClose;
    std::function<void (Look)> onLook;

    void show (Kind kind, float currentScale);
    Kind getKind() const noexcept { return kind; }

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 220;

private:
    class Link;
    class LookChip;

    Theme& theme;
    Kind kind = Kind::help;
    float scale = 0.85f;
    std::unique_ptr<Link> closeLink, small, medium, large;
    std::array<std::unique_ptr<LookChip>, numLooks> looks;
};
} // namespace vox
