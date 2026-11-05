# WoW Forever на 3DS — план и шаг 1 (пробы)

WoW: Forever (Classic 1.60.1, выход 4.11.2026) — современный клиент Classic. Частные серверы — форк TrinityCore
(`advocaite/TrinityCore`, ветка `forever`; пример — `forever_docker`). С клиентом 1.12 они не совместимы: другой вход,
другой мировой протокол и другие файлы данных. Наш клиент от WoWee не зависит; справочник — исходники форка.

## Что уже сделано (шаг 1, `net/forever/`)
- `w3dfprobe` — вход как клиент 1.60.1:
  - REST `POST /bnetserver/login/` (JSON-форма с e-mail и паролем) → билет `TC-…`; при неудаче HTTPS пробует HTTP;
  - Battle.net RPC по TLS (mbedTLS 2.28): кадр = u16 BE длина заголовка + `bgs.protocol.Header` + сообщение;
    `ConnectionService.Connect` (0x65446991/1), `AuthenticationService.Logon` v1 (0x0DECFC01/1) с билетом →
    `AuthenticationListener.OnLogonComplete` (0x71240E35/5); `GameUtilities` (0x3FC1274D): `ClientRequest` (1) с
    `Command_RealmListTicketRequest_v1_b9` (32-байтовый секрет клиента), `GetAllValuesForAttribute` (10) — подрегионы,
    `Command_RealmListRequest_v1_b9` (zlib-JSON), `Command_RealmJoinRequest_v1_b9` → билет, адрес мира, секрет сервера;
  - мир: строки-приветствия V2, пакет = u32 размер + 12 байт тега AES-GCM + (u32 опкод + данные);
    `SMSG_AUTH_CHALLENGE` → `CMSG_AUTH_SESSION`; ключ сессии 40 байт (HMAC-SHA512 + генератор SHA512), ключ шифрования
    32 байта; `SMSG_ENTER_ENCRYPTED_MODE` (подпись Ed25519 не проверяем) → ACK, AES-256-GCM с IV = счётчик пакетов +
    "SRVR"/"CLNT"; `SMSG_AUTH_RESPONSE` → `CMSG_ENUM_CHARACTERS`; сжатые пакеты — один raw-deflate поток;
  - опкоды Classic: `tools/forever/gen_opcodes.py` переводит таблицы форка (сдвиг групп для сборок 70291+) в
    `net/forever/fopcodes.inc`; ключ сборки для дайджеста неизвестен — серверы его не проверяют (`SkipBuildAuthKeyCheck`).
- `w3dfdata` — данные клиента: CASC (CascLib, MIT), DB2 WDC3/4/5 (inline / bit-packed / common / pallet, строки,
  список ID), WDT с MAID (FileDataID тайлов), ADT root/_obj0/_tex0, M2 с MD21/SFID/TXID/AFID, корень WMO.
- Тесты: `net/tests/run_forever.sh` — проба против `forever_fake.py` (REST + RPC с настоящими protobuf-описаниями форка
  `data/forever_bnet.desc` + мир с независимо написанным выводом ключей и AES-GCM) и `forever_data_test` (синтетические
  WDC5/WDT/ADT/M2). На настоящем сервере и настоящих данных ещё НЕ проверено.

## Дальше (после отчётов проб)
1. Сеть: перенести вход в `net/` клиента 3DS (mbedTLS собирается под ARM), разбор `SMSG_ENUM_CHARACTERS_RESULT` в
   раскладке Classic, вход в мир (`CMSG_PLAYER_LOGIN`, второе соединение `SMSG_CONNECT_TO`), объекты (`UPDATE_OBJECT`
   с полями по маскам изменений — своя раскладка у каждой сборки), движение, чат.
2. Конвертер: вход CASC вместо MPQ, DB2 вместо DBC (раскладки — WoWDBDefs), ADT из трёх файлов, M2 с внешними
   .skin/.anim, WMO по FileDataID, листфайл для имён. Формат паков 3DS не меняется.
3. Новое содержимое (Скайборны, новые зоны, коллекции, достижения).
