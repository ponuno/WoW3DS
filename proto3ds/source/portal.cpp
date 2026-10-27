#include "portal.hpp"

#include <algorithm>
#include <cmath>

void PortalFx::start() {
    if (phase_ == Phase::Closing || phase_ == Phase::Hold) return;
    if (phase_ == Phase::Idle) cover_ = 0;   // opening: close again from where it is
    phase_ = Phase::Closing;
    finishAfterClose_ = false;
}

void PortalFx::finish() {
    if (phase_ == Phase::Closing) finishAfterClose_ = true;
    else if (phase_ == Phase::Hold) phase_ = Phase::Opening;
}

void PortalFx::cancel() {
    phase_ = Phase::Idle;
    cover_ = 0;
    finishAfterClose_ = false;
}

void PortalFx::update(float dt) {
    if (phase_ == Phase::Idle) return;
    time_ += dt;
    switch (phase_) {
    case Phase::Closing:
        cover_ = std::min(1.0f, cover_ + dt / kCloseSec);
        if (cover_ >= 1.0f) phase_ = finishAfterClose_ ? Phase::Opening : Phase::Hold;
        break;
    case Phase::Opening:
        cover_ = std::max(0.0f, cover_ - dt / kOpenSec);
        if (cover_ <= 0.0f) phase_ = Phase::Idle;
        break;
    default:
        break;
    }
}

namespace {
constexpr float kPi = 3.14159265f;

// White-hot core -> blue -> violet at the rim (u: 0 centre .. 1 rim).
uint32_t swirlColor(float u, int alpha) {
    float r, g, b;
    if (u < 0.35f) {
        float k = u / 0.35f;
        r = 255 + (110 - 255) * k; g = 255 + (190 - 255) * k; b = 255;
    } else {
        float k = (u - 0.35f) / 0.65f;
        r = 110 + (110 - 110) * k; g = 190 + (40 - 190) * k; b = 255 + (230 - 255) * k;
        r = 110 + (140 - 110) * k;
    }
    return hudRgba((int)r, (int)g, (int)b, std::clamp(alpha, 0, 255));
}
}  // namespace

void PortalFx::build(HudList& l, int w, int h, const char* text) const {
    if (phase_ == Phase::Idle || cover_ <= 0.0f) return;
    const float e = cover_ * cover_ * (3.0f - 2.0f * cover_);   // smoothstep
    const int savedFade = l.fade;
    if (pic_.ok()) {   // the game's own loading screen
        l.noClip();
        l.fade = 255;
        l.rect(0, 0, w, h, hudRgba(0, 0, 0, (int)(255.0f * e)));
        l.image(0, 0, w, h, pic_, hudRgba(255, 255, 255, (int)(255.0f * e)));
        if (progress_ >= 0.0f) {
            const int bw = w - 120, bx = 60, by = h - 30;
            l.rect(bx - 2, by - 2, bw + 4, 12, hudRgba(0, 0, 0, (int)(200.0f * e)));
            l.rect(bx, by, (int)(bw * std::clamp(progress_, 0.0f, 1.0f)), 8, hudRgba(214, 170, 40, (int)(255.0f * e)));
            l.outline(bx - 2, by - 2, bw + 4, 12, hudRgba(120, 100, 60, (int)(255.0f * e)));
        }
        if (text && *text && cover_ > 0.6f) l.textCentered(w / 2, h - 50, text, hudRgba(255, 235, 170), true, ui::Font::Normal);
        l.fade = savedFade;
        return;
    }
    const int cx = w / 2, cy = h / 2 - (text && *text ? 10 : 0);
    l.noClip();
    l.fade = 255;
    // the world dims into a deep violet
    l.rect(0, 0, w, h, hudRgba(14, 8, 36, (int)(235.0f * e)));
    // the portal disc: nested translucent ellipses (horizontal strips), brightest in the middle
    const float maxR = std::sqrt((float)(w * w + h * h)) * 0.5f * (0.15f + 0.95f * e);
    for (int ring = 0; ring < 5; ++ring) {
        float rx = maxR * (1.0f - ring * 0.17f) * 0.62f, ry = rx * 0.8f;
        int a = (int)((34 + ring * 22) * e);
        uint32_t col = swirlColor(1.0f - ring / 5.0f, a);
        for (int dy = -(int)ry; dy < (int)ry; dy += 6) {
            float k = (dy + 3) / ry;
            float hw = rx * std::sqrt(std::max(0.0f, 1.0f - k * k));
            if (hw >= 1.0f) l.rect(cx - (int)hw, cy + dy, (int)(2 * hw), 6, col);
        }
    }
    // three spiral arms, turning
    const int arms = 3, perArm = 48;
    for (int a = 0; a < arms; ++a)
        for (int i = 0; i < perArm; ++i) {
            float u = (float)i / (perArm - 1);
            float r = maxR * (0.08f + 0.92f * u);
            float th = a * 2.0f * kPi / arms + u * 5.2f - time_ * 3.2f + 0.04f * (i % 2);
            int sz = 3 + (int)(u * 5.0f);
            int x = cx + (int)(r * std::cos(th)) - sz / 2;
            int y = cy + (int)(r * std::sin(th) * 0.8f) - sz / 2;
            int alpha = (int)(240.0f * e * (1.0f - 0.45f * u));
            l.rect(x, y, sz, sz, swirlColor(1.0f - u, alpha));
        }
    // sparks orbiting the other way
    for (int i = 0; i < 20; ++i) {
        float u = std::fmod(i * 0.381966f + time_ * 0.25f * (1 + (i % 3)), 1.0f);
        float th = i * 2.399963f + time_ * (i % 2 ? 2.1f : -1.7f);
        float r = maxR * (0.1f + 0.9f * u);
        int x = cx + (int)(r * std::cos(th)), y = cy + (int)(r * std::sin(th) * 0.8f);
        l.rect(x, y, 2, 2, hudRgba(255, 255, 255, (int)(220.0f * e * (1.0f - u))));
    }
    // flash while it opens or closes fully
    if (phase_ == Phase::Opening) l.rect(0, 0, w, h, hudRgba(255, 255, 255, (int)(150.0f * std::max(0.0f, cover_ - 0.7f) / 0.3f)));
    if (text && *text && cover_ > 0.9f) l.textCentered(cx, h - 38, text, hudRgba(235, 225, 255), true, ui::Font::Normal);
    l.fade = savedFade;
}
