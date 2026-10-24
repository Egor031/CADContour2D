# Локальные vcpkg ports

`vcpkg-configuration.json` подключает `vcpkg/ports` относительно корня репозитория.
Overlay применяется автоматически при manifest install, в том числе через оба
CMake configure presets. Дополнительные переменные окружения или абсолютные пути
к overlay не нужны. `builtin-baseline` в корневом `vcpkg.json` не изменён.

## fmt 12.2.0#1 / MSVC C4702

Port скопирован из зафиксированного vcpkg baseline
`1460b31b08c42cc2e9ac2c79f45ec8707e2675e2`, tree fmt
`7ca0b8c0026883daf28a0db75f6b4964bae2979a`. Версия, `port-version`, зависимости,
source SHA-512, build options, `usage` и существующий backport #4813 сохранены.
Единственное дополнение к recipe — `code-point-length-c4702.patch`.
Исходный код fmt в репозитории не хранится.

Патч переносит только изменение `code_point_length()` из upstream
[b54093c658ae5656799756ec5d4467f49064929f](https://github.com/fmtlib/fmt/commit/b54093c658ae5656799756ec5d4467f49064929f).
При инстанцировании `wchar_t` из header-only spdlog с `SPDLOG_WCHAR_FILENAMES`
безусловный `if constexpr` return оставлял generic UTF-8 код недостижимым.
Явный `else` отделяет его от wide-ветки, сохраняя результаты и constexpr.
Остальные изменения upstream commit не нужны для воспроизведённого случая.

Последующий upstream
[cd67635a19d027d2eae1cd6f0f0fa8f057492907](https://github.com/fmtlib/fmt/commit/cd67635a19d027d2eae1cd6f0f0fa8f057492907)
вернул прежний control flow и подавил C4702 во всём `base.h`. Здесь выбран
проверенный локальный `else`, чтобы не скрывать диагностику других функций.
Уровни предупреждений, C++20, ABI и `SPDLOG_WCHAR_FILENAMES` не меняются;
production spdlog остаётся header-only, fmt — внешней shared библиотекой.

Изменение recipe/patch меняет vcpkg package ABI hash даже при прежней версии
`12.2.0#1`. Повторный configure автоматически переустанавливает fmt и пересобирает
зависимый spdlog либо восстанавливает уже исправленные пакеты из binary cache.
Это ожидаемая инвалидация cache, а не изменение публичного C++ ABI или версии
spdlog. Редактировать установленные заголовки и вручную очищать старые пакеты
не требуется.

## Другой компьютер

После получения изменений открыть x64 Developer Command Prompt, задать прежние
`VCPKG_ROOT` и `QT_ROOT` для локальных установок и выполнить из корня репозитория:

```bat
chcp 65001 >nul
set "PATH=%QT_ROOT%\bin;%PATH%"
cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-debug --target all all_qmllint --parallel 4
ctest --preset windows-debug -L production --output-on-failure
cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-release --target all all_qmllint --parallel 4
ctest --preset windows-release -L production --output-on-failure
```

Сначала нужен именно configure с включённым manifest install; оставленный от
диагностики `VCPKG_MANIFEST_INSTALL=OFF` следует переопределить значением `ON`.
Пользовательский command-line overlay с fmt имеет приоритет перед конфигурацией
репозитория — его также нужно учитывать. В выводе vcpkg источник fmt должен быть
`vcpkg/ports/fmt`; при сборке из исходников видна строка
`Applying patch code-point-length-c4702.patch`. При binary-cache restore проверить
установленный `share/fmt/vcpkg_abi_info.txt`: он содержит hash этого patch-файла.
Фактический include path проверяется в verbose compiler command или `/showIncludes`.

При пустом download/binary cache потребуется обычная загрузка исходников и
инструментов vcpkg. Локальный C4702 patch поступает из Git. Рецепт не зависит
от букв дисков, SSD/HDD и расположения checkout; Qt/MSVC и остальные зависимости
не обновляются. Сборка на втором компьютере отдельно не подтверждена.

При будущем изменении версии fmt этот overlay нужно пересмотреть явно: он
сознательно сохраняет 12.2.0#1. Удалять его можно после проверки upstream-версии
на том же wide reproducer и чистой сборке проекта в Debug/Release.

## Проверка 2026-10-24

На основном компьютере: Windows 11 x64, MSVC 19.51.36260/v145, Qt 6.12.0,
CMake 3.30.5, Ninja 1.12.1. Проверены presets `windows-debug`/`windows-release`
с `-B` в отдельные пустые каталоги и включёнными tools. Configure и build
`all all_qmllint` завершились с кодом 0: по 110 C++-компиляций и два lint-задания,
без compiler/linker/QML warnings. Optional Qt TaskTree/Vulkan diagnostics остаются.

Исходный reproducer выдаёт C4702 при `/Od` и `/O2`; исправленный и установленный
через overlay заголовок проходит оба режима с `/W4 /WX`, runtime и constexpr
проверками wide/narrow результатов. `/showIncludes` реального проблемного TU
подтверждает использование заголовка из нового installed tree. Имена и ordinals
65 exports fmt и 392 exports spdlog совпадают с исходными DLL в Debug и Release.
У остальных пакетов ABI metadata не изменились. Проверена также автоматическая
переустановка fmt/spdlog с прежними версиями при изменении recipe/patch hash.

В каждой конфигурации адресные logging/application lifecycle CTest — 60/60;
production CTest — 803 passed, 0 failed, 3 skipped из 806. Два cross-volume SKIP
связаны с TEMP/build на одном томе, третий — с symlink privilege 1314; тесты и
их SKIP-логика не менялись. Изолированные процессы проверили реальный
`operationDiagnostic`: создание/запись `operations.log`, обе ротации в Unicode
каталоге и безопасный отказ при невозможности создать каталог логов. Отдельно
проверен wide rotating sink с Unicode-путём. Application/persistence/GUI-код,
форматы и статусы принятия этапов не изменены.
