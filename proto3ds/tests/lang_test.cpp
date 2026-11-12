// lang_test — 7.15: the interface language tables (English key -> translation), escapes and fallbacks.
#include <cstdio>
#include <cstring>

#include "ui_style.hpp"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    const char* path = "lang_test_tmp.txt";
    FILE* f = fopen(path, "wb");
    if (!f) return 2;
    fputs("Bags\tBolsas\nSell \tVender \nLine\\nBreak\tLinea\\nRota\nOnly here\tSolo aqui\n", f);
    fclose(f);

    ui::g_lang = ui::kLangSpanish;
    CHECK(ui::loadLanguageTable(path), "table loaded");
    CHECK(!strcmp(ui::tr("Bags", "Сумки"), "Bolsas"), "Spanish lookup");
    CHECK(!strcmp(ui::tr("Sell ", "Продать "), "Vender "), "a trailing space is kept (got '%s')", ui::tr("Sell ", ""));
    CHECK(!strcmp(ui::tr("Line\nBreak", ""), "Linea\nRota"), "escaped newline");
    CHECK(!strcmp(ui::tr("Not translated", "fallback"), "Not translated"), "missing key falls back to English");
    CHECK(ui::trText("Bags") == "Bolsas", "trText uses the table");
    ui::setLanguage(0);
    CHECK(!strcmp(ui::tr("Bags", "Сумки"), "Bags"), "English");
    remove(path);
    if (argc > 1) {
        ui::g_lang = ui::kLangSpanish;
        CHECK(ui::loadLanguageTable(argv[1]), "the bundled Spanish file loads");
        CHECK(!strcmp(ui::tr("Bags", ""), "Bolsas"), "bundled: Bags -> Bolsas");
        CHECK(!strcmp(ui::tr("Quests", ""), "Misiones"), "bundled: Quests -> Misiones");
    }
    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
