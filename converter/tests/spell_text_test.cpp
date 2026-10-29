// 6.20: the variables of spell descriptions ($s1, $d, $o1, ${...}, $12345s1, $l, $?) resolved like the original's tooltips.
#include <cstdio>
#include <cstring>

#include "conv.h"

using namespace w3d;

static int fails = 0;
static void check(const char* text, const SpellTextVals& v, const char* want,
                  const std::function<const SpellTextVals*(uint32_t)>& other = nullptr) {
    const std::string got = resolveSpellText(text, v, other);
    if (got != want) { ++fails; std::printf("FAIL: \"%s\" -> \"%s\", want \"%s\"\n", text, got.c_str(), want); }
}

int main() {
    SpellTextVals fireball;   // BasePoints 19, DieSides 6: 20 to 25
    fireball.base[0] = 19; fireball.dieSides[0] = 6;
    fireball.base[1] = 7; fireball.dieSides[1] = 1; fireball.amplitudeMs[1] = 2000; fireball.durationMs = 8000;
    fireball.radiusMin[0] = 10; fireball.radiusMax[0] = 20; fireball.procChance = 25; fireball.rangeMax = 35;
    check("Hurls a fiery ball that causes $s1 Fire damage and an additional $o2 Fire damage over $d.", fireball,
          "Hurls a fiery ball that causes 20 to 25 Fire damage and an additional 32 Fire damage over 8 sec.");
    check("$m1 - $M1, $a1 to $A1 yards, $h% chance, range $r, every $t2 sec", fireball, "20 - 25, 10 to 20 yards, 25% chance, range 35, every 2 sec");
    check("${$s2*3} and ${$m1/10}.1 and ${($m1+$M1)/2}", fireball, "24 and 2.0 and 23");
    SpellTextVals aura;   // Heroic Strike-like flat value, a 2 minute duration, negative points
    aura.base[0] = -11; aura.dieSides[0] = 1; aura.durationMs = 120000; aura.stack = 5;
    check("Reduces armor by $s1 for $d. Stacks $u times.", aura, "Reduces armor by 10 for 2 min. Stacks 5 times.");
    SpellTextVals zero;   // DieSides 0: the base points are the value
    zero.base[0] = 30;
    check("Increases by $s1.", zero, "Increases by 30.");
    // another spell's numbers, singular / plural, conditions, unknown variables, tabs / line breaks
    auto other = [&](uint32_t id) -> const SpellTextVals* { return id == 1234 ? &fireball : nullptr; };
    check("As $1234s1 and $9999s1 and $1234d", zero, "As 20 to 25 and and 8 sec", other);
    check("$s1 $lsecond:seconds; and $m2 $lsecond:seconds;", [] { SpellTextVals v; v.base[0] = 0; v.dieSides[0] = 1; v.base[1] = 1; v.dieSides[1] = 1; return v; }(),
          "1 second and 2 seconds");
    check("A$?s12345[ bonus][ plain] text", zero, "A plain text");
    check("B$?s12345[ bonus] text", zero, "B text");
    check("C$?a1[x]?s2[y][ else] end", zero, "C else end");
    check("Hello $z world $gher:his; $$5\r\nnext |cffffffffgold|r", zero, "Hello world her $5 next gold");
    std::puts(fails ? "SPELL TEXT: FAIL" : "SPELL TEXT: PASS");
    return fails ? 1 : 0;
}
