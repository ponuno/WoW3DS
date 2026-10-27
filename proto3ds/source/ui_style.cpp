#include "ui_style.hpp"

#include <string.h>

namespace ui {

int g_lang = 1;

uint32_t qualityColor(uint32_t q) {
    switch (q) {
        case 0: return hudRgba(157, 157, 157);
        case 1: return hudRgba(255, 255, 255);
        case 2: return hudRgba(30, 255, 0);
        case 3: return hudRgba(0, 140, 255);
        case 4: return hudRgba(190, 90, 255);
        case 5: return hudRgba(255, 128, 0);
        default: return kText;
    }
}

uint32_t classColor(uint32_t cls) {
    switch (cls) {
        case 1: return hudRgba(199, 156, 110);
        case 2: return hudRgba(245, 140, 186);
        case 3: return hudRgba(171, 212, 115);
        case 4: return hudRgba(255, 245, 105);
        case 5: return hudRgba(255, 255, 255);
        case 7: return hudRgba(0, 112, 222);
        case 8: return hudRgba(105, 204, 240);
        case 9: return hudRgba(148, 130, 201);
        case 11: return hudRgba(255, 125, 10);
        default: return hudRgba(180, 180, 180);
    }
}

uint32_t powerColor(uint8_t type) {
    switch (type) {
        case 1: return hudRgba(220, 40, 40);
        case 2: return hudRgba(220, 130, 40);
        case 3: return hudRgba(240, 225, 40);
        default: return hudRgba(50, 100, 245);
    }
}

const char* className(uint32_t cls) {
    switch (cls) {
        case 1: return tr("Warrior", "Воин");
        case 2: return tr("Paladin", "Паладин");
        case 3: return tr("Hunter", "Охотник");
        case 4: return tr("Rogue", "Разбойник");
        case 5: return tr("Priest", "Жрец");
        case 7: return tr("Shaman", "Шаман");
        case 8: return tr("Mage", "Маг");
        case 9: return tr("Warlock", "Чернокнижник");
        case 11: return tr("Druid", "Друид");
        default: return "";
    }
}

std::string trText(const std::string& en) {
    if (!g_lang) return en;
    struct Pair { const char* en; const char* ru; };
    static const Pair exact[] = {
        {"No target", "Нет цели"},
        {"Loot", "Обыскать"}, {"Talk", "Говорить"}, {"Quest", "Задание"}, {"Buy", "Купить"}, {"Train", "Учиться"},
        {"Open", "Открыть"}, {"Use", "Использовать"}, {"Mail", "Почта"}, {"Bank", "Банк"}, {"Auction", "Аукцион"},
        {"Fly", "Полёт"}, {"Herb", "Собрать"}, {"Mine", "Добыть"}, {"Pick", "Взломать"},
        {"Requires Herbalism", "Нужно травничество"}, {"Requires Mining", "Нужно горное дело"},
        {"Requires Lockpicking", "Нужно умение взлома"}, {"Requires a key", "Нужен ключ"}, {"You can't open that", "Это не открыть"},
        {"Too far away", "Слишком далеко"},
        {"You are too far away", "Вы слишком далеко"},
        {"You are too far away to loot that corpse", "Слишком далеко, чтобы обыскать"},
        {"Nothing to loot", "Нечего добыть"},
        {"It is hostile to you", "Он враждебен к вам"},
        {"Ghosts can only talk to spirit healers", "Призрак может говорить только с целителем душ"},
        {"You are dead (release your spirit first)", "Вы мертвы (сначала освободите дух)"},
        {"Your bags are full.", "Ваши сумки полны."},
        {"You don't have enough money.", "Недостаточно денег."},
        {"Trade cancelled.", "Обмен отменён."},
        {"Trade complete.", "Обмен завершён."},
        {"Mail sent.", "Письмо отправлено."},
        {"You have new mail.", "У вас новое письмо."},
        {"Auction created.", "Лот выставлен."},
        {"Auction cancelled.", "Лот снят."},
        {"Bid accepted.", "Ставка принята."},
        {"You take a flight.", "Вы отправляетесь в полёт."},
        {"New flight path discovered!", "Открыт новый маршрут полёта!"},
        {"You have no talent points.", "Нет очков талантов."},
        {"You need the talent before it first.", "Сначала изучите предыдущий талант."},
        {"You already know every rank of that talent.", "Все ранги таланта уже изучены."},
        {"You bought a bank slot.", "Ячейка банка куплена."},
        {"You are already there.", "Вы уже здесь."},
        {"You haven't learned that flight path yet.", "Этот маршрут ещё не открыт."},
        {"There is no flight path to that destination.", "Туда нет маршрута."},
        {"Your bid is too low.", "Ставка слишком мала."},
        {"You cannot bid on your own auction.", "Нельзя делать ставку на свой лот."},
        {"Unequip it first.", "Сначала снимите предмет."},
        {"You are already trading.", "Вы уже торгуете."},
        {"Whom to send it to?", "Кому отправить?"},
        {"A letter needs a subject.", "У письма должна быть тема."},
        {"You don't have enough money (postage is 30c).", "Недостаточно денег (почтовый сбор 30м)."},
        {"Choose a reward first.", "Сначала выберите награду."},
        {"Choose a reward first (tap it)", "Сначала выберите награду (нажмите на неё)"},
        {"Your target is dead", "Ваша цель мертва"},
        {"You cannot attack that target", "Эту цель нельзя атаковать"},
        {"Already casting", "Уже произносится"},
        {"You are dead.", "Вы мертвы."},
        {"You are alive again.", "Вы снова живы."},
        {"Friend added", "Друг добавлен"},
        {"Friend removed", "Друг удалён"},
        {"Player not found", "Игрок не найден"},
        {"Player ignored", "Игрок в списке игнора"},
        {"No longer ignored", "Игрок убран из игнора"},
        {"Already on your friend list", "Уже в списке друзей"},
        {"Trade declined", "Обмен отклонён"},
        {"That player is busy", "Игрок занят"},
        {"Trade target is too far away", "Игрок слишком далеко"},
        {"Your corpse could not be found.", "Ваше тело не найдено."},
        {"This vendor has nothing to sell.", "Торговцу нечего продать."},
        {"Nothing to repair.", "Нечего чинить."},
        {"Your quest log is full.", "Журнал заданий полон."},
        {"That item is sold out.", "Товар распродан."},
        {"No recent enemy", "Нет недавнего врага"},
        {"Your target has no target", "У вашей цели нет цели"},
        {"Target a player first", "Сначала выберите игрока"},
        {"Binding cancelled", "Привязка отменена"},
        {"You are dead", "Вы мертвы"},
    };
    for (const Pair& p : exact)
        if (en == p.en) return p.ru;
    static const Pair prefix[] = {
        {"You loot ", "Добыча: "},
        {"Quest accepted: ", "Задание принято: "},
        {"You have learned a new spell: ", "Изучено заклинание: "},
        {"You gain ", "Получено "},
        {"Sold ", "Продано: "},
        {"Bought ", "Куплено: "},
        {"Your corpse is ", "Ваше тело: "},
        {"You can resurrect in ", "Воскрешение через "},
        {"Nothing to do with ", "Нечего делать с "},
        {"None left: ", "Больше нет: "},
        {"Empty slot", "Пустая ячейка"},
        {"Hotbar page ", "Страница панели "},
        {"Bound to ", "Привязано: "},
        {"Cleared ", "Очищено: "},
        {"Press a button (or tap a bar cell) for ", "Нажмите кнопку (или ячейку) для: "},
        {"You need ", "Нужно "},
    };
    for (const Pair& p : prefix) {
        size_t n = strlen(p.en);
        if (en.compare(0, n, p.en) == 0) return std::string(p.ru) + en.substr(n);
    }
    return en;
}

}  // namespace ui
