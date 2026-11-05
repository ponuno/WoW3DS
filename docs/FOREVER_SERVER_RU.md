# Свой сервер WoW Forever для проб (без ключа клиента)

Готовый проект: https://github.com/jibay442/forever_docker (Docker, форк TrinityCore, ветка `forever`; его README написан под сборку 70450, но ветка
уже знает и новые: 70532, 70630, 70634, 70658 — см. `sql/custom/auth/` в форке). Сервер у него
уже отключает проверку ключа сборки (`Network.SkipBuildAuthKeyCheck 1`), так что `w3dfprobe` входит без `--auth-key`.
Данные карт извлекаются из ВАШЕГО клиента Forever; в репозитории игровых файлов нет.

## Что нужно
- Linux с Docker и Docker Compose (на Windows — Docker Desktop с WSL2; сборка ~8 ГБ ОЗУ, запуск ~4 ГБ, диска ~20 ГБ).
- Установленный клиент Forever (у вас 70532; подходит, см. ниже) (папка «World of Warcraft» с `Data` и `.build.info`) — для извлечения карт.

## Запуск
```sh
git clone https://github.com/jibay442/forever_docker forever && cd forever
cp .env.example .env
```
В `.env`: три пароля (`MYSQL_ROOT_PASSWORD`, `DB_PASSWORD`, `SOAP_PASSWORD`, без кавычек), `PUBLIC_ADDRESS` (127.0.0.1 — только эта
машина; LAN-адрес ПК — чтобы доставалось с другого устройства, например 3DS), `WOW_DIR` (путь к «World of Warcraft»).
```sh
./scripts/gen-certs.sh              # ваш СА и сертификат сервера; forever_root_ca.key никому не отдавать
# сборка клиента: в .env оставьте CLIENT_BUILD пустым (сервер = последняя сборка ветки, 70658) и добавьте в overrides/bnetserver.conf:
#   Realm.AlsoAcceptBuilds = "70634,70630,70532"
# (по заметкам форка 70532..70634 — тот же вход и мировой протокол и те же данные; либо CLIENT_BUILD=70532, экспериментально)
docker compose build                # компиляция TrinityCore, долго
docker compose run --rm extractor   # карты из вашего клиента в ./data
docker compose up -d
docker compose logs -f worldserver  # первый старт: импорт базы, несколько минут, НЕ перезапускать
```
Порты (только TCP): 1119 (вход Battle.net, TLS), 8081 (REST входа), 8085 (мир). Наружу не выставлять — 3306 и SOAP.

## Аккаунт
```sh
docker compose attach worldserver     # выход: Ctrl+P, Ctrl+Q
bnetaccount create mail@example.com Пароль
```
(или сайт регистрации из проекта на 127.0.0.1:8088).

## Проверка нашей пробой
```
w3dfprobe 127.0.0.1 mail@example.com --build 70532 --ca certs/ca/forever_root_ca.crt --stay 20 --out report.txt --log packets.fwl
```
(пароль спросит консоль; сервер с другого устройства — его LAN-адрес вместо 127.0.0.1; в имени сертификата должен быть тот же адрес,
иначе используйте имя из `PUBLIC_ADDRESS`.) Ожидаем: REST → билет → Battle.net → список областей → вход в мир → список персонажей.
Пришлите `report.txt` — по нему пойдёт шаг «вход в мир с 3DS». `--build` — номер вашего клиента (70532); с `Realm.AlsoAcceptBuilds` сервер примет и его, и 70658.

## Пока не проверено мной
Этот сервер я здесь не запускал (нет Docker и клиента). Всё выше — по README проекта и коду нашей пробы; первый настоящий прогон — ваш.
