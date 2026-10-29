#include "glue_ui.hpp"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cmath>

#include "ui_assets.hpp"
#include "ui_style.hpp"

using namespace ui;

namespace {
// libctru key bits (the UI stays free of 3ds.h)
constexpr uint32_t kA = 1u << 0, kB = 1u << 1, kSelect = 1u << 2, kStart = 1u << 3, kDRight = 1u << 4, kDLeft = 1u << 5,
                   kDUp = 1u << 6, kDDown = 1u << 7, kR = 1u << 8, kL = 1u << 9, kX = 1u << 10, kY = 1u << 11,
                   kCRight = 1u << 28, kCLeft = 1u << 29, kCUp = 1u << 30, kCDown = 1u << 31;
constexpr uint32_t kLeft = kDLeft | kCLeft, kRight = kDRight | kCRight, kUp = kDUp | kCUp, kDown = kDDown | kCDown;

constexpr uint32_t kGlueGold = hudRgba(255, 209, 0), kGlueText = hudRgba(255, 255, 255), kGlueDim = hudRgba(170, 170, 170);
constexpr uint32_t kAllianceBlue = hudRgba(90, 150, 255), kHordeRed = hudRgba(230, 60, 50);

bool inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

std::string realmType(const w3n::Realm& r) {
    switch (r.icon) {
        case 1: return tr("PvP", "PvP");
        case 6: return tr("RP", "RP");
        case 8: return tr("RP PvP", "RP PvP");
        default: return tr("Normal", "Обычный");
    }
}
std::string realmPop(const w3n::Realm& r) {
    if (r.flags & 0x02) return tr("Offline", "Не в сети");
    if (r.population < 0.75f) return tr("Low", "Низкая");
    if (r.population < 1.5f) return tr("Medium", "Средняя");
    return tr("High", "Высокая");
}
uint32_t realmPopColor(const w3n::Realm& r) {
    if (r.flags & 0x02) return kGlueDim;
    if (r.population < 0.75f) return hudRgba(30, 255, 0);
    if (r.population < 1.5f) return kGlueText;
    return hudRgba(255, 60, 40);
}
std::string charLine(const w3n::Character& c) {
    char b[96];
    snprintf(b, sizeof b, "%s %u %s %s", tr("Level", "Уровень"), (unsigned)c.level, glue::raceName(c.race, g_lang),
             glue::className(c.cls, g_lang));
    return b;
}
std::string zoneOf(const GlueModel& m, const w3n::Character& c) {
    if (m.texts)
        if (const std::string* z = m.texts->zone(c.zone)) return *z;
    return "";
}
const char* lookLabel(int row) {
    switch (row) {
        case 0: return tr("Skin Color", "Цвет кожи");
        case 1: return tr("Face", "Лицо");
        case 2: return tr("Hair Style", "Прическа");
        case 3: return tr("Hair Color", "Цвет волос");
        default: return tr("Facial Hair", "Борода");
    }
}
// the first / next value of a sorted choice list, wrapping
uint8_t stepIn(const std::vector<uint8_t>& v, uint8_t cur, int dir) {
    if (v.empty()) return cur;
    auto it = std::find(v.begin(), v.end(), cur);
    int i = it == v.end() ? 0 : (int)(it - v.begin());
    i = (i + dir + (int)v.size()) % (int)v.size();
    return v[(size_t)i];
}
uint8_t fit(const std::vector<uint8_t>& v, uint8_t cur) {
    if (v.empty() || std::find(v.begin(), v.end(), cur) != v.end()) return cur;
    return v[0];
}
}  // namespace

// =================================================================== state
void GlueUi::defaultFocus(GlueScreen s) {
    switch (s) {
        case GlueScreen::Login: focus_ = (int)kField << 16 | (int)GlueAction::EditAccount << 8; break;
        case GlueScreen::CharCreate: focus_ = (int)kRace << 16 | create.race; break;
        default: focus_ = -1; break;
    }
}

void GlueUi::startCreate(uint8_t race, uint8_t sex, const GlueModel& m) {
    create = GlueCreate();
    create.race = glue::race(race) ? race : 1;
    create.sex = sex & 1;
    int n = 0;
    const glue::ClassInfo* cs = glue::classes(&n);
    for (int i = 0; i < n; ++i)
        if (glue::allowed(create.race, cs[i].id)) { create.cls = cs[i].id; break; }
    randomize(m);
    modelYaw = 0;
    infoTab_ = 0;
    infoScroll_ = 0;
    focus_ = (int)kRace << 16 | create.race;
    ++rev_;
}

void GlueUi::randomize(const GlueModel& m) {
    auto pick = [&](const std::vector<uint8_t>& v, uint8_t def) -> uint8_t {
        if (v.empty()) return def;
        rand_ = rand_ * 1664525u + 1013904223u;
        return v[(rand_ >> 8) % v.size()];
    };
    if (m.appearanceKnown) {
        create.skin = pick(m.appearance.skins, 0);
        create.hairStyle = pick(m.appearance.hairStyles, 0);
        // faces and hair colours depend on the skin / style: login.cpp asks for the new lists next frame, clampLook fits them
        create.face = pick(m.appearance.faces, 0);
        create.hairColor = pick(m.appearance.hairColors, 0);
        create.facial = pick(m.appearance.facials, 0);
    } else {
        rand_ = rand_ * 1664525u + 1013904223u;
        create.skin = 0; create.face = 0; create.hairStyle = 0; create.hairColor = 0; create.facial = 0;
    }
    ++rev_;
}

void GlueUi::clampLook(const glue::Appearance& a) {
    const uint8_t before[5] = {create.skin, create.face, create.hairStyle, create.hairColor, create.facial};
    create.skin = fit(a.skins, create.skin);
    create.face = fit(a.faces, create.face);
    create.hairStyle = fit(a.hairStyles, create.hairStyle);
    create.hairColor = fit(a.hairColors, create.hairColor);
    create.facial = fit(a.facials, create.facial);
    const uint8_t after[5] = {create.skin, create.face, create.hairStyle, create.hairColor, create.facial};
    if (memcmp(before, after, sizeof before) != 0) ++rev_;
}

void GlueUi::selectRace(uint8_t race, const GlueModel& m) {
    if (!glue::race(race)) return;
    if (race != create.race) {
        create.race = race;
        if (!glue::allowed(race, create.cls)) {
            int n = 0;
            const glue::ClassInfo* cs = glue::classes(&n);
            for (int i = 0; i < n; ++i)
                if (glue::allowed(race, cs[i].id)) { create.cls = cs[i].id; break; }
        }
        create.skin = create.face = create.hairStyle = create.hairColor = create.facial = 0;   // another model's choices
        (void)m;
    }
    infoTab_ = 0;
    infoScroll_ = 0;
    ++rev_;
}

void GlueUi::stepLook(int row, int dir, const GlueModel& m) {
    if (!m.appearanceKnown) return;
    const glue::Appearance& a = m.appearance;
    switch (row) {
        case 0: create.skin = stepIn(a.skins, create.skin, dir); break;
        case 1: create.face = stepIn(a.faces, create.face, dir); break;
        case 2: create.hairStyle = stepIn(a.hairStyles, create.hairStyle, dir); break;
        case 3: create.hairColor = stepIn(a.hairColors, create.hairColor, dir); break;
        default: create.facial = stepIn(a.facials, create.facial, dir); break;
    }
    ++rev_;
}

// The focusable widget of the top screen nearest in a direction (centres; the axis distance counts double).
void GlueUi::moveFocus(int dx, int dy) {
    const Widget* cur = nullptr;
    for (const Widget& w : top_)
        if (w.focusable && focusKey(w) == focus_) cur = &w;
    if (!cur) {
        for (const Widget& w : top_)
            if (w.focusable) { focus_ = focusKey(w); return; }
        return;
    }
    const float cx = cur->x + cur->w * 0.5f, cy = cur->y + cur->h * 0.5f;
    const Widget* best = nullptr;
    float bestD = 1e30f;
    for (const Widget& w : top_) {
        if (!w.focusable || &w == cur) continue;
        const float wx = w.x + w.w * 0.5f, wy = w.y + w.h * 0.5f;
        const float ax = wx - cx, ay = wy - cy;
        const float along = dx ? ax * dx : ay * dy, across = dx ? std::fabs(ay) : std::fabs(ax);
        if (along <= 2.0f || across > along * 2.5f + 8.0f) continue;   // in that direction, not far off to the side
        const float d = along + across * 2.0f;
        if (d < bestD) { bestD = d; best = &w; }
    }
    if (best) { focus_ = focusKey(*best); ++rev_; }
}

GlueAction GlueUi::activate(const Widget& w, const GlueModel& m) {
    switch (w.kind) {
        case kRace: selectRace((uint8_t)w.arg, m); return GlueAction::None;
        case kSex: if (create.sex != w.arg) { create.sex = (uint8_t)w.arg; create.face = create.hairStyle = create.hairColor = create.facial = 0; ++rev_; } return GlueAction::None;
        case kClass:
            if (glue::allowed(create.race, (uint8_t)w.arg)) { create.cls = (uint8_t)w.arg; infoTab_ = 1; infoScroll_ = 0; ++rev_; }
            return GlueAction::None;
        case kLook: stepLook(w.arg / 2, (w.arg & 1) ? 1 : -1, m); return GlueAction::None;
        case kRealmRow:
            if (realm == w.arg) return GlueAction::RealmAccept;   // a second tap on the chosen row
            realm = w.arg; ++rev_; return GlueAction::None;
        case kCharRow:
            if (character == w.arg) return GlueAction::EnterWorld;
            character = w.arg; modelYaw = 0; ++rev_; return GlueAction::None;
        case kTab: infoTab_ = w.arg; infoScroll_ = 0; ++rev_; return GlueAction::None;
        case kScrollUp: listScroll_ = std::max(0, listScroll_ - 1); ++rev_; return GlueAction::None;
        case kScrollDown: ++listScroll_; ++rev_; return GlueAction::None;
        default: break;
    }
    if (w.kind == kButton && w.act == GlueAction::None && w.arg == 1) { randomize(m); return GlueAction::None; }
    if (w.act == GlueAction::DeleteCharacter && !confirmDelete_) {   // ask first
        confirmDelete_ = true;
        ++rev_;
        return GlueAction::None;
    }
    if (w.act == GlueAction::DeleteCharacter) confirmDelete_ = false;
    if (w.act == GlueAction::Back && w.arg == 7) { confirmDelete_ = false; ++rev_; return GlueAction::None; }   // dialog "Cancel"
    ++rev_;
    return w.act;
}

GlueAction GlueUi::update(const GlueModel& m, const GlueInput& in) {
    blink_ += in.dt;
    if (!screenSeen_ || m.screen != screen_) {   // a new screen: its default focus, the lists from the top
        screen_ = m.screen;
        screenSeen_ = true;
        defaultFocus(m.screen);
        confirmDelete_ = false;
        listScroll_ = 0;
        top_.clear();
        bottom_.clear();
        ++rev_;
        return GlueAction::None;   // widgets of this screen exist after its first build
    }
    if (m.message != lastMessage_ || m.wait != lastWait_) { lastMessage_ = m.message; lastWait_ = m.wait; ++rev_; }
    // the character turns while L / R are held
    if (m.screen == GlueScreen::CharSelect || m.screen == GlueScreen::CharCreate) {
        if (in.held & kL) modelYaw += in.dt * 2.2f;
        if (in.held & kR) modelYaw -= in.dt * 2.2f;
    }
    // dialogs take every button
    if (!m.message.empty()) {
        if (in.down & (kA | kB)) { ++rev_; return GlueAction::DismissMessage; }
    } else if (!m.wait.empty()) {
        if ((in.down & (kA | kB)) && !m.waitFixed) { ++rev_; return GlueAction::CancelWait; }
    } else if (confirmDelete_) {
        if (in.down & kA) { confirmDelete_ = false; ++rev_; return GlueAction::DeleteCharacter; }
        if (in.down & kB) { confirmDelete_ = false; ++rev_; }
    } else {
        const int nChars = m.chars ? (int)m.chars->size() : 0;
        const int nRealms = m.realms ? (int)m.realms->size() : 0;
        switch (m.screen) {
            case GlueScreen::Login:
                if (in.down & kUp) moveFocus(0, -1);
                else if (in.down & kDown) moveFocus(0, 1);
                else if (in.down & kLeft) moveFocus(-1, 0);
                else if (in.down & kRight) moveFocus(1, 0);
                if (in.down & kA)
                    for (const Widget& w : top_)
                        if (w.focusable && focusKey(w) == focus_) return activate(w, m);
                if (in.down & kStart) return GlueAction::Quit;
                break;
            case GlueScreen::RealmList:
                if (nRealms) {
                    if (in.down & kUp) { realm = (realm + nRealms - 1) % nRealms; ++rev_; }
                    if (in.down & kDown) { realm = (realm + 1) % nRealms; ++rev_; }
                }
                if ((in.down & kA) && nRealms) { ++rev_; return GlueAction::RealmAccept; }
                if (in.down & kB) { ++rev_; return GlueAction::RealmCancel; }
                break;
            case GlueScreen::CharSelect:
                if (nChars) {
                    if (in.down & kUp) { character = (character + nChars - 1) % nChars; modelYaw = 0; ++rev_; }
                    if (in.down & kDown) { character = (character + 1) % nChars; modelYaw = 0; ++rev_; }
                }
                if ((in.down & (kA | kStart)) && nChars) { ++rev_; return GlueAction::EnterWorld; }
                if (in.down & kX) { ++rev_; return GlueAction::CreateNew; }
                if ((in.down & kY) && nChars) { confirmDelete_ = true; ++rev_; }
                if (in.down & kSelect) { ++rev_; return GlueAction::ChangeRealm; }
                if (in.down & kB) { ++rev_; return GlueAction::Back; }
                break;
            case GlueScreen::CharCreate: {
                const Widget* cur = nullptr;
                for (const Widget& w : top_)
                    if (w.focusable && focusKey(w) == focus_) cur = &w;
                if (cur && cur->kind == kLook && (in.down & (kLeft | kRight))) {   // a look row: left / right change it
                    stepLook(cur->arg / 2, (in.down & kRight) ? 1 : -1, m);
                } else {
                    if (in.down & kUp) moveFocus(0, -1);
                    else if (in.down & kDown) moveFocus(0, 1);
                    else if (in.down & kLeft) moveFocus(-1, 0);
                    else if (in.down & kRight) moveFocus(1, 0);
                }
                if ((in.down & kA) && cur) return activate(*cur, m);
                if (in.down & kX) randomize(m);
                if (in.down & kY) return GlueAction::EditName;
                if (in.down & kStart) return GlueAction::CreateAccept;
                if (in.down & kB) return GlueAction::CreateBack;
                break;
            }
        }
    }
    // the touch screen
    if (in.touchDown) {
        dragY_ = in.ty;
        dragScroll_ = infoScroll_;
        for (const Widget& w : bottom_)
            if (inside(in.tx, in.ty, w.x, w.y, w.w, w.h)) return activate(w, m);
    } else if (in.touching && dragY_ >= 0 && m.screen == GlueScreen::CharCreate) {   // drag the description
        const int s = std::clamp(dragScroll_ + (dragY_ - in.ty), 0, std::max(0, infoHeight_));
        if (s != infoScroll_) { infoScroll_ = s; ++rev_; }
    }
    if (!in.touching) dragY_ = -1;
    return GlueAction::None;
}

// =================================================================== drawing helpers
void GlueUi::backdrop(HudList& l, int w, int h, const GlueModel& m) const {
    if (m.sceneDrawn && w == kHudW) return;   // the 3D scene is already there
    // a dark gradient (the scene's colours when there is none: deep blue, the portal's green glow low)
    const int bands = 12;
    for (int i = 0; i < bands; ++i) {
        const float t = (float)i / (bands - 1);
        l.rect(0, h * i / bands, w, h * (i + 1) / bands - h * i / bands + 1,
               hudRgba((int)(10 + 14 * t), (int)(14 + 30 * t), (int)(30 + 10 * t)));
    }
}

void GlueUi::panel(HudList& l, int x, int y, int w, int h, int alpha) const {
    l.rect(x + 3, y + 3, w - 6, h - 6, hudRgba(0, 0, 0, alpha));
    Image b = assets().sprite("glue_border");
    if (!b.ok()) b = assets().sprite("dialog");
    if (b.ok()) l.nine(x, y, w, h, b, 0xFFFFFFFFu, false);
    else { l.outline(x, y, w, h, kFrame); l.outline(x + 1, y + 1, w - 2, h - 2, kFrameDim); }
}

void GlueUi::button(HudList& l, int x, int y, int w, int h, const std::string& text, bool enabled, bool focus,
                    const char* hint) const {
    Image im = assets().sprite(enabled ? "glue_button" : "glue_button_off");
    if (!im.ok()) im = assets().sprite(enabled ? "button" : "button_off");
    if (im.ok()) l.nine(x, y, w, h, im);
    else {
        l.rect(x, y, w, h, enabled ? hudRgba(120, 16, 8, 235) : hudRgba(60, 40, 36, 235));
        l.outline(x, y, w, h, enabled ? kFrame : kFrameDim);
    }
    if (focus) {
        Image hl = assets().sprite("glue_button_hl");
        if (hl.ok()) l.nine(x, y, w, h, hl, hudRgba(255, 255, 255, 150));
        l.outline(x - 1, y - 1, w + 2, h + 2, kGlueGold);
    }
    Font f = h >= 20 ? Font::Normal : Font::Small;
    if (f == Font::Normal && HudList::width(text, Font::Normal) > w - 10) f = Font::Small;   // a long label: the small font
    const int ty = y + (h - HudList::lineHeight(f)) / 2;
    l.textCentered(x + w / 2, ty, HudList::fit(text, w - 8, f), enabled ? kGlueGold : kGlueDim, true, f);
    if (hint && enabled) l.text(x + w - 9, y + 1, hint, hudRgba(255, 255, 255, 170), true, Font::Small);
}

void GlueUi::field(HudList& l, int x, int y, int w, int h, const std::string& text, bool focus, bool password,
                   const char* placeholder) const {
    l.rect(x + 2, y + 2, w - 4, h - 4, hudRgba(0, 0, 0, 200));
    Image im = assets().sprite("editbox");
    if (im.ok()) l.nine(x, y, w, h, im);
    else l.outline(x, y, w, h, focus ? kGlueGold : hudRgba(130, 130, 130));
    std::string shown = password ? std::string(std::min<size_t>(text.size(), 16), '*') : text;
    const int ty = y + (h - HudList::lineHeight(Font::Normal)) / 2;
    if (shown.empty() && placeholder) l.text(x + 7, ty, placeholder, kGlueDim, false, Font::Small);
    else {
        int tw = l.text(x + 7, ty, HudList::fit(shown, w - 16, Font::Normal), kGlueText, true, Font::Normal);
        if (focus && std::fmod(blink_, 1.0f) < 0.5f) l.rect(x + 8 + tw, ty + 1, 1, HudList::lineHeight(Font::Normal) - 2, kGlueText);
    }
    if (focus) l.outline(x - 1, y - 1, w + 2, h + 2, kGlueGold);
}

void GlueUi::iconButton(HudList& l, int x, int y, int size, const char* iconName, const char* fallback, bool selected,
                        bool focus, bool enabled) const {
    Image im = assets().namedIcon(iconName);
    const uint32_t tint = enabled ? 0xFFFFFFFFu : hudRgba(90, 90, 90);
    if (im.ok()) l.image(x, y, size, size, im, tint);
    else {
        l.rect(x, y, size, size, enabled ? hudRgba(40, 34, 24, 230) : hudRgba(24, 22, 20, 230));
        l.textCentered(x + size / 2, y + (size - HudList::lineHeight(Font::Small)) / 2, HudList::fit(fallback, size - 2),
                       enabled ? kGlueText : kGlueDim, true, Font::Small);
    }
    Image bevel = assets().sprite("cc_bevel");
    if (bevel.ok()) l.image(x, y, size, size, bevel);
    if (selected) {
        Image chk = assets().sprite("cc_checked");
        if (chk.ok()) l.image(x, y, size, size, chk);
        l.outline(x - 1, y - 1, size + 2, size + 2, kGlueGold);
        l.outline(x - 2, y - 2, size + 4, size + 4, hudRgba(255, 209, 0, 140));
    }
    if (focus) {
        Image hl = assets().sprite("cc_highlight");
        if (hl.ok()) l.image(x, y, size, size, hl, hudRgba(255, 255, 255, 170));
        l.outline(x - 3, y - 3, size + 6, size + 6, kGlueText);
    }
}

// a wait or message dialog over the screen, and the delete question
void GlueUi::dialogs(HudList& l, int w, int h, const GlueModel& m, bool topScreen) {
    std::string text;
    const char* btn = nullptr;
    GlueAction act = GlueAction::None;
    int arg = 0;
    if (!m.message.empty()) { text = m.message; btn = tr("Okay", "Ок"); act = GlueAction::DismissMessage; }
    else if (!m.wait.empty()) { text = m.wait; if (!m.waitFixed) { btn = tr("Cancel", "Отмена"); act = GlueAction::CancelWait; } }
    else if (confirmDelete_ && m.chars && character < (int)m.chars->size()) {
        const w3n::Character& c = (*m.chars)[(size_t)character];
        text = std::string(tr("Delete ", "Удалить ")) + c.name + "?\n" + charLine(c);
    } else return;
    if (!topScreen) bottom_.clear();   // modal: only the dialog's buttons answer
    l.rect(0, 0, w, h, hudRgba(0, 0, 0, 110));
    const int dw = std::min(w - 20, 300);
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        for (auto& s : HudList::wrap(text.substr(at, e - at), dw - 24, Font::Normal)) lines.push_back(s);
        at = e + 1;
    }
    const int lh = HudList::lineHeight(Font::Normal);
    const int dh = 22 + (int)lines.size() * lh + (btn || confirmDelete_ ? 34 : 6);
    const int dx = (w - dw) / 2, dy = (h - dh) / 2;
    panel(l, dx, dy, dw, dh, 240);
    for (size_t i = 0; i < lines.size(); ++i)
        l.textCentered(w / 2, dy + 12 + (int)i * lh, lines[i], kGlueGold, true, Font::Normal);
    const int by = dy + dh - 30;
    if (confirmDelete_ && m.message.empty() && m.wait.empty()) {
        button(l, w / 2 - 92, by, 84, 22, tr("Okay", "Ок"), true, false, topScreen ? "A" : nullptr);
        button(l, w / 2 + 8, by, 84, 22, tr("Cancel", "Отмена"), true, false, topScreen ? "B" : nullptr);
        if (!topScreen) {
            addBottom(w / 2 - 92, by, 84, 22, GlueAction::DeleteCharacter, 0, kButton);
            addBottom(w / 2 + 8, by, 84, 22, GlueAction::Back, 7, kButton);
        }
        return;
    }
    if (btn) {
        button(l, w / 2 - 50, by, 100, 22, btn, true, topScreen, topScreen ? "A" : nullptr);
        if (!topScreen) addBottom(w / 2 - 50, by, 100, 22, act, arg, kButton);
    }
}

// =================================================================== top screen
void GlueUi::buildTop(HudList& l, const GlueModel& m) {
    l.clear();
    top_.clear();
    backdrop(l, kHudW, kHudH, m);
    switch (m.screen) {
        case GlueScreen::Login: topLogin(l, m); break;
        case GlueScreen::RealmList: topRealms(l, m); break;
        case GlueScreen::CharSelect: topChars(l, m); break;
        case GlueScreen::CharCreate: topCreate(l, m); break;
    }
    dialogs(l, kHudW, kHudH, m, true);
}

void GlueUi::topLogin(HudList& l, const GlueModel& m) {
    Image logo = assets().sprite("glue_logo");
    if (logo.ok()) l.image(4, 2, logo.w, logo.h, logo);
    else {
        l.text(10, 10, "World of Warcraft", kGlueGold, true, Font::Large);
    }
    const int bx = 125, bw = 150;
    struct F { const char* label; const std::string* v; GlueAction a; bool pw; const char* ph; };
    static const std::string kHidden = "********";
    static const std::string kEmpty;
    const F fs[3] = {{tr("Server", "Сервер"), &m.server, GlueAction::EditServer, false, "host:port"},
                     {tr("Account Name", "Учётная запись"), &m.account, GlueAction::EditAccount, false, nullptr},
                     {tr("Account Password", "Пароль"), m.havePassword ? &kHidden : &kEmpty, GlueAction::EditPassword, true,
                      nullptr}};
    for (int i = 0; i < 3; ++i) {
        const int y = 94 + i * 36;
        l.textCentered(200, y, fs[i].label, kGlueGold, true, Font::Small);
        field(l, bx, y + 11, bw, 21, *fs[i].v, focused(kField, fs[i].a, 0), fs[i].pw, fs[i].ph);
        addTop(bx, y + 11, bw, 21, fs[i].a, 0, kField);
    }
    button(l, 150, 204, 100, 24, tr("Login", "Вход"), true, focused(kButton, GlueAction::Login, 0));
    addTop(150, 204, 100, 24, GlueAction::Login, 0, kButton);
    button(l, 318, 186, 76, 20, tr("Offline", "Без сети"), true, focused(kButton, GlueAction::Offline, 0));
    addTop(318, 186, 76, 20, GlueAction::Offline, 0, kButton);
    button(l, 318, 210, 76, 22, tr("Quit", "Выход"), true, focused(kButton, GlueAction::Quit, 0));
    addTop(318, 210, 76, 22, GlueAction::Quit, 0, kButton);
    l.text(4, 226, "Version 1.12.1 (5875)", kGlueDim, true, Font::Small);
}

void GlueUi::topRealms(HudList& l, const GlueModel& m) {
    Image logo = assets().sprite("glue_logo");
    if (logo.ok()) l.image(4, 2, logo.w / 2, logo.h / 2, logo, hudRgba(255, 255, 255, 160));
    const int px = 14, py = 22, pw = 372, ph = 210;
    panel(l, px, py, pw, ph);
    l.textCentered(200, py + 8, tr("Realm Selection", "Выбор игрового мира"), kGlueGold, true, Font::Normal);
    const int cName = px + 14, cType = px + 170, cChars = px + 240, cPop = px + 300, hy = py + 28;
    l.text(cName, hy, tr("Realm Name", "Название"), kGlueGold, true, Font::Small);
    l.text(cType, hy, tr("Type", "Тип"), kGlueGold, true, Font::Small);
    l.text(cChars, hy, tr("Chars", "Персонажи"), kGlueGold, true, Font::Small);
    l.text(cPop, hy, tr("Population", "Население"), kGlueGold, true, Font::Small);
    l.rect(px + 10, hy + 13, pw - 20, 1, kFrameDim);
    const int rowH = 16, rows = 8, y0 = hy + 17;
    const int n = m.realms ? (int)m.realms->size() : 0;
    realm = std::clamp(realm, 0, std::max(0, n - 1));
    const int first = std::clamp(realm - rows + 1, 0, std::max(0, n - rows));
    for (int i = 0; i < rows && first + i < n; ++i) {
        const w3n::Realm& r = (*m.realms)[(size_t)(first + i)];
        const int y = y0 + i * rowH;
        const bool sel = first + i == realm;
        if (sel) { l.rect(px + 10, y - 1, pw - 20, rowH, hudRgba(80, 80, 140, 170)); l.outline(px + 10, y - 1, pw - 20, rowH, kGlueGold); }
        const uint32_t c = (r.flags & 0x02) ? kGlueDim : sel ? kGlueText : hudRgba(255, 230, 140);
        l.textFit(cName, y + 1, r.name, cType - cName - 6, c, true, Font::Small);
        l.text(cType, y + 1, realmType(r), (r.icon == 1 || r.icon == 8) ? hudRgba(255, 90, 70) : c, true, Font::Small);
        l.text(cChars + 18, y + 1, r.characters ? std::to_string(r.characters) : std::string(""), c, true, Font::Small);
        l.text(cPop, y + 1, realmPop(r), realmPopColor(r), true, Font::Small);
    }
    if (!n) l.textCentered(200, y0 + 30, tr("No realms", "Нет миров"), kGlueDim, true, Font::Normal);
    button(l, 110, py + ph - 30, 84, 22, tr("Accept", "Принять"), n > 0, false, "A");
    button(l, 206, py + ph - 30, 84, 22, tr("Cancel", "Отмена"), true, false, "B");
}

void GlueUi::topChars(HudList& l, const GlueModel& m) {
    const int n = m.chars ? (int)m.chars->size() : 0;
    character = std::clamp(character, 0, std::max(0, n - 1));
    // realm name and Change Realm (top right)
    const int lx = 260, lw = 136;
    l.textCentered(lx + lw / 2, 2, HudList::fit(m.realmName, lw, Font::Small), kGlueGold, true, Font::Small);
    button(l, lx + 8, 14, lw - 16, 18, tr("Change Realm", "Сменить мир"), true, false, "SEL");
    // the character list
    const int py = 36, ph = 152, rowH = 34, rows = (ph - 8) / rowH;
    panel(l, lx, py, lw, ph);
    const int first = std::clamp(character - rows + 1, 0, std::max(0, n - rows));
    for (int i = 0; i < rows && first + i < n; ++i) {
        const w3n::Character& c = (*m.chars)[(size_t)(first + i)];
        const int y = py + 5 + i * rowH;
        const bool sel = first + i == character;
        if (sel) {
            l.rect(lx + 4, y, lw - 8, rowH - 1, hudRgba(120, 100, 20, 120));
            l.outline(lx + 4, y, lw - 8, rowH - 1, kGlueGold);
        }
        l.textFit(lx + 9, y + 1, c.name, lw - 16, sel ? kGlueText : kGlueGold, true, Font::Normal);
        l.textFit(lx + 9, y + 13, charLine(c), lw - 16, kGlueText, true, Font::Small);
        l.textFit(lx + 9, y + 23, zoneOf(m, c), lw - 16, kGlueDim, false, Font::Small);
    }
    if (!n) l.textCentered(lx + lw / 2, py + 40, tr("No characters", "Нет персонажей"), kGlueDim, true, Font::Small);
    button(l, lx + 4, 192, lw - 8, 20, tr("Create New Character", "Создать персонажа"), true, false, "X");
    button(l, lx + 4, 215, (lw - 12) / 2, 22, tr("Delete", "Удалить"), n > 0, false, "Y");
    button(l, lx + 8 + (lw - 12) / 2, 215, (lw - 12) / 2, 22, tr("Back", "Назад"), true, false, "B");
    // Enter World, under the character
    button(l, 74, 212, 112, 24, tr("Enter World", "Войти в мир"), n > 0, true, "A");
    l.text(6, 226, tr("L / R: turn", "L / R: поворот"), kGlueDim, true, Font::Small);
}

void GlueUi::topCreate(HudList& l, const GlueModel& m) {
    int nr = 0, nc = 0;
    const glue::RaceInfo* rs = glue::races(&nr);
    const glue::ClassInfo* cs = glue::classes(&nc);
    // left: races (Alliance, Horde), sex, Randomize, Back
    panel(l, 0, 0, 100, 240, 170);
    l.textCentered(25, 4, tr("Alliance", "Альянс"), kAllianceBlue, true, Font::Small);
    l.textCentered(73, 4, tr("Horde", "Орда"), kHordeRed, true, Font::Small);
    int ai = 0, hi = 0;
    for (int i = 0; i < nr; ++i) {
        const glue::RaceInfo& r = rs[i];
        const bool alliance = r.faction == glue::Faction::Alliance;
        const int x = alliance ? 10 : 58, y = 16 + (alliance ? ai++ : hi++) * 32;
        char icon[16];
        snprintf(icon, sizeof icon, "race_%d", r.id * 2 + create.sex);
        iconButton(l, x, y, 30, icon, g_lang ? r.ru : r.en, create.race == r.id, focused(kRace, GlueAction::None, r.id), true);
        addTop(x, y, 30, 30, GlueAction::None, r.id, kRace);
    }
    for (int s = 0; s < 2; ++s) {
        const int x = s ? 56 : 16;
        char icon[12];
        snprintf(icon, sizeof icon, "gender_%d", s);
        iconButton(l, x, 148, 28, icon, s ? tr("Female", "Жен.") : tr("Male", "Муж."), create.sex == s,
                   focused(kSex, GlueAction::None, s), true);
        addTop(x, 148, 28, 28, GlueAction::None, s, kSex);
    }
    l.textCentered(50, 178, create.sex ? tr("Female", "Женский") : tr("Male", "Мужской"), kGlueGold, true, Font::Small);
    button(l, 6, 190, 88, 20, tr("Randomize", "Случайно"), true, focused(kButton, GlueAction::None, 1), "X");
    addTop(6, 190, 88, 20, GlueAction::None, 1, kButton);
    button(l, 6, 214, 88, 22, tr("Back", "Назад"), true, focused(kButton, GlueAction::CreateBack, 0), "B");
    addTop(6, 214, 88, 22, GlueAction::CreateBack, 0, kButton);
    // right: classes, the look, Accept
    panel(l, 300, 0, 100, 240, 170);
    for (int i = 0; i < nc; ++i) {
        const glue::ClassInfo& c = cs[i];
        const int x = 306 + (i % 3) * 31, y = 6 + (i / 3) * 31;
        char icon[16];
        snprintf(icon, sizeof icon, "class_%d", c.id);
        const bool ok = glue::allowed(create.race, c.id);
        iconButton(l, x, y, 27, icon, g_lang ? c.ru : c.en, create.cls == c.id, focused(kClass, GlueAction::None, c.id), ok);
        addTop(x, y, 27, 27, GlueAction::None, c.id, kClass);
    }
    const uint8_t vals[5] = {create.skin, create.face, create.hairStyle, create.hairColor, create.facial};
    const std::vector<uint8_t>* lists[5] = {&m.appearance.skins, &m.appearance.faces, &m.appearance.hairStyles,
                                            &m.appearance.hairColors, &m.appearance.facials};
    for (int k = 0; k < 5; ++k) {
        const int y = 99 + k * 22;
        const bool f = focused(kLook, GlueAction::None, k * 2);
        if (f) l.rect(302, y - 1, 96, 21, hudRgba(120, 100, 20, 120));
        Image left = assets().sprite("glue_left"), right = assets().sprite("glue_right");
        if (left.ok()) l.image(303, y, 18, 18, left); else l.text(306, y + 3, "<", kGlueGold, true, Font::Normal);
        if (right.ok()) l.image(379, y, 18, 18, right); else l.text(386, y + 3, ">", kGlueGold, true, Font::Normal);
        l.textCentered(350, y - 1, HudList::fit(lookLabel(k), 56, Font::Small), f ? kGlueText : kGlueGold, true, Font::Small);
        char b[16];
        if (m.appearanceKnown) {
            const auto& v = *lists[k];
            auto it = std::find(v.begin(), v.end(), vals[k]);
            snprintf(b, sizeof b, "%d/%d", it == v.end() ? 1 : (int)(it - v.begin()) + 1, (int)v.size());
        } else snprintf(b, sizeof b, "...");
        l.textCentered(350, y + 9, b, kGlueDim, false, Font::Small);
        if (f) l.outline(302, y - 1, 96, 21, kGlueGold);
        addTop(302, y - 1, 96, 21, GlueAction::None, k * 2, kLook);
    }
    button(l, 306, 212, 88, 24, tr("Accept", "Принять"), !create.name.empty(), focused(kButton, GlueAction::CreateAccept, 0),
           "ST");
    addTop(306, 212, 88, 24, GlueAction::CreateAccept, 0, kButton);
    // centre: race and class, the name
    std::string title = std::string(glue::raceName(create.race, g_lang)) + " " + glue::className(create.cls, g_lang);
    l.textCentered(200, 3, title, kGlueGold, true, Font::Normal);
    l.textCentered(200, 186, tr("Name", "Имя"), kGlueGold, true, Font::Small);
    field(l, 130, 197, 140, 21, create.name, focused(kField, GlueAction::EditName, 0), false, tr("Y: enter a name", "Y: введите имя"));
    addTop(130, 197, 140, 21, GlueAction::EditName, 0, kField);
    l.textCentered(200, 225, tr("L / R: turn", "L / R: поворот"), kGlueDim, true, Font::Small);
}

// =================================================================== touch screen
void GlueUi::buildBottom(HudList& l, const GlueModel& m) {
    l.clear();
    bottom_.clear();
    l.rect(0, 0, kTouchW, kTouchH, hudRgba(12, 10, 8));
    switch (m.screen) {
        case GlueScreen::Login: bottomLogin(l, m); break;
        case GlueScreen::RealmList: bottomRealms(l, m); break;
        case GlueScreen::CharSelect: bottomChars(l, m); break;
        case GlueScreen::CharCreate: bottomCreate(l, m); break;
    }
    dialogs(l, kTouchW, kTouchH, m, false);
}

void GlueUi::bottomLogin(HudList& l, const GlueModel& m) {
    l.textCentered(160, 4, tr("Account Login", "Вход в игру"), kGlueGold, true, Font::Normal);
    struct R { const char* label; std::string v; GlueAction a; };
    const R rows[3] = {{tr("Server", "Сервер"), m.server, GlueAction::EditServer},
                       {tr("Account", "Учётная запись"), m.account.empty() ? tr("(tap to enter)", "(нажмите, чтобы ввести)") : m.account,
                        GlueAction::EditAccount},
                       {tr("Password", "Пароль"), m.havePassword ? "********" : tr("(tap to enter)", "(нажмите, чтобы ввести)"),
                        GlueAction::EditPassword}};
    for (int i = 0; i < 3; ++i) {
        const int y = 24 + i * 34;
        panel(l, 8, y, 304, 30, 200);
        l.text(16, y + 4, rows[i].label, kGlueGold, true, Font::Small);
        l.textFit(16, y + 15, rows[i].v, 288, kGlueText, true, Font::Normal);
        addBottom(8, y, 304, 30, rows[i].a, 0, kButton);
    }
    button(l, 90, 130, 140, 30, tr("Login", "Войти"), true, false);
    addBottom(90, 130, 140, 30, GlueAction::Login, 0, kButton);
    char p[32], g[32];
    snprintf(p, sizeof p, "%s %d", tr("Protocol", "Протокол"), m.protocol);
    snprintf(g, sizeof g, "%s %s", tr("Log", "Журнал"), m.packetLog ? tr("on", "вкл") : tr("off", "выкл"));
    button(l, 8, 172, 98, 24, tr("Offline", "Без сети"), true, false);
    addBottom(8, 172, 98, 24, GlueAction::Offline, 0, kButton);
    button(l, 111, 172, 98, 24, p, true, false);
    addBottom(111, 172, 98, 24, GlueAction::ToggleProtocol, 0, kButton);
    button(l, 214, 172, 98, 24, g, true, false);
    addBottom(214, 172, 98, 24, GlueAction::ToggleLog, 0, kButton);
    button(l, 214, 208, 98, 24, tr("Quit", "Выход"), true, false);
    addBottom(214, 208, 98, 24, GlueAction::Quit, 0, kButton);
    l.text(8, 206, tr("D-pad, A: the top screen", "Крестовина, A: верхний экран"), kGlueDim, false, Font::Small);
    l.text(8, 218, tr("The password is never saved", "Пароль не сохраняется"), kGlueDim, false, Font::Small);
}

void GlueUi::bottomRealms(HudList& l, const GlueModel& m) {
    l.textCentered(160, 4, tr("Realm Selection", "Выбор игрового мира"), kGlueGold, true, Font::Normal);
    const int n = m.realms ? (int)m.realms->size() : 0;
    const int rowH = 30, rows = 5, y0 = 22;
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, n - rows));
    if (realm < listScroll_) listScroll_ = realm;
    if (realm >= listScroll_ + rows) listScroll_ = realm - rows + 1;
    for (int i = 0; i < rows && listScroll_ + i < n; ++i) {
        const int idx = listScroll_ + i;
        const w3n::Realm& r = (*m.realms)[(size_t)idx];
        const int y = y0 + i * rowH;
        const bool sel = idx == realm;
        l.rect(8, y, 284, rowH - 2, sel ? hudRgba(80, 80, 140, 200) : (i & 1) ? kRowAlt : kRow);
        if (sel) l.outline(8, y, 284, rowH - 2, kGlueGold);
        l.textFit(14, y + 2, r.name, 200, (r.flags & 2) ? kGlueDim : kGlueText, true, Font::Normal);
        std::string info = realmType(r);
        if (r.characters) info += std::string("  ") + std::to_string(r.characters) + tr(" chars", " перс.");
        l.text(14, y + 15, info, kGlueDim, false, Font::Small);
        l.textRight(286, y + 8, realmPop(r), realmPopColor(r), true, Font::Small);
        addBottom(8, y, 284, rowH - 2, GlueAction::None, idx, kRealmRow);
    }
    if (n > rows) {
        button(l, 296, y0, 20, 70, "^", listScroll_ > 0, false);
        addBottom(296, y0, 20, 70, GlueAction::None, 0, kScrollUp);
        button(l, 296, y0 + 76, 20, 70, "v", listScroll_ + rows < n, false);
        addBottom(296, y0 + 76, 20, 70, GlueAction::None, 0, kScrollDown);
    }
    button(l, 30, 200, 120, 30, tr("Accept", "Принять"), n > 0, false);
    addBottom(30, 200, 120, 30, GlueAction::RealmAccept, 0, kButton);
    button(l, 170, 200, 120, 30, tr("Cancel", "Отмена"), true, false);
    addBottom(170, 200, 120, 30, GlueAction::RealmCancel, 0, kButton);
}

void GlueUi::bottomChars(HudList& l, const GlueModel& m) {
    const int n = m.chars ? (int)m.chars->size() : 0;
    const int rowH = 36, rows = 5, y0 = 2;
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, n - rows));
    if (character < listScroll_) listScroll_ = character;
    if (character >= listScroll_ + rows) listScroll_ = character - rows + 1;
    for (int i = 0; i < rows && listScroll_ + i < n; ++i) {
        const int idx = listScroll_ + i;
        const w3n::Character& c = (*m.chars)[(size_t)idx];
        const int y = y0 + i * rowH;
        const bool sel = idx == character;
        l.rect(4, y, 288, rowH - 2, sel ? hudRgba(120, 100, 20, 200) : (i & 1) ? kRowAlt : kRow);
        if (sel) l.outline(4, y, 288, rowH - 2, kGlueGold);
        char icon[16];
        snprintf(icon, sizeof icon, "class_%d", c.cls);
        Image ci = assets().namedIcon(icon);
        if (ci.ok()) l.image(8, y + 3, 28, 28, ci);
        l.textFit(42, y + 2, c.name, 240, classColor(c.cls), true, Font::Normal);
        l.textFit(42, y + 15, charLine(c), 240, kGlueText, false, Font::Small);
        l.textFit(42, y + 24, zoneOf(m, c), 240, kGlueDim, false, Font::Small);
        addBottom(4, y, 288, rowH - 2, GlueAction::None, idx, kCharRow);
    }
    if (!n) l.textCentered(150, 60, tr("No characters on this realm yet", "В этом мире ещё нет персонажей"), kGlueDim, true, Font::Normal);
    if (n > rows) {
        button(l, 296, y0, 20, 86, "^", listScroll_ > 0, false);
        addBottom(296, y0, 20, 86, GlueAction::None, 0, kScrollUp);
        button(l, 296, y0 + 92, 20, 86, "v", listScroll_ + rows < n, false);
        addBottom(296, y0 + 92, 20, 86, GlueAction::None, 0, kScrollDown);
    }
    button(l, 4, 184, 154, 28, tr("Enter World", "Войти в мир"), n > 0, false);
    addBottom(4, 184, 154, 28, GlueAction::EnterWorld, 0, kButton);
    button(l, 162, 184, 154, 28, tr("Create New Character", "Создать персонажа"), true, false);
    addBottom(162, 184, 154, 28, GlueAction::CreateNew, 0, kButton);
    button(l, 4, 216, 100, 22, tr("Delete", "Удалить"), n > 0, false);
    if (n) addBottom(4, 216, 100, 22, GlueAction::DeleteCharacter, 0, kButton);
    button(l, 110, 216, 100, 22, tr("Change Realm", "Сменить мир"), true, false);
    addBottom(110, 216, 100, 22, GlueAction::ChangeRealm, 0, kButton);
    button(l, 216, 216, 100, 22, tr("Back", "Назад"), true, false);
    addBottom(216, 216, 100, 22, GlueAction::Back, 0, kButton);
}

void GlueUi::bottomCreate(HudList& l, const GlueModel& m) {
    // tabs
    const char* tabs[2] = {tr("Race", "Раса"), tr("Class", "Класс")};
    for (int t = 0; t < 2; ++t) {
        const int x = 4 + t * 158;
        l.rect(x, 2, 154, 22, infoTab_ == t ? hudRgba(120, 100, 20, 220) : kRow);
        l.outline(x, 2, 154, 22, infoTab_ == t ? kGlueGold : kFrameDim);
        l.textCentered(x + 77, 6, tabs[t], infoTab_ == t ? kGlueText : kGlueGold, true, Font::Normal);
        addBottom(x, 2, 154, 22, GlueAction::None, t, kTab);
    }
    // the text, scrolled by dragging
    const int top = 28, bottom = kTouchH - 2, w = kTouchW - 16;
    l.clip(0, top, kTouchW, bottom - top);
    int y = top + 2 - infoScroll_;
    const int lh = HudList::lineHeight(Font::Small), lhN = HudList::lineHeight(Font::Normal);
    char icon[16];
    std::vector<std::string> paras;
    std::string traitsTitle;
    if (infoTab_ == 0) {
        const glue::RaceInfo* r = glue::race(create.race);
        snprintf(icon, sizeof icon, "race_%d", create.race * 2 + create.sex);
        Image im = assets().namedIcon(icon);
        if (im.ok()) l.image(8, y, 32, 32, im);
        l.text(46, y + 2, glue::raceName(create.race, g_lang), kGlueGold, true, Font::Large);
        if (r) l.text(46, y + 2 + HudList::lineHeight(Font::Large),
                      r->faction == glue::Faction::Alliance ? tr("Alliance", "Альянс") : tr("Horde", "Орда"),
                      r->faction == glue::Faction::Alliance ? kAllianceBlue : kHordeRed, true, Font::Small);
        y += 38;
        static const glue::Texts kNone;
        paras.push_back(glue::raceDescription(m.texts ? *m.texts : kNone, create.race, g_lang));
        if (r) { traitsTitle = tr("Racial traits", "Расовые способности"); paras.push_back(g_lang ? r->traitsRu : r->traitsEn); }
    } else {
        const glue::ClassInfo* c = glue::cls(create.cls);
        snprintf(icon, sizeof icon, "class_%d", create.cls);
        Image im = assets().namedIcon(icon);
        if (im.ok()) l.image(8, y, 32, 32, im);
        l.text(46, y + 2, glue::className(create.cls, g_lang), classColor(create.cls), true, Font::Large);
        y += 38;
        static const glue::Texts kNone;
        if (c) paras.push_back(g_lang ? c->roleRu : c->roleEn);
        paras.push_back(glue::classDescription(m.texts ? *m.texts : kNone, create.cls, g_lang));
        std::string who;
        int nr = 0;
        const glue::RaceInfo* rs = glue::races(&nr);
        for (int i = 0; i < nr; ++i)
            if (glue::allowed(rs[i].id, create.cls)) who += (who.empty() ? "" : ", ") + std::string(g_lang ? rs[i].ru : rs[i].en);
        traitsTitle = tr("Races", "Расы");
        paras.push_back(who);
    }
    for (size_t p = 0; p < paras.size(); ++p) {
        const bool traits = p + 1 == paras.size() && !traitsTitle.empty();
        if (traits) {
            y += 4;
            l.text(8, y, traitsTitle, kGlueGold, true, Font::Normal);
            y += lhN;
        }
        const bool bullets = traits && infoTab_ == 0;   // racial traits: one per line
        size_t at = 0;
        const std::string& s = paras[p];
        while (at <= s.size()) {
            size_t e = s.find('\n', at);
            if (e == std::string::npos) e = s.size();
            const std::vector<std::string> wl = HudList::wrap(s.substr(at, e - at), w - (bullets ? 12 : 0), Font::Small);
            for (size_t k = 0; k < wl.size(); ++k) {
                if (bullets && k == 0) l.rect(10, y + lh / 2 - 1, 3, 3, kGlueGold);
                l.text(bullets ? 18 : 8, y, wl[k], kGlueText, false, Font::Small);
                y += lh;
            }
            at = e + 1;
        }
        y += 4;
    }
    l.noClip();
    infoHeight_ = std::max(0, y + infoScroll_ - bottom + 4);
    if (infoHeight_ > 0) {   // scroll bar
        const int trackH = bottom - top, barH = std::max(16, trackH * trackH / (trackH + infoHeight_));
        const int by = top + (trackH - barH) * infoScroll_ / std::max(1, infoHeight_);
        l.rect(kTouchW - 5, top, 3, trackH, hudRgba(60, 50, 30));
        l.rect(kTouchW - 5, by, 3, barH, kGlueGold);
    }
}
