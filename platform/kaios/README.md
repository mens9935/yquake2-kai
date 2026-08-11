# Quake II for KaiOS

Unofficial port of [Yamagi Quake II](https://github.com/yquake2/yquake2) to
KaiOS (2.5.x) D-Pad feature phones, built on Emscripten/asm.js.

Two languages below: [Русский](#русский) / [English](#english).

---

## Русский

### Что это такое

Порт движка Yamagi Quake II (пересборка оригинального id Software Quake II
под современный код) на KaiOS — операционную систему кнопочных смартфонов
(Nokia 8110 4G и подобные). Игра работает полностью в браузерном движке
телефона через Emscripten (компиляция C в asm.js), без установки нативного
кода.

### Текущее состояние

Это любительский, экспериментальный порт, а не готовый релиз. Он доведён до
состояния "во что-то стабильное действительно можно играть", но:

- он тестировался только на одном конкретном устройстве и не гарантированно
  одинаково ведёт себя на всех KaiOS-телефонах;
- часть настроек (например, положение оружия в кадре, см. ниже) — это
  подобранные вручную компромиссы, а не точный расчёт, и могут не подойти
  всем моделям оружия одинаково хорошо;
- программный рендерер (Software) и GL-рендерер (GLES3) визуально и по
  поведению местами отличаются — то, что исправлено для одного, может не
  относиться к другому;
- дальнейшая поддержка и разработка не гарантируются — автор порта в любой
  момент может перестать им заниматься. Работайте с тем, что есть.

Если это не устраивает — используйте на свой страх и риск, как и любой
homebrew-проект.

### Установка

1. Установите `.zip`-сборку из этой папки как приложение (через WebIDE,
   App Manager или sideload-механизм вашего KaiOS-устройства).
2. Игре нужны оригинальные файлы данных Quake II (`baseq2/pak0.pak` и
   т.д.) — **они не включены в этот репозиторий и не поставляются с
   приложением**. Это отдельная, платная, до сих пор охраняемая авторским
   правом id Software/Bethesda игра — используйте свою легально купленную
   копию.
3. Скопируйте папку `baseq2` (из вашей копии игры) куда угодно на SD-карту
   или во внутреннюю память телефона — вложенность и точное имя диска
   значения не имеют.
4. При первом запуске (или по кнопке Play, если файл ещё не найден)
   приложение **рекурсивно** просматривает содержимое SD-карты и
   внутреннего хранилища целиком (через `DeviceStorage.enumerate()`),
   в поисках файла `pak0.pak` внутри папки `baseq2` — независимо от того,
   на каком уровне вложенности она находится, и без учёта регистра имени
   (`BaseQ2`, `BASEQ2`, `baseq2` — всё подойдёт). Если найдётся несколько
   подходящих папок, приложение предложит выбрать нужную.
5. После первого успешного запуска путь запоминается — повторного полного
   скана при обычном запуске (кнопка Play) не происходит.

### Управление

| Клавиша KaiOS | Действие |
|---|---|
| D-Pad вверх/вниз | Движение вперёд/назад |
| D-Pad влево/вправо | Движение влево/вправо (strafe) |
| OK (центральная) | Огонь (в игре) / Выбрать (в меню) |
| Левая софт-клавиша | Посмотреть вверх |
| Кнопка звонка (Call) | Посмотреть вниз |
| `#` | Прыжок |
| `*` | Присесть |
| Правая софт-клавиша | Пауза/меню (Escape) / Назад (в меню) |
| Цифры `1`–`9`, `0` | Выбор оружия (Бластер, Дробовик, Дробовик двойной, Автомат, Миниган, Гранатомёт, Ракетница, Гипербластер, Рельса, BFG10K) |

В экранах настроек лаунчера (не в самой игре) значения можно листать как
кнопкой OK, так и стрелками влево/вправо на D-Pad.

### Настройки и сохранения

- Все настройки лаунчера (разрешение, рендерер, громкость, отладочные
  параметры и т.д.) хранятся в `localStorage` браузерного движка телефона.
- Файл `config.cfg` самого движка, сохранения (`baseq2/save/...`) и
  скриншоты хранятся в постоянном хранилище IndexedDB (через IDBFS),
  смонтированном поверх домашней директории движка — это не то же самое,
  что `localStorage`, но так же переживает перезапуск и закрытие
  приложения. Удаление приложения или очистка данных сайта в браузере
  сотрёт и то, и другое.
- Первая копия `baseq2` (конфиги, сохранения) при обнаружении на SD-карте
  переносится в это постоянное хранилище один раз — дальше игра пишет
  сохранения именно туда, а не обратно на SD-карту.

### Известные проблемы / особенности

- В программном рендерере (Software) модель оружия в кадре штатно
  выглядит расположенной выше, чем должна — добавлена ручная поправка
  (настройка "Weapon height" в Video), но единого идеального значения для
  всех моделей оружия нет: слишком сильный сдвиг прячет одни модели
  (например, миниган) или ломает отображение других (рельса, автомат).
  В GLES3-рендерере эта проблема отсутствует, поэтому поправка на него не
  действует.
- Значок приложения может один раз показаться закрашенным плейсхолдером
  сразу после множественных переустановок во время тестирования — при
  обычной единичной установке этого не происходит.
- Экспериментальный переключатель "Narrow interface" (более чёткий шрифт
  ценой смещения элементов интерфейса) был убран — не работал так, как
  задумывалось.

### Лицензии

- **Код движка и игры** (`engine/`) — GNU GPL v2, унаследовано от
  оригинального открытого исходного кода id Software Quake II и проекта
  Yamagi Quake II. Полный текст: `engine/LICENSE`.
- **Игровые данные** (`baseq2/pak0.pak` и т.д.) — НЕ включены, остаются
  собственностью id Software/Bethesda Softworks и распространяются под их
  собственной коммерческой лицензией. Используйте только свою легально
  приобретённую копию.
- **Шрифт "Quake2"** (`quake2.ttf`, лого и маркер выделения в меню) —
  бесплатный шрифт с Fonts2u (freeware).
- **Шрифт Open Sans** (`open-sans.woff`, текст меню) — Apache License 2.0.
- **Значок приложения** (`icon-56.png`, `icon-112.png`) — предоставлен
  пользователем этого порта.

### Благодарности

- id Software — оригинальная игра Quake II.
- [Yamagi Quake II](https://github.com/yquake2/yquake2) — современная,
  активно поддерживаемая пересборка движка, на которой основан этот порт.
- Проект ClassiCube (KaiOS-порт) — источник вдохновения и нескольких
  технических решений для звука/загрузки на этой платформе.

---

## English

### What this is

A port of the [Yamagi Quake II](https://github.com/yquake2/yquake2) engine
(a modernized rebuild of id Software's original open-sourced Quake II) to
KaiOS (2.5.x) D-Pad feature phones, built with Emscripten/asm.js. It runs
entirely inside the phone's browser engine — no native code installation
involved.

### Current state

This is a hobbyist, experimental port, not a finished release. It's reached
"genuinely playable," but:

- it has only been tested on one specific device, and isn't guaranteed to
  behave identically on every KaiOS phone;
- some settings (the view weapon's vertical position, see below) are
  hand-picked compromises, not an exact calculation, and may not suit every
  weapon model equally well;
- the software renderer and the GLES3 renderer differ visually and
  behaviorally in places -- a fix for one doesn't necessarily apply to the
  other;
- ongoing support and development aren't guaranteed -- the author may stop
  working on this at any point. Take it as-is.

If that's not acceptable for your use case, treat this like any other
homebrew project: use at your own risk.

### Installation

1. Install the `.zip` build from this folder as an app (via WebIDE, App
   Manager, or your KaiOS device's sideloading mechanism).
2. The game needs the original Quake II data files (`baseq2/pak0.pak`
   etc.) -- **these are NOT included in this repository or the app**. Quake
   II's game data is a separate, paid, still-copyrighted product of id
   Software/Bethesda -- use your own legally purchased copy.
3. Copy the `baseq2` folder (from your own copy of the game) anywhere on
   the SD card or internal storage -- nesting depth and the exact volume
   don't matter.
4. On first launch (or when pressing Play if the data hasn't been found
   yet), the app **recursively** scans the entire contents of the SD card
   and internal storage (via `DeviceStorage.enumerate()`) for a `pak0.pak`
   file inside a `baseq2` folder -- regardless of how deeply nested it is,
   and case-insensitively (`BaseQ2`, `BASEQ2`, `baseq2` all match). If
   multiple matching folders are found, you'll be asked to pick one.
5. Once found, the path is remembered -- a normal launch (Play) does not
   re-scan from scratch.

### Controls

| KaiOS key | Action |
|---|---|
| D-Pad up/down | Move forward/back |
| D-Pad left/right | Strafe left/right |
| OK (center key) | Fire (in-game) / Select (in menus) |
| Left soft key | Look up |
| Call key | Look down |
| `#` | Jump |
| `*` | Crouch |
| Right soft key | Pause/menu (Escape) / Back (in menus) |
| Digits `1`-`9`, `0` | Weapon select (Blaster, Shotgun, Super Shotgun, Machinegun, Chaingun, Grenade Launcher, Rocket Launcher, HyperBlaster, Railgun, BFG10K) |

On the launcher's own settings screens (not in-game), values can be cycled
with either OK or the D-Pad's left/right arrows.

### Settings and save data

- All launcher settings (resolution, renderer, volume, debug tuning, etc.)
  are stored in the phone's browser engine's `localStorage`.
- The engine's own `config.cfg`, save games (`baseq2/save/...`), and
  screenshots live in persistent IndexedDB storage (via IDBFS), mounted
  over the engine's home directory -- a different mechanism from
  `localStorage`, but equally durable across app restarts and closures.
  Uninstalling the app or clearing the browser's site data wipes both.
- The first `baseq2` copy found on external storage is copied into this
  persistent storage once -- from then on, saves are written there, not
  back to the SD card.

### Known issues / quirks

- On the software renderer, the view weapon model normally sits higher on
  screen than it should -- a manual correction ("Weapon height" under
  Video) is applied, but there's no single value that's ideal for every
  weapon model: too aggressive a shift hides some (e.g. the chaingun) or
  breaks the rendering of others (railgun, machinegun). The GLES3 renderer
  doesn't have this problem, so the correction doesn't apply there.
- The app icon may briefly show as a plain placeholder right after
  repeated reinstalls during testing -- this doesn't happen on a normal,
  one-time install.
- The experimental "Narrow interface" toggle (crisper text at the cost of
  UI elements shifting) has been removed -- it didn't work as intended.

### Licenses

- **Engine and game code** (`engine/`) -- GNU GPL v2, inherited from id
  Software's original open-sourced Quake II code and the Yamagi Quake II
  project. Full text: `engine/LICENSE`.
- **Game data** (`baseq2/pak0.pak` etc.) -- NOT included, remains the
  property of id Software/Bethesda Softworks under their own commercial
  license. Use only your own legally purchased copy.
- **"Quake2" font** (`quake2.ttf`, the logo and menu selection marker) --
  a free font from Fonts2u (freeware).
- **Open Sans font** (`open-sans.woff`, menu text) -- Apache License 2.0.
- **App icon** (`icon-56.png`, `icon-112.png`) -- provided by this port's
  user.

### Credits

- id Software -- the original Quake II.
- [Yamagi Quake II](https://github.com/yquake2/yquake2) -- the modern,
  actively maintained engine rebuild this port is based on.
- The ClassiCube project's KaiOS port -- inspiration and a few technical
  approaches for audio/loading on this platform.
