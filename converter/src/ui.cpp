// Interface pack (UI stage 7): ui.w3dp for the 3DS game UI, made from the user's own client.
//
//   icons    Interface\Icons of every spell (SpellIcon.dbc via Spell.dbc) and item display (ItemDisplayInfo.dbc),
//            plus the named icons of the skin config (tab pictures, paper doll slots, class icons); border trimmed,
//            resampled to iconSize, edge-replicated into iconSize+2 cells of 512x512 ETC1 pages
//   skin     the sprites of the config (9-slice frames from tooltip / dialog borders, buttons, the status bar
//            texture, gossip / quest / money icons, a map arrow in 16 rotations) in RGBA4 pages
//   fonts    the client's Fonts\FRIZQT__.TTF (or --ui-font) at the normal / large sizes, missing glyphs (Cyrillic)
//            from fallback fonts, an A8 atlas; the small size stays the 3DS built-in font unless --ui-font-small
//   maps     every WorldMapArea zone / continent: the 12 tiles of Interface\WorldMap\<name>\ with all overlays
//            ("explored"), cropped to 1002x668 and resampled to mapWidth x 2/3 in a 512x256 ETC1 texture
// Format: common/w3d_ui.h. The skin config format: converter/ui_skin.txt (the built-in default is the same text).
#include "conv.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "w3d_ui.h"

namespace w3d {

std::vector<uint8_t> encodeTextureLevel(const Image& im, uint8_t fmt);   // texture.cpp

namespace {

const char* kDefaultSkin = R"(# wow3ds interface skin: what ui.w3dp takes from the client (UI stage 7). Paths are game paths; "a|b" = the first
# that exists. Crops are fractions of the source picture (u0 v0 u1 v1), sizes / margins are output pixels.
#   edge   NAME PATH SIZE                        backdrop edge file (8 squares) -> 9-slice frame, corners SIZE px
#   nine   NAME PATH u0 v0 u1 v1 W H  L T R B    a crop stretched to W x H with 9-slice margins
#   sprite NAME PATH u0 v0 u1 v1 W H             a crop resampled to W x H
#   arrow  NAME PATH SIZE                        16 rotations NAME0..NAME15 (step 22.5 degrees counter-clockwise)
#   icon   NAME PATH [u0 v0 u1 v1]               a named icon in the icon atlas (Interface\Icons are trimmed)
#   grid   NAME PATH COLS ROWS ID:C,R ...        cells of a sheet as icons NAME_ID
edge   panel        Interface\Tooltips\UI-Tooltip-Border.blp 8
edge   dialog       Interface\DialogFrame\UI-DialogBox-Border.blp 12
nine   button       Interface\Buttons\UI-Panel-Button-Up.blp 0 0 0.625 0.6875 80 22 10 6 10 6
nine   button_down  Interface\Buttons\UI-Panel-Button-Down.blp 0 0 0.625 0.6875 80 22 10 6 10 6
nine   button_off   Interface\Buttons\UI-Panel-Button-Disabled.blp 0 0 0.625 0.6875 80 22 10 6 10 6
sprite slot         Interface\Buttons\UI-Quickslot2.blp 0 0 1 1 64 64
sprite slot_empty   Interface\PaperDoll\UI-Backpack-EmptySlot.blp 0 0 1 1 40 40
sprite bar          Interface\TargetingFrame\UI-StatusBar.blp 0 0 1 1 64 8
sprite gossip_0     Interface\GossipFrame\GossipGossipIcon.blp 0 0 1 1 16 16
sprite gossip_1     Interface\GossipFrame\VendorGossipIcon.blp 0 0 1 1 16 16
sprite gossip_2     Interface\GossipFrame\TaxiGossipIcon.blp 0 0 1 1 16 16
sprite gossip_3     Interface\GossipFrame\TrainerGossipIcon.blp 0 0 1 1 16 16
sprite gossip_4     Interface\GossipFrame\HealerGossipIcon.blp|Interface\GossipFrame\GossipGossipIcon.blp 0 0 1 1 16 16
sprite gossip_5     Interface\GossipFrame\BinderGossipIcon.blp 0 0 1 1 16 16
sprite gossip_6     Interface\GossipFrame\BankerGossipIcon.blp 0 0 1 1 16 16
sprite gossip_7     Interface\GossipFrame\PetitionGossipIcon.blp 0 0 1 1 16 16
sprite gossip_8     Interface\GossipFrame\TabardGossipIcon.blp 0 0 1 1 16 16
sprite gossip_9     Interface\GossipFrame\BattleMasterGossipIcon.blp 0 0 1 1 16 16
sprite quest_new    Interface\GossipFrame\AvailableQuestIcon.blp 0 0 1 1 16 16
sprite quest_done   Interface\GossipFrame\ActiveQuestIcon.blp 0 0 1 1 16 16
sprite coin_gold    Interface\MoneyFrame\UI-GoldIcon.blp 0 0 1 1 12 12
sprite coin_silver  Interface\MoneyFrame\UI-SilverIcon.blp 0 0 1 1 12 12
sprite coin_copper  Interface\MoneyFrame\UI-CopperIcon.blp 0 0 1 1 12 12
sprite check        Interface\Buttons\UI-CheckBox-Check.blp 0 0 1 1 16 16
arrow  arrow        Interface\Minimap\MinimapArrow.blp 16
icon   tab_combat   Interface\Icons\Ability_DualWield.blp|Interface\Icons\INV_Sword_04.blp
icon   tab_map      Interface\Icons\INV_Misc_Map_01.blp|Interface\Icons\INV_Misc_Map_02.blp
icon   tab_bags     Interface\Icons\INV_Misc_Bag_08.blp|Interface\Icons\INV_Misc_Bag_07.blp
icon   tab_spells   Interface\Icons\INV_Misc_Book_09.blp|Interface\Icons\INV_Misc_Book_11.blp
icon   tab_quests   Interface\Icons\INV_Misc_Note_01.blp|Interface\Icons\INV_Scroll_03.blp
icon   tab_chat     Interface\Icons\INV_Letter_01.blp|Interface\Icons\INV_Misc_Note_02.blp
icon   tab_social   Interface\Icons\Spell_Holy_PrayerOfFortitude.blp|Interface\Icons\INV_Misc_Head_Human_01.blp
icon   tab_settings Interface\Icons\INV_Misc_Gear_01.blp|Interface\Icons\Trade_Engineering.blp
icon   talents      Interface\Icons\Ability_Marksmanship.blp|Interface\Icons\INV_Misc_Book_07.blp
icon   attack       Interface\Icons\Ability_MeleeDamage.blp|Interface\Icons\INV_Sword_04.blp
icon   money        Interface\Icons\INV_Misc_Coin_01.blp
icon   unknown      Interface\Icons\INV_Misc_QuestionMark.blp
icon   slot_0       Interface\PaperDoll\UI-PaperDoll-Slot-Head.blp
icon   slot_1       Interface\PaperDoll\UI-PaperDoll-Slot-Neck.blp
icon   slot_2       Interface\PaperDoll\UI-PaperDoll-Slot-Shoulder.blp
icon   slot_3       Interface\PaperDoll\UI-PaperDoll-Slot-Shirt.blp
icon   slot_4       Interface\PaperDoll\UI-PaperDoll-Slot-Chest.blp
icon   slot_5       Interface\PaperDoll\UI-PaperDoll-Slot-Waist.blp
icon   slot_6       Interface\PaperDoll\UI-PaperDoll-Slot-Legs.blp
icon   slot_7       Interface\PaperDoll\UI-PaperDoll-Slot-Feet.blp
icon   slot_8       Interface\PaperDoll\UI-PaperDoll-Slot-Wrists.blp
icon   slot_9       Interface\PaperDoll\UI-PaperDoll-Slot-Hands.blp
icon   slot_10      Interface\PaperDoll\UI-PaperDoll-Slot-Finger.blp
icon   slot_11      Interface\PaperDoll\UI-PaperDoll-Slot-Finger.blp
icon   slot_12      Interface\PaperDoll\UI-PaperDoll-Slot-Trinket.blp
icon   slot_13      Interface\PaperDoll\UI-PaperDoll-Slot-Trinket.blp
icon   slot_14      Interface\PaperDoll\UI-PaperDoll-Slot-Chest.blp|Interface\PaperDoll\UI-Backpack-EmptySlot.blp
icon   slot_15      Interface\PaperDoll\UI-PaperDoll-Slot-MainHand.blp
icon   slot_16      Interface\PaperDoll\UI-PaperDoll-Slot-SecondaryHand.blp
icon   slot_17      Interface\PaperDoll\UI-PaperDoll-Slot-Ranged.blp
icon   slot_18      Interface\PaperDoll\UI-PaperDoll-Slot-Tabard.blp
icon   slot_19      Interface\PaperDoll\UI-PaperDoll-Slot-Bag.blp|Interface\PaperDoll\UI-Backpack-EmptySlot.blp
grid   class        Interface\Glues\CharacterCreate\UI-CharacterCreate-Classes.blp 4 4 1:0,0 8:1,0 4:2,0 11:3,0 3:0,1 7:1,1 5:2,1 9:3,1 2:0,2
)";

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// The first of "a|b|c" that decodes.
bool loadBlp(const DataDir& data, const std::string& paths, Image& out, std::string* used = nullptr) {
    std::stringstream ss(paths);
    std::string p;
    while (std::getline(ss, p, '|')) {
        std::vector<uint8_t> bytes;
        if (data.read(p, bytes) && decodeBlp(bytes, out) && out.w > 0 && out.h > 0) {
            if (used) *used = p;
            return true;
        }
    }
    return false;
}

Image crop(const Image& s, float u0, float v0, float u1, float v1) {
    int x0 = std::clamp((int)std::lround(u0 * s.w), 0, s.w), x1 = std::clamp((int)std::lround(u1 * s.w), 0, s.w);
    int y0 = std::clamp((int)std::lround(v0 * s.h), 0, s.h), y1 = std::clamp((int)std::lround(v1 * s.h), 0, s.h);
    Image d;
    d.w = std::max(1, x1 - x0);
    d.h = std::max(1, y1 - y0);
    d.rgba.assign((size_t)d.w * d.h * 4, 0);
    for (int y = 0; y < d.h && y0 + y < s.h; ++y)
        std::memcpy(&d.rgba[(size_t)y * d.w * 4], &s.rgba[((size_t)(y0 + y) * s.w + x0) * 4], (size_t)std::min(d.w, s.w - x0) * 4);
    return d;
}

// Area resample with fractional coverage, alpha-weighted colour (no dark fringes at transparent edges).
Image resample(const Image& s, int w, int h) {
    Image d;
    d.w = w;
    d.h = h;
    d.rgba.assign((size_t)w * h * 4, 0);
    const double sx = (double)s.w / w, sy = (double)s.h / h;
    for (int y = 0; y < h; ++y) {
        double fy0 = y * sy, fy1 = (y + 1) * sy;
        for (int x = 0; x < w; ++x) {
            double fx0 = x * sx, fx1 = (x + 1) * sx;
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int yy = (int)fy0; yy < std::min(s.h, (int)std::ceil(fy1)); ++yy) {
                double wy = std::min<double>(yy + 1, fy1) - std::max<double>(yy, fy0);
                if (wy <= 0) continue;
                for (int xx = (int)fx0; xx < std::min(s.w, (int)std::ceil(fx1)); ++xx) {
                    double wx = std::min<double>(xx + 1, fx1) - std::max<double>(xx, fx0);
                    if (wx <= 0) continue;
                    const uint8_t* p = &s.rgba[((size_t)yy * s.w + xx) * 4];
                    double wgt = wx * wy, a = p[3] / 255.0;
                    acc[0] += p[0] * a * wgt; acc[1] += p[1] * a * wgt; acc[2] += p[2] * a * wgt;
                    acc[3] += a * wgt;
                    wsum += wgt;
                }
            }
            uint8_t* o = &d.rgba[((size_t)y * w + x) * 4];
            if (wsum > 0 && acc[3] > 1e-6) {
                for (int c = 0; c < 3; ++c) o[c] = (uint8_t)std::clamp(acc[c] / acc[3] + 0.5, 0.0, 255.0);
                o[3] = (uint8_t)std::clamp(acc[3] / wsum * 255.0 + 0.5, 0.0, 255.0);
            }
        }
    }
    return d;
}

// Rotation about the centre (bilinear), counter-clockwise by 'rad' on screen.
Image rotate(const Image& s, double rad) {
    Image d;
    d.w = s.w;
    d.h = s.h;
    d.rgba.assign(s.rgba.size(), 0);
    const double c = std::cos(rad), sn = std::sin(rad), cx = s.w / 2.0, cy = s.h / 2.0;
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double fx = c * dx - sn * dy + cx - 0.5, fy = sn * dx + c * dy + cy - 0.5;   // inverse of a CCW turn (y down)
            int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
            double ax = fx - x0, ay = fy - y0, acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                double wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                if (xx < 0 || yy < 0 || xx >= s.w || yy >= s.h) continue;
                const uint8_t* p = &s.rgba[((size_t)yy * s.w + xx) * 4];
                double a = p[3] / 255.0;
                for (int ch = 0; ch < 3; ++ch) acc[ch] += p[ch] * a * wgt;
                acc[3] += a * wgt;
            }
            uint8_t* o = &d.rgba[((size_t)y * d.w + x) * 4];
            if (acc[3] > 1e-6)
                for (int ch = 0; ch < 3; ++ch) o[ch] = (uint8_t)std::clamp(acc[ch] / acc[3] + 0.5, 0.0, 255.0);
            o[3] = (uint8_t)std::clamp(acc[3] * 255.0 + 0.5, 0.0, 255.0);
        }
    return d;
}

void blit(Image& d, const Image& s, int x, int y) {
    for (int j = 0; j < s.h; ++j)
        for (int i = 0; i < s.w; ++i) {
            int tx = x + i, ty = y + j;
            if (tx < 0 || ty < 0 || tx >= d.w || ty >= d.h) continue;
            std::memcpy(&d.rgba[((size_t)ty * d.w + tx) * 4], &s.rgba[((size_t)j * s.w + i) * 4], 4);
        }
}

// Alpha "over" (overlays on a map).
void blend(Image& d, const Image& s, int x, int y) {
    for (int j = 0; j < s.h; ++j)
        for (int i = 0; i < s.w; ++i) {
            int tx = x + i, ty = y + j;
            if (tx < 0 || ty < 0 || tx >= d.w || ty >= d.h) continue;
            const uint8_t* p = &s.rgba[((size_t)j * s.w + i) * 4];
            uint8_t* o = &d.rgba[((size_t)ty * d.w + tx) * 4];
            int a = p[3];
            for (int c = 0; c < 3; ++c) o[c] = (uint8_t)((p[c] * a + o[c] * (255 - a)) / 255);
            o[3] = (uint8_t)std::max<int>(o[3], a);
        }
}

// Backdrop edge file: 8 squares left to right = left, right, top, bottom edge (the top and bottom ones rotated by
// 90 degrees: their texel u runs along the frame), top-left, top-right, bottom-left, bottom-right corners.
Image edgeFrame(const Image& strip, int size) {
    const int n = std::max(1, strip.h);   // square side in the file
    auto part = [&](int k) { return crop(strip, (float)(k * n) / strip.w, 0, (float)((k + 1) * n) / strip.w, 1); };
    auto rot90 = [](const Image& s) {   // texel (x, y) -> (y, x): the edge squares store the horizontal edges transposed
        Image d;
        d.w = s.h; d.h = s.w;
        d.rgba.resize(s.rgba.size());
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x) std::memcpy(&d.rgba[((size_t)x * d.w + y) * 4], &s.rgba[((size_t)y * s.w + x) * 4], 4);
        return d;
    };
    Image left = resample(part(0), size, size), right = resample(part(1), size, size);
    Image top = resample(rot90(part(2)), size, size), bottom = resample(rot90(part(3)), size, size);
    Image tl = resample(part(4), size, size), tr = resample(part(5), size, size);
    Image bl = resample(part(6), size, size), br = resample(part(7), size, size);
    Image d;
    d.w = d.h = size * 3;
    d.rgba.assign((size_t)d.w * d.h * 4, 0);
    blit(d, tl, 0, 0); blit(d, top, size, 0); blit(d, tr, 2 * size, 0);
    blit(d, left, 0, size); blit(d, right, 2 * size, size);
    blit(d, bl, 0, 2 * size); blit(d, bottom, size, 2 * size); blit(d, br, 2 * size, 2 * size);
    return d;
}

struct Sprite { std::string name; Image img; int l = 0, t = 0, r = 0, b = 0; int page = -1, x = 0, y = 0; };
struct IconSrc { std::string path; Image img; bool tried = false, ok = false; float u0 = 0, v0 = 0, u1 = 1, v1 = 1; };

int pow2At(int v, int lo) {
    int p = lo;
    while (p < v) p *= 2;
    return p;
}

std::vector<std::string> splitWs(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string w;
    while (ss >> w) out.push_back(w);
    return out;
}

}  // namespace

bool convertUi(const UiOptions& uo, const DataDir& data, bool verbose) {
    PackWriter pack;
    std::vector<W3dUiTex> texs;
    auto addTex = [&](const Image& im, uint8_t fmt, uint8_t kind) {
        uint16_t id = (uint16_t)texs.size();
        std::vector<uint8_t> blob = encodeTextureLevel(im, fmt);
        W3dUiTex t{(uint16_t)im.w, (uint16_t)im.h, fmt, kind, 0};
        texs.push_back(t);
        pack.add(W3D_ENTRY_UI_TEX, id, std::move(blob));
        return id;
    };

    // ---------------------------------------------------------------- skin config
    std::string cfg = kDefaultSkin;
    if (!uo.skinConfig.empty()) {
        std::vector<uint8_t> b;
        if (!readFile(uo.skinConfig, b)) { std::fprintf(stderr, "ui: cannot read %s\n", uo.skinConfig.c_str()); return false; }
        cfg.assign(b.begin(), b.end());
    }
    std::vector<Sprite> sprites;
    std::map<std::string, IconSrc> iconSrc;                 // normalised path -> picture
    std::vector<std::pair<uint32_t, std::string>> iconKeys;  // key -> path
    size_t skinMissing = 0;
    {
        std::stringstream ss(cfg);
        std::string line;
        int lineNo = 0;
        while (std::getline(ss, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> w = splitWs(line);
            if (w.empty() || w[0][0] == '#') continue;
            auto num = [&](size_t i, float def) { return i < w.size() ? (float)std::atof(w[i].c_str()) : def; };
            const std::string& kind = w[0];
            if (w.size() < 3) { std::fprintf(stderr, "ui: skin line %d: too short\n", lineNo); continue; }
            const std::string& name = w[1];
            const std::string& path = w[2];
            if (kind == "icon") {
                std::stringstream ps(path);
                std::string p, found;
                while (std::getline(ps, p, '|')) {   // the first alternative that exists
                    std::vector<uint8_t> tmp;
                    if (data.read(p, tmp)) { found = p; break; }
                }
                if (found.empty()) { ++skinMissing; if (verbose) std::fprintf(stderr, "  ui: icon %s missing\n", name.c_str()); continue; }
                std::string key = DataDir::normalize(found);
                IconSrc& is = iconSrc[key];
                is.path = found;
                if (w.size() >= 7) { is.u0 = num(3, 0); is.v0 = num(4, 0); is.u1 = num(5, 1); is.v1 = num(6, 1); }
                else if (key.find("interface/icons/") != std::string::npos) { is.u0 = is.v0 = 5.0f / 64; is.u1 = is.v1 = 59.0f / 64; }
                iconKeys.push_back({W3D_UI_KEY(W3D_UIKEY_NAMED, w3dUiHash(name.c_str())), key});
                continue;
            }
            Image src;
            if (!loadBlp(data, path, src)) { ++skinMissing; if (verbose) std::fprintf(stderr, "  ui: %s missing (%s)\n", name.c_str(), path.c_str()); continue; }
            if (kind == "edge") {
                int size = std::max(2, (int)num(3, 8));
                Sprite s{name, edgeFrame(src, size)};
                s.l = s.t = s.r = s.b = size;
                sprites.push_back(std::move(s));
            } else if (kind == "nine" || kind == "sprite") {
                Image c = crop(src, num(3, 0), num(4, 0), num(5, 1), num(6, 1));
                int W = std::max(1, (int)num(7, (float)c.w)), H = std::max(1, (int)num(8, (float)c.h));
                Sprite s{name, resample(c, W, H)};
                if (kind == "nine") { s.l = (int)num(9, 0); s.t = (int)num(10, 0); s.r = (int)num(11, 0); s.b = (int)num(12, 0); }
                sprites.push_back(std::move(s));
            } else if (kind == "arrow") {
                int size = std::max(4, (int)num(3, 16));
                Image base = resample(src, size, size);
                for (int k = 0; k < 16; ++k) sprites.push_back({name + std::to_string(k), rotate(base, k * 3.14159265358979 / 8.0)});
            } else if (kind == "grid") {
                int cols = std::max(1, (int)num(3, 1)), rows = std::max(1, (int)num(4, 1));
                for (size_t i = 5; i < w.size(); ++i) {
                    int id = 0, c = 0, r = 0;
                    if (std::sscanf(w[i].c_str(), "%d:%d,%d", &id, &c, &r) != 3) continue;
                    std::string key = DataDir::normalize(path) + "#" + std::to_string(c) + "," + std::to_string(r);
                    IconSrc& is = iconSrc[key];
                    is.tried = true;
                    is.ok = true;
                    is.img = crop(src, (float)c / cols, (float)r / rows, (float)(c + 1) / cols, (float)(r + 1) / rows);
                    iconKeys.push_back({W3D_UI_KEY(W3D_UIKEY_NAMED, w3dUiHash((name + "_" + std::to_string(id)).c_str())), key});
                }
            } else {
                std::fprintf(stderr, "ui: skin line %d: unknown kind '%s'\n", lineNo, kind.c_str());
            }
        }
    }

    // ---------------------------------------------------------------- skin atlas (RGBA4, 256 wide, shelves)
    size_t skinPages = 0;
    {
        std::vector<size_t> order(sprites.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sprites[a].img.h > sprites[b].img.h; });
        const int W = 256, maxH = 512;
        size_t k = 0;
        while (k < order.size()) {
            int x = 0, y = 0, rowH = 0;
            std::vector<size_t> onPage;
            for (; k < order.size(); ++k) {
                Sprite& s = sprites[order[k]];
                if (s.img.w > W) { s.img = resample(s.img, W, std::max(1, s.img.h * W / s.img.w)); }
                if (x + s.img.w > W) { x = 0; y += rowH + 1; rowH = 0; }
                if (y + s.img.h > maxH) break;
                s.x = x;
                s.y = y;
                x += s.img.w + 1;
                rowH = std::max(rowH, s.img.h);
                onPage.push_back(order[k]);
            }
            if (onPage.empty()) break;
            Image page;
            page.w = W;
            page.h = pow2At(y + rowH, 8);
            page.rgba.assign((size_t)page.w * page.h * 4, 0);
            for (size_t i : onPage) blit(page, sprites[i].img, sprites[i].x, sprites[i].y);
            uint16_t tex = addTex(page, W3D_TEX_RGBA4, W3D_UITEX_SKIN);
            for (size_t i : onPage) sprites[i].page = tex;
            ++skinPages;
        }
    }

    // ---------------------------------------------------------------- icons of spells and items
    size_t spellIcons = 0, itemIcons = 0;
    if (uo.icons) {
        DbcTable si, spell, idi;
        std::map<uint32_t, std::string> iconPathById;
        if (si.load(data, "SpellIcon.dbc"))
            for (uint32_t r = 0; r < si.records; ++r) {
                std::string p = si.str(r, 1);
                if (!p.empty()) iconPathById[si.u32(r, 0)] = p;
            }
        auto addIcon = [&](uint32_t key, std::string path) {
            if (path.empty()) return false;
            if (lower(path).find(".blp") == std::string::npos) path += ".blp";
            std::string n = DataDir::normalize(path);
            IconSrc& is = iconSrc[n];
            if (is.path.empty()) { is.path = path; is.u0 = is.v0 = 5.0f / 64; is.u1 = is.v1 = 59.0f / 64; }   // Interface\Icons: trim the bevel
            iconKeys.push_back({key, n});
            return true;
        };
        if (spell.load(data, "Spell.dbc") && !iconPathById.empty()) {
            int nameCol = spellNameColumn(spell);
            if (nameCol < 0) nameCol = 120;
            const uint32_t iconCol = (uint32_t)nameCol - 3, effCol = (uint32_t)nameCol - 59, trigCol = (uint32_t)nameCol - 11;
            std::unordered_map<uint32_t, uint32_t> iconOf;
            for (uint32_t r = 0; r < spell.records; ++r) iconOf[spell.u32(r, 0)] = spell.u32(r, iconCol);
            for (uint32_t r = 0; r < spell.records; ++r) {
                uint32_t id = spell.u32(r, 0), icon = spell.u32(r, iconCol);
                for (uint32_t k = 0; k < 3; ++k)   // a trainer's teaching spell shows what it teaches
                    if (spell.u32(r, effCol + k) == 36) {
                        auto it = iconOf.find(spell.u32(r, trigCol + k));
                        if (it != iconOf.end() && it->second) icon = it->second;
                    }
                auto p = iconPathById.find(icon);
                if (id && p != iconPathById.end() && addIcon(W3D_UI_KEY(W3D_UIKEY_SPELL, id), p->second)) ++spellIcons;
            }
        }
        if (idi.load(data, "ItemDisplayInfo.dbc") && idi.fields > 5)
            for (uint32_t r = 0; r < idi.records; ++r) {
                std::string n = idi.str(r, 5);
                if (!n.empty() && addIcon(W3D_UI_KEY(W3D_UIKEY_ITEM, idi.u32(r, 0)), "Interface\\Icons\\" + n)) ++itemIcons;
            }
    }
    // decode, trim, resample every distinct picture once; pages of ETC1 cells
    const int iconSize = std::clamp(uo.iconSize, 16, 64), cell = iconSize + 2, perRow = 512 / cell, perPage = perRow * perRow;
    std::vector<W3dUiIcon> icons;
    size_t iconPics = 0, iconFailed = 0, iconPages = 0;
    {
        std::map<std::string, int> slotOf;   // path -> global cell index
        std::vector<const IconSrc*> cells;
        size_t done = 0;
        for (auto& [key, is] : iconSrc) {
            if (++done % 200 == 0) progress(false, "ui: icons %zu/%zu", done, iconSrc.size());
            if (!is.tried) {
                is.tried = true;
                Image src;
                if (loadBlp(data, is.path, src)) {
                    is.img = crop(src, is.u0, is.v0, is.u1, is.v1);
                    is.ok = true;
                }
            }
            if (!is.ok) { ++iconFailed; if (verbose) std::fprintf(stderr, "  ui: icon %s undecodable\n", is.path.c_str()); continue; }
            is.img = resample(is.img, iconSize, iconSize);
            slotOf[key] = (int)cells.size();
            cells.push_back(&is);
        }
        iconPics = cells.size();
        std::vector<uint16_t> pageTex;
        for (size_t p0 = 0; p0 < cells.size(); p0 += perPage) {
            size_t n = std::min<size_t>(perPage, cells.size() - p0);
            int rowsUsed = (int)((n + perRow - 1) / perRow);
            Image page;
            page.w = 512;
            page.h = pow2At(rowsUsed * cell, 8);
            page.rgba.assign((size_t)page.w * page.h * 4, 255);
            for (size_t i = 0; i < n; ++i) {
                const Image& im = cells[p0 + i]->img;
                int cx = (int)(i % perRow) * cell, cy = (int)(i / perRow) * cell;
                for (int y = -1; y <= iconSize; ++y)   // edge-replicated border: linear filtering never reads a neighbour
                    for (int x = -1; x <= iconSize; ++x) {
                        int sx = std::clamp(x, 0, iconSize - 1), sy = std::clamp(y, 0, iconSize - 1);
                        const uint8_t* s = &im.rgba[((size_t)sy * iconSize + sx) * 4];
                        uint8_t* o = &page.rgba[((size_t)(cy + 1 + y) * page.w + cx + 1 + x) * 4];
                        int a = s[3];   // opaque page: transparent corners of paper doll pictures become dark
                        o[0] = (uint8_t)(s[0] * a / 255 + 12 * (255 - a) / 255);
                        o[1] = (uint8_t)(s[1] * a / 255 + 12 * (255 - a) / 255);
                        o[2] = (uint8_t)(s[2] * a / 255 + 16 * (255 - a) / 255);
                        o[3] = 255;
                    }
            }
            pageTex.push_back(addTex(page, W3D_TEX_ETC1, W3D_UITEX_ICONS));
            ++iconPages;
            progress(false, "ui: icon page %zu", iconPages);
        }
        std::set<uint32_t> seen;
        for (auto& [key, path] : iconKeys) {
            auto it = slotOf.find(path);
            if (it == slotOf.end() || !seen.insert(key).second) continue;
            icons.push_back({key, pageTex[it->second / perPage], (uint16_t)(it->second % perPage)});
        }
        std::sort(icons.begin(), icons.end(), [](const W3dUiIcon& a, const W3dUiIcon& b) { return a.key < b.key; });
    }

    // ---------------------------------------------------------------- fonts
    std::vector<W3dUiFont> fonts(3);
    std::vector<W3dUiGlyph> glyphs;
    std::string fontNote = "built-in";
    {
        std::vector<std::vector<uint8_t>> ttfs;
        std::vector<uint8_t> main;
        std::string mainName = uo.font.empty() ? "Fonts\\FRIZQT__.TTF (client)" : uo.font;
        bool ok = uo.font.empty() ? data.read("Fonts\\FRIZQT__.TTF", main) : readFile(uo.font, main);
        if (ok) {
            ttfs.push_back(main);
            for (const std::string& f : uo.fallbackFonts) {
                std::vector<uint8_t> b;
                if (readFile(f, b)) ttfs.push_back(std::move(b));
                else std::fprintf(stderr, "ui: fallback font %s not found\n", f.c_str());
            }
            std::vector<uint8_t> b;
            if (data.read("Fonts\\ARIALN.TTF", b)) ttfs.push_back(std::move(b));
            for (const char* sys : {"C:\\Windows\\Fonts\\arialbd.ttf", "C:\\Windows\\Fonts\\arial.ttf",
                                    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/Library/Fonts/Arial.ttf"}) {
                std::vector<uint8_t> s;
                if (readFile(sys, s)) { ttfs.push_back(std::move(s)); break; }
            }
            std::vector<uint8_t> atlas;
            int ah = 0;
            std::vector<BakedFont> baked;
            size_t missing = 0;
            if (bakeFonts(ttfs, {uo.normalPx, uo.largePx}, uiCodepoints(), 512, atlas, &ah, baked, &missing)) {
                Image a;
                a.w = 512;
                a.h = ah;
                a.rgba.assign((size_t)a.w * a.h * 4, 255);
                for (size_t i = 0; i < atlas.size(); ++i) a.rgba[i * 4 + 3] = atlas[i];
                uint16_t tex = addTex(a, W3D_TEX_A8, W3D_UITEX_FONT);
                for (size_t k = 0; k < baked.size(); ++k) {
                    W3dUiFont& f = fonts[k + 1];
                    f.tex = tex;
                    f.pixelSize = (uint8_t)baked[k].pixelSize;
                    f.lineHeight = (uint8_t)baked[k].lineHeight;
                    f.ascent = (int8_t)baked[k].ascent;
                    f.firstGlyph = (uint32_t)glyphs.size();
                    f.glyphCount = (uint32_t)baked[k].glyphs.size();
                    for (const BakedGlyph& g : baked[k].glyphs)
                        glyphs.push_back({g.cp, (uint16_t)g.x, (uint16_t)g.y, (uint8_t)g.w, (uint8_t)g.h, (int8_t)g.xoff, (int8_t)g.yoff,
                                          (uint8_t)g.advance, {0, 0, 0}});
                }
                char b2[160];
                std::snprintf(b2, sizeof b2, "%s %d/%d px, %zu fallback fonts%s", mainName.c_str(), uo.normalPx, uo.largePx,
                              ttfs.size() - 1, missing ? " (some glyphs missing: the 3DS uses its own for them)" : "");
                fontNote = b2;
            }
        } else if (!uo.font.empty()) {
            std::fprintf(stderr, "ui: font %s not found\n", uo.font.c_str());
        }
        if (!uo.fontSmall.empty()) {
            std::vector<uint8_t> sm;
            std::vector<uint8_t> atlas;
            int ah = 0;
            std::vector<BakedFont> baked;
            if (readFile(uo.fontSmall, sm) && bakeFonts({sm}, {uo.smallPx}, uiCodepoints(), 256, atlas, &ah, baked, nullptr)) {
                Image a;
                a.w = 256;
                a.h = ah;
                a.rgba.assign((size_t)a.w * a.h * 4, 255);
                for (size_t i = 0; i < atlas.size(); ++i) a.rgba[i * 4 + 3] = atlas[i];
                uint16_t tex = addTex(a, W3D_TEX_A8, W3D_UITEX_FONT);
                W3dUiFont& f = fonts[0];
                f.tex = tex;
                f.pixelSize = (uint8_t)baked[0].pixelSize;
                f.lineHeight = (uint8_t)baked[0].lineHeight;
                f.ascent = (int8_t)baked[0].ascent;
                f.firstGlyph = (uint32_t)glyphs.size();
                f.glyphCount = (uint32_t)baked[0].glyphs.size();
                for (const BakedGlyph& g : baked[0].glyphs)
                    glyphs.push_back({g.cp, (uint16_t)g.x, (uint16_t)g.y, (uint8_t)g.w, (uint8_t)g.h, (int8_t)g.xoff, (int8_t)g.yoff,
                                      (uint8_t)g.advance, {0, 0, 0}});
            } else {
                std::fprintf(stderr, "ui: small font %s not usable\n", uo.fontSmall.c_str());
            }
        }
    }

    // ---------------------------------------------------------------- maps
    std::vector<W3dUiMap> maps;
    std::string strings(1, '\0');
    size_t mapsFailed = 0, overlays = 0;
    if (uo.maps) {
        DbcTable wma, wmo;
        bool haveOv = wmo.load(data, "WorldMapOverlay.dbc") && wmo.fields >= 13;
        if (wma.load(data, "WorldMapArea.dbc") && wma.fields >= 8) {
            const int mw = std::clamp(uo.mapWidth - uo.mapWidth % 8, 64, 512), mh = mw * 2 / 3;
            for (uint32_t r = 0; r < wma.records; ++r) {
                std::string name = wma.str(r, 3);
                if (name.empty()) continue;
                progress(false, "ui: map %u/%u %s", r + 1, wma.records, name.c_str());
                Image full;
                full.w = 1024;
                full.h = 768;
                full.rgba.assign((size_t)full.w * full.h * 4, 0);
                int tiles = 0;
                for (int i = 0; i < 12; ++i) {
                    Image t;
                    if (!loadBlp(data, "Interface\\WorldMap\\" + name + "\\" + name + std::to_string(i + 1) + ".blp", t)) continue;
                    if (t.w != 256 || t.h != 256) t = resample(t, 256, 256);
                    blit(full, t, (i % 4) * 256, (i / 4) * 256);
                    ++tiles;
                }
                if (tiles < 6) { ++mapsFailed; continue; }
                if (haveOv)
                    for (uint32_t o = 0; o < wmo.records; ++o) {
                        if (wmo.u32(o, 1) != wma.u32(r, 0)) continue;
                        std::string tn = wmo.str(o, 8);
                        int tw = (int)wmo.u32(o, 9), th = (int)wmo.u32(o, 10), ox = (int)wmo.u32(o, 11), oy = (int)wmo.u32(o, 12);
                        if (tn.empty() || tw <= 0 || th <= 0 || tw > 1024 || th > 768) continue;
                        const int cols = (tw + 255) / 256, rows = (th + 255) / 256;
                        for (int t = 0; t < cols * rows; ++t) {
                            Image piece;
                            if (!loadBlp(data, "Interface\\WorldMap\\" + name + "\\" + tn + std::to_string(t + 1) + ".blp", piece)) continue;
                            int c = t % cols, rr = t / cols;
                            int pw = std::min(256, tw - c * 256), ph = std::min(256, th - rr * 256);
                            blend(full, crop(piece, 0, 0, (float)pw / piece.w, (float)ph / piece.h), ox + c * 256, oy + rr * 256);
                        }
                        ++overlays;
                    }
                Image pic = resample(crop(full, 0, 0, 1002.0f / 1024, 668.0f / 768), mw, mh);
                Image tex;
                tex.w = pow2At(mw, 8);
                tex.h = pow2At(mh, 8);
                tex.rgba.assign((size_t)tex.w * tex.h * 4, 255);
                for (int y = 0; y < tex.h; ++y)   // edge-replicate the right / bottom padding
                    for (int x = 0; x < tex.w; ++x)
                        std::memcpy(&tex.rgba[((size_t)y * tex.w + x) * 4], &pic.rgba[((size_t)std::min(y, mh - 1) * mw + std::min(x, mw - 1)) * 4], 4);
                W3dUiMap m{};
                m.id = wma.u32(r, 0);
                m.mapId = wma.u32(r, 1);
                m.areaId = wma.u32(r, 2);
                m.left = wma.f32(r, 4);
                m.right = wma.f32(r, 5);
                m.top = wma.f32(r, 6);
                m.bottom = wma.f32(r, 7);
                m.tex = addTex(tex, W3D_TEX_ETC1, W3D_UITEX_MAP);
                m.w = (uint16_t)mw;
                m.h = (uint16_t)mh;
                m.nameOff = (uint32_t)strings.size();
                strings += name;
                strings += '\0';
                maps.push_back(m);
            }
        }
    }

    // ---------------------------------------------------------------- loading screens
    // Map.dbc names a LoadingScreens.dbc row per map; the column is found by looking for the one whose values are all
    // LoadingScreens ids and differ most between maps (the fixed column number differs between client builds).
    std::vector<W3dUiLoading> loading;
    size_t loadingFailed = 0;
    int loadingCol = -1;
    if (uo.maps) {
        DbcTable mp, ls;
        if (mp.load(data, "Map.dbc") && ls.load(data, "LoadingScreens.dbc") && ls.fields >= 3) {
            std::map<uint32_t, std::string> lsPath;
            for (uint32_t r = 0; r < ls.records; ++r) lsPath[ls.u32(r, 0)] = ls.str(r, 2);
            size_t bestDistinct = 0;
            for (uint32_t f = 3; f < mp.fields; ++f) {
                std::set<uint32_t> distinct;
                bool valid = true;
                for (uint32_t r = 0; r < mp.records && valid; ++r) {
                    uint32_t v = mp.u32(r, f);
                    if (!v) continue;
                    if (!lsPath.count(v)) valid = false;
                    distinct.insert(v);
                }
                if (valid && distinct.size() >= 4 && distinct.size() > bestDistinct) { bestDistinct = distinct.size(); loadingCol = (int)f; }
            }
            if (loadingCol >= 0) {
                std::map<uint32_t, int> texOf;   // LoadingScreens id -> (texture index) or -1 when it did not load
                for (uint32_t r = 0; r < mp.records; ++r) {
                    uint32_t lid = mp.u32(r, (uint32_t)loadingCol);
                    if (!lid) continue;
                    auto it = texOf.find(lid);
                    if (it == texOf.end()) {
                        int id = -1;
                        Image src;
                        progress(false, "ui: loading screen %u", lid);
                        if (loadBlp(data, lsPath[lid], src) && src.w >= 64 && src.h >= 64) {
                            // 4:3 pictures sit in the top 3/4 of a square texture
                            Image pic = src.w == src.h ? crop(src, 0, 0, 1.0f, 0.75f) : src;
                            Image tex = resample(pic, 512, 256);
                            id = addTex(tex, W3D_TEX_ETC1, W3D_UITEX_MAP);
                        } else {
                            ++loadingFailed;
                        }
                        it = texOf.emplace(lid, id).first;
                    }
                    if (it->second >= 0) loading.push_back({mp.u32(r, 0), (uint16_t)it->second, 512, 256, 0});
                }
            }
        }
    }

    // ---------------------------------------------------------------- directory
    std::vector<W3dUiSprite> spr;
    for (const Sprite& s : sprites) {
        if (s.page < 0) continue;
        spr.push_back({w3dUiHash(s.name.c_str()), (uint16_t)s.page, (uint16_t)s.x, (uint16_t)s.y, (uint16_t)s.img.w, (uint16_t)s.img.h,
                       (uint8_t)s.l, (uint8_t)s.t, (uint8_t)s.r, (uint8_t)s.b, 0});
    }
    std::sort(spr.begin(), spr.end(), [](const W3dUiSprite& a, const W3dUiSprite& b) { return a.key < b.key; });
    Blob d;
    W3dUiDir hdr{};
    d.put(hdr);
    auto table = [&](const auto& v, uint32_t& count, uint32_t& off) {
        d.align(4);
        off = (uint32_t)d.data.size();
        count = (uint32_t)v.size();
        d.putArray(v);
    };
    table(texs, hdr.texCount, hdr.offTex);
    table(fonts, hdr.fontCount, hdr.offFonts);
    table(glyphs, hdr.glyphCount, hdr.offGlyphs);
    table(spr, hdr.spriteCount, hdr.offSprites);
    table(icons, hdr.iconCount, hdr.offIcons);
    table(maps, hdr.mapCount, hdr.offMaps);
    table(loading, hdr.loadCount, hdr.offLoading);
    d.align(4);
    hdr.offStrings = (uint32_t)d.data.size();
    hdr.strSize = (uint32_t)strings.size();
    d.putBytes(strings.data(), strings.size());
    hdr.magic = W3DU_MAGIC;
    hdr.version = W3DU_VERSION;
    hdr.iconCell = (uint16_t)cell;
    hdr.iconSize = (uint16_t)iconSize;
    hdr.iconsPerRow = (uint16_t)perRow;
    hdr.iconPages = (uint16_t)iconPages;
    std::memcpy(d.data.data(), &hdr, sizeof hdr);
    pack.add(W3D_ENTRY_UI_DIR, 0, d.data);
    if (!pack.write(uo.out)) { std::fprintf(stderr, "ui: cannot write %s\n", uo.out.c_str()); return false; }
    size_t texBytes = pack.totalBytes(W3D_ENTRY_UI_TEX);
    progressDone("ui: wrote %s (%.1f MB)", uo.out.c_str(), (texBytes + d.data.size()) / 1048576.0);
    std::printf("wrote %s: %.1f MB of textures, %.0f KB of tables\n", uo.out.c_str(), texBytes / 1048576.0, d.data.size() / 1024.0);
    std::printf("  icons: %zu pictures on %zu pages (%d px), %zu spells + %zu item displays mapped, %zu undecodable\n", iconPics,
                iconPages, iconSize, spellIcons, itemIcons, iconFailed);
    std::printf("  skin: %zu sprites on %zu pages, %zu config entries missing in this client\n", spr.size(), skinPages, skinMissing);
    std::printf("  fonts: %s\n", fontNote.c_str());
    std::printf("  maps: %zu (%zu overlays), %zu without tiles\n", maps.size(), overlays, mapsFailed);
    std::printf("  loading screens: %zu maps (Map.dbc column %d), %zu pictures failed\n", loading.size(), loadingCol, loadingFailed);
    return true;
}

}  // namespace w3d
