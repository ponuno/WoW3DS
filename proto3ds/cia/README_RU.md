# Сборка CIA (178 МБ на New 3DS)

`.3dsx` под Homebrew Launcher получает 124 МБ памяти, CIA с этим RSF — 178 МБ. Игра сама видит,
сколько ей дали (`source/heaps.cpp`): при 178 МБ обычная куча 48 МБ (была 24), остальное — линейная
память (~130 МБ вместо ~100).

## Один раз: инструменты

Нужны `makerom` и `bannertool` (рядом с devkitPro, например в `C:\devkitPro\tools\bin`):

- **makerom** — из релизов Project_CTR: https://github.com/3DSGuy/Project_CTR/releases
  (архив `makerom-…-win_x86_64.zip`, внутри `makerom.exe`).
- **bannertool** — Windows-сборка из релизов https://github.com/Epicpkmn11/bannertool/releases
  (или старая 1.2.0 Steveice10: в архиве папка `windows-x86_64`).

Можно не класть в PATH, а указать пути: `make cia MAKEROM=C:/tools/makerom.exe BANNERTOOL=C:/tools/bannertool.exe`.

## Сборка

```
cd proto3ds
make cia
```

Получится `w3dproto.cia` (а также, как раньше, `w3dproto.3dsx` и `w3dproto.elf`).
Установить: скопировать на SD и поставить через FBI. Иконка «w3dproto» появится на HOME-экране.
Паки и настройки остаются там же: `sdmc:/3ds/w3dproto/`.

## Проверить после установки

1. `perf_events.txt`, первая строка `memory:` — должно быть `APPLICATION region 182272 KB`,
   `heap 49152 KB`. Если там 126976 / 24576 — запущена старая .3dsx или режим не включился.
2. Чат и ввод логина (системная клавиатура) — в режиме 178 МБ системе остаётся меньше памяти,
   это проверить первым.
3. Штормград на дальности 533 — в `perf_events.txt` не должно быть `HEAP LOW` и `HEAP: … 3 x 3`.

## Что внутри

- `w3dproto.rsf`: `SystemModeExt: 178MB`, `CpuSpeed: 804MHz`, `EnableL2Cache: true`, `CanAccessCore2: true`
  (поток чтения, загрузчик интерфейса и звук работают на ядре 2), доступ к SD, сеть (`soc:U`), `ndm:u` и т. д.
  Title ID `000400000F7D3A00`.
- `icon.png` (48×48), `banner.png` (256×128), `audio.wav` (2 с) — простые, свои; можно заменить.
- На Old 3DS этот CIA получит 64 МБ — для игры мало, это сборка для New 3DS.
