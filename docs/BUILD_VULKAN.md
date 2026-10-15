# Сборка Vulkan-рендера (xrEngine_VK.exe)

Инструкция, как собрать ветку `vulkan-renderer` и запустить её на своей игре.
Рендер **форвардный** (свет/тени/HDR/SSAO/погода считаются прямо в forward-шейдерах),
deferred-путь из R4 (`uber_deffer`) сюда НЕ портировался и не нужен.

Собирается под **Windows x64**, Visual Studio 2022, MSVC.

---

## 0. Что нужно поставить (one-time)

| Что | Зачем | Примечание |
|-----|-------|------------|
| **Visual Studio 2022** (Community или Build Tools) | компилятор MSVC | нужна нагрузка **«Разработка классических приложений на C++»** |
| Компонент **C++ ATL** (`Microsoft.VisualStudio.Component.VC.ATL`, +ATLMFC) | `splash.cpp` тянет `atlimage.h`/`atlstr.h` | без него линк падает на ATL |
| **Vulkan SDK** (1.3+; норм и 1.4.x) — https://vulkan.lunarg.com | заголовки/библиотеки Vulkan; props фейлятся, если не задан | задать переменную `VULKAN_SDK` (см. ниже) |
| **Git** | склонировать репо + подтянуть зависимости | — |
| GPU с поддержкой **Vulkan 1.x** | запуск | тестировалось на RTX 5070 Laptop |

> Шейдеры **уже скомпилированы и лежат в репо** (`*.spv` рядом с `*.glsl`).
> glslc / пересборка шейдеров для запуска НЕ требуется.

---

## 1. Склонировать и подтянуть зависимости

```bat
git clone <repo-url> OGSR-Engine
cd OGSR-Engine
git checkout vulkan-renderer

REM подтянуть исходники сторонних либ (DirectXTex/Math, VMA, lz4, zstd, mimalloc, ...)
Update_Components.cmd
```

`Update_Components.cmd` клонирует все 3rd-party репозитории в `3rd_party\Src\...`.
**Без него сборка не пройдёт** — submodule-исходников в репо нет.
Для Vulkan среди прочего тянется **VulkanMemoryAllocator (VMA)**.

---

## 2. Задать VULKAN_SDK

В том же терминале, где будешь собирать (пример — поправь версию под свою):

```bat
set VULKAN_SDK=C:\VulkanSDK\1.4.350.0
```

(Или один раз через «Изменение переменных среды» в Windows.)
Props рендера сразу фейлятся с понятной ошибкой, если переменная не выставлена.

---

## 3. (Только для Release) пересобрать LuaJIT под нужный CRT

> Если собираешь **Debug** — пропусти этот шаг.

Подводный камень: проект LuaJIT пересобирает либу, только если её **нет**; иначе просто
копирует уже лежащую. В репо/после Debug-сборки лежит **Debug-вариант (`/MTd`)**, а
Release-движок линкуется с `/MD` → конфликт CRT → битый линк/краш.

Лечится так — собрать LuaJIT Release **руками один раз**. Сохрани это как
`build_luajit_rel.bat` в корне репо (4 команды должны идти **отдельными строками**,
не через `&&` — иначе cmd подставит `%CD%`/`%PATH%` до `cd`/vcvars):

```bat
@echo off
del /q ogsr_engine\LuaJIT\bin\x64\LuaJIT.lib
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd ogsr_engine\LuaJIT\src
set PATH=%CD%;%PATH%
msvcbuild.bat x64 static Release
```

(Путь к `vcvars64.bat` поправь под свою установку VS — у Community он в
`...\2022\Community\VC\Auxiliary\Build\`.) `set PATH=%CD%;...` — фикс, чтобы
`minilua`/`buildvm` нашлись. Должна появиться `ogsr_engine\LuaJIT\bin\x64\LuaJIT.lib`.
Дальше при сборке solution проект LuaJIT просто скопирует её в Release-папку.

---

## 4. Собрать solution

Из **x64 Native Tools Command Prompt for VS 2022** (или после `vcvars64.bat`),
в корне репо:

```bat
REM Release (для бега/демок/перфа — рекомендуется)
msbuild Engine.sln /p:Configuration=Release /p:Platform=x64 /m

REM Debug (если нужно отлаживать)
msbuild Engine.sln /p:Configuration=Debug /p:Platform=x64 /m
```

`/m` — параллельная сборка. Соберутся все 3rd-party и движковые либы, затем сам движок.

### Проверка результата

В солюшне больше нет DX-проектов (`xrRender_R4`, `XR_3DA` и `DirectXTex` удалены вместе
с D3D11-рендером), поэтому **msbuild должен вернуть 0**. Если вернул не 0 — это
настоящая ошибка, а не былая «безвредная» ругань DirectXTex/fxc (exit 9009).

Плюс проверка по артефакту:

- в логе есть строка `XR_3DA_Vulkan.vcxproj -> ...\bin_x64\xrEngine_VK.exe`;
- файл `bin_x64\xrEngine_VK.exe` свежий;
- **Release ≈ 18 МБ**, Debug ≈ 60 МБ (быстрый способ убедиться, что собрался именно Release).

**Результат:** `bin_x64\xrEngine_VK.exe`.

---

## 5. Деплой в игру и запуск

Нужна установленная игра на движке OGSR (например сборка с Gunslinger).
Движок-`exe` кладётся рядом со штатным `xrEngine.exe`, шейдеры — в `gamedata`.

1. **Движок:** скопировать в `<game>\bin_x64\`:
   - `bin_x64\xrEngine_VK.exe`
   - `bin_x64\xrEngine_VK.pdb` (по желанию, для крэш-логов)

2. **Шейдеры:** скопировать **все** `*.spv` из
   `ogsr_engine\Layers\xrRenderVulkan\shaders\`
   в `<game>\gamedata\shaders\` (папку создать, если её нет).
   > Загрузчик ищет `.spv` в `<папка с exe>\..\gamedata\shaders\`.

3. **Запуск:** запустить `<game>\bin_x64\xrEngine_VK.exe`
   (рабочая папка — `bin_x64`).
   Рантайм-DLL (OpenAL32/dbghelp/discord-rpc) обычно уже лежат в игре.

4. **Логи:** `<game>\_appdata_\logs\xray_<user>_*.log` — туда писать, если что не так.

> **Release-движок собран с `/MD`** → на машине должен стоять
> **Microsoft Visual C++ 2015–2022 Redistributable (x64)**, иначе exe не стартует
> с ошибкой про отсутствующие `*.dll` CRT.

Готовый набор для друга = `xrEngine_VK.exe` + папка `*.spv` + (если нет) VC++ Redist.
Свою игру/геймдату он подставляет сам.

---

## Быстрый чеклист

```
[ ] VS 2022 + C++ desktop workload + ATL
[ ] Vulkan SDK поставлен, VULKAN_SDK выставлен
[ ] git checkout vulkan-renderer
[ ] Update_Components.cmd отработал
[ ] (Release) LuaJIT пересобран в Release
[ ] msbuild Engine.sln /p:Configuration=Release /p:Platform=x64 /m
[ ] bin_x64\xrEngine_VK.exe ≈18 МБ существует (DirectXTex-ошибку игнорим)
[ ] exe+pdb -> <game>\bin_x64\
[ ] shaders\*.spv -> <game>\gamedata\shaders\
[ ] стоит VC++ Redist (x64)
[ ] запустить xrEngine_VK.exe
```

---

## Если не собирается / не запускается

| Симптом | Причина / лечение |
|---------|-------------------|
| `atlimage.h`/`atlstr.h` not found | не стоит компонент **C++ ATL** в VS Installer |
| ошибка про `VULKAN_SDK` в props | не выставлена переменная `VULKAN_SDK` (шаг 2) |
| нет исходников 3rd-party / include `3rd_party\...` not found | не запускался `Update_Components.cmd` (шаг 1) |
| линк-ошибки/краш на LuaJIT в Release | не пересобрал LuaJIT Release (шаг 3) — CRT-конфликт `/MTd` vs `/MD` |
| msbuild вернул 1, но `xrEngine_VK.exe` собрался | это та самая DirectXTex/fxc ошибка — игнорируй, см. шаг 4 |
| exe не стартует, просит CRT `*.dll` | поставить **VC++ Redistributable x64** |
| чёрный экран / нет картинки | не скопированы `*.spv` в `<game>\gamedata\shaders\`; смотри лог в `_appdata_\logs\` |
| вылет на старте | проверь, что GPU/драйвер поддерживает Vulkan (свежий драйвер NVIDIA/AMD) |
