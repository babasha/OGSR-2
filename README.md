<h1 align="center">OGSR 2</h1>

<p align="center">
  <b>Движок X-Ray для «S.T.A.L.K.E.R.: Тень Чернобыля» на Vulkan — ПК, Nintendo Switch, Android, Linux</b><br>
  <i>X-Ray engine for S.T.A.L.K.E.R.: Shadow of Chernobyl, rebuilt on Vulkan for PC, Nintendo Switch, Android and Linux</i>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/API-Vulkan%201.3-AC162C?style=flat-square&logo=vulkan&logoColor=white" alt="Vulkan 1.3">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Windows-x64-0078D6?style=flat-square&logo=windows&logoColor=white" alt="Windows x64">
  <img src="https://img.shields.io/badge/Switch-homebrew-E60012?style=flat-square&logo=nintendoswitch&logoColor=white" alt="Nintendo Switch homebrew">
  <img src="https://img.shields.io/badge/Android-arm64-3DDC84?style=flat-square&logo=android&logoColor=white" alt="Android arm64">
  <img src="https://img.shields.io/badge/status-в%20разработке-orange?style=flat-square" alt="status: work in progress">
</p>

<p align="center">
  <a href="https://github.com/babasha"><img src="https://img.shields.io/badge/автор-Egor%20Babushkin%20%40babasha-181717?style=flat-square&logo=github&logoColor=white" alt="Author: Egor Babushkin (@babasha)"></a>
  <a href="https://boosty.to/babaiiia"><img src="https://img.shields.io/badge/поддержать-Boosty-F15F2C?style=flat-square&logo=boosty&logoColor=white" alt="Support on Boosty"></a>
</p>

<p align="center">
  <a href="#русский"><b>Русский</b></a> · <a href="#english"><b>English</b></a>
</p>

---

## Русский

**OGSR 2** — продолжение [OGSR Engine](https://github.com/OGSR/OGSR-Engine): тот же движок «Тени Чернобыля»,
но с новым рендером на Vulkan, единым кодом для нескольких платформ и переписанными узкими местами
(ИИ, звук, интерфейс, загрузка). Цель — играть в моды на OGSR на ПК и на портативных устройствах со стабильными 60 кадрами.

> [!WARNING]
> Проект в активной разработке. Ветка `main` собирается, но отдельные системы (порты Switch и Android,
> новый боевой ИИ) ещё не доведены. Готовых сборок пока нет.

### Что нового по сравнению с OGSR

| Область | Что сделано |
|---|---|
| **Рендер** | Форвардный рендер на Vulkan вместо D3D11; bindless-материалы мира, VSM-тени, дальняя тень в фоне, LOD скинов, туман и дождь с намоканием, DLSS (Streamline/NGX) и FSR |
| **Платформы** | Один код на все платформы: Switch и Android — это пресет настроек, а не копия движка. Сборка Switch (`.nro`, devkitPro + mesa/NVK), Android (`libmain.so`, NDK + SDL2), первый Linux-бинарь |
| **Интерфейс** | Меню, инвентарь, торговля, диалоги, КПК, экран загрузки и всплывающие сообщения на [RmlUi](https://github.com/mikke89/RmlUi) (HTML/CSS), управление с геймпада, колесо оружия |
| **ИИ** | Боевой ИИ v2: запрос позиций, командир боя, «запечённые» дома из AI-сетки, карта угрозы; слух и зрение NPC с учётом того, заметили ли игрока |
| **Звук** | Глухость за стенами, HDR-микс, распространение звука по AI-сетке, автоматический реверб; на Switch — свой драйвер на DSP (audren) |
| **Производительность** | Сборщик Lua без рывков (финализаторы luabind вне атомарной фазы), быстрый спавн NPC, фоновая предзагрузка, приоритеты потоков, аим-ассист для геймпада |
| **Сеть** | Транспорт на GameNetworkingSockets (`xrOnlineNet`) — основа для совместной игры |
| **Обновления** | Встроенный апдейтер: проверяет GitHub Releases этого репозитория, качает сборку с подписанным манифестом (Ed25519) и подменяет файлы при следующем запуске — на ПК, Switch и Android |

### Платформы

| Платформа | Состояние | Как собрать |
|---|---|---|
| Windows x64 | ✅ основная | Visual Studio 2022, `Engine.sln` — см. [docs/BUILD_VULKAN.md](docs/BUILD_VULKAN.md) |
| Nintendo Switch (homebrew) | 🟡 играбельно, оптимизация | CMake + `cmake/toolchain-switch.cmake` (devkitPro) |
| Android arm64 | 🟠 запуск движка | `ogsr_engine/cmake/android/build.sh` (NDK 27, SDL2) |
| Linux | 🟠 первый бинарь | `ogsr_engine/CMakeLists.txt` + `cmake/toolchain-aarch64-linux-clang.cmake` |

### Сборка под Windows

Нужно: **Visual Studio 2022** (нагрузка «Разработка классических приложений на C++» + ATL),
**[Vulkan SDK](https://vulkan.lunarg.com) 1.3+** с заданной переменной `VULKAN_SDK`, **Git**.

```bat
git clone https://github.com/babasha/OGSR-2.git
cd OGSR-2
Update_Components.cmd
```

Затем вручную поставить зависимости, которые скрипт не качает сам (RmlUi + freetype, AMD FidelityFX SDK,
Streamline, meshoptimizer) — точные команды записаны комментариями в `Update_Components.cmd`.
После этого открыть `Engine.sln`, выбрать `Release | x64` и собрать решение.

Скомпилированные шейдеры (`*.spv`) лежат в репозитории, glslc для запуска не нужен.
Подробности, деплой в игру и частые ошибки — в [docs/BUILD_VULKAN.md](docs/BUILD_VULKAN.md).

### Структура репозитория

```
ogsr_engine/        исходники движка
  xr_3da/           ядро и главный цикл
  xrCore/ xrCDB/    базовые библиотеки, коллизии
  xrGame/           игровая логика, ИИ, интерфейс RmlUi
  Layers/xrRenderVulkan/   рендер на Vulkan
  xrSound/          звук (OpenAL, audren на Switch)
  xrOnlineNet/      сетевой транспорт
  android/ cmake/   сборка Android, Switch, Linux
3rd_party/          сторонние библиотеки (часть подтягивает Update_Components.cmd)
Game/               ресурсы движка (gamedata)
_tools/             утилиты: упаковка ресурсов для Switch, выпуск обновлений (update/), разбор профилей, шрифты UI
docs/               документация по сборке
```

### Лицензия

Код движка основан на исходниках X-Ray от GSC Game World и распространяется на условиях
[LICENSE.md](LICENSE.md): **только некоммерческое использование**, с сохранением уведомления об авторских правах.
Сторонние библиотеки в `3rd_party/` — под своими лицензиями.

Проект не связан с Nintendo, GSC Game World или AMD/NVIDIA. Для игры нужна своя легальная копия
«S.T.A.L.K.E.R.: Тень Чернобыля»; ресурсы игры в репозитории не распространяются.

### Автор

**Егор Бабушкин** — [github.com/babasha](https://github.com/babasha): рендер на Vulkan, порты на Switch, Android и Linux,
интерфейс на RmlUi, боевой ИИ, звук, апдейтер и всё остальное, что отличает OGSR 2 от OGSR.

Поддержать разработку: [boosty.to/babaiiia](https://boosty.to/babaiiia).

### Благодарности

Всем, на чьей работе стоит этот движок: команде [OGSR](https://github.com/OGSR/OGSR-Engine) и её
[участникам](https://github.com/OGSR/OGSR-Engine/graphs/contributors), а также
[Im-dex](https://github.com/Im-dex), [SkyLoader](https://github.com/SkyLoaderr), [Shoker](https://github.com/ShokerStlk),
[OpenXRay](https://github.com/OpenXRay/xray-16), [vTurbine](https://github.com/vTurbine), [solbjorn](https://github.com/solbjorn),
[abramcumner](https://github.com/abramcumner), [ForserX](https://github.com/ForserX), [Xottab-DUTY](https://github.com/Xottab-DUTY),
[morrey](https://github.com/morrey), [Anomaly](https://bitbucket.org/anomalymod/xray-monolith),
[Alpet & KD](https://github.com/joye-ramone/xray_xp_dev), [CoC-Xray](https://github.com/revolucas/CoC-Xray),
авторам [RmlUi](https://github.com/mikke89/RmlUi), [devkitPro](https://devkitpro.org) и [mesa](https://mesa3d.org).

---

## English

**OGSR 2** continues [OGSR Engine](https://github.com/OGSR/OGSR-Engine): the same Shadow of Chernobyl engine with a new
Vulkan renderer, a single codebase for several platforms, and rewritten hot spots (AI, audio, UI, loading).
The goal is to play OGSR-based mods on PC and handhelds at a stable 60 fps.

> [!WARNING]
> Work in progress. `main` builds, but the Switch and Android ports and the new combat AI are not finished. No binary releases yet.

**Highlights**

- **Vulkan forward renderer** replacing D3D11: bindless world materials, VSM shadows, async far shadow, skinned LODs, fog, rain and wetness, DLSS and FSR.
- **One codebase, many targets**: Windows x64, Nintendo Switch homebrew (`.nro`, devkitPro + mesa/NVK), Android arm64 (NDK + SDL2), Linux. Handheld targets are a settings preset, not a fork.
- **RmlUi interface**: main menu, inventory, trade, dialogs, PDA, loading screen and HUD news in HTML/CSS, full gamepad support, weapon wheel.
- **Combat AI v2**: position queries, squad commander, houses baked from the AI mesh, threat map; NPC hearing and sight depend on whether they have noticed the player.
- **Audio**: occlusion, HDR mix, sound propagation over the AI mesh, automatic reverb; a native DSP (audren) backend on Switch.
- **Performance**: stall-free Lua GC, fast NPC spawning, background preloading, thread priorities, gamepad aim assist.
- **Networking**: GameNetworkingSockets transport (`xrOnlineNet`) as the base for co-op.
- **Updates**: built-in updater that checks this repository's GitHub Releases, downloads an Ed25519-signed build and swaps it in at the next start (PC, Switch, Android).

**Building on Windows** — Visual Studio 2022 (Desktop C++ + ATL), Vulkan SDK 1.3+ with `VULKAN_SDK` set, Git:

```bat
git clone https://github.com/babasha/OGSR-2.git
cd OGSR-2
Update_Components.cmd
```

Then install the manual dependencies listed as comments in `Update_Components.cmd` (RmlUi + freetype, AMD FidelityFX SDK,
Streamline, meshoptimizer), open `Engine.sln`, pick `Release | x64` and build. Full guide (in Russian): [docs/BUILD_VULKAN.md](docs/BUILD_VULKAN.md).

**Author** — **Egor Babushkin**, [github.com/babasha](https://github.com/babasha): the Vulkan renderer, the Switch, Android
and Linux ports, the RmlUi interface, combat AI, audio, the updater and everything else that sets OGSR 2 apart from OGSR.
Support the development on [Boosty](https://boosty.to/babaiiia).

**Credits** — everyone whose work this engine stands on: the [OGSR](https://github.com/OGSR/OGSR-Engine) team and
[contributors](https://github.com/OGSR/OGSR-Engine/graphs/contributors), [OpenXRay](https://github.com/OpenXRay/xray-16),
[Anomaly](https://bitbucket.org/anomalymod/xray-monolith), [CoC-Xray](https://github.com/revolucas/CoC-Xray) and the people
listed in the Russian section, plus the authors of [RmlUi](https://github.com/mikke89/RmlUi), [devkitPro](https://devkitpro.org)
and [mesa](https://mesa3d.org).

**License** — engine code derives from GSC Game World's X-Ray source and is covered by [LICENSE.md](LICENSE.md):
**non-commercial use only**. Third-party libraries keep their own licenses. Not affiliated with Nintendo, GSC Game World, AMD or NVIDIA.
You need your own copy of S.T.A.L.K.E.R.: Shadow of Chernobyl; no game assets are distributed here.
