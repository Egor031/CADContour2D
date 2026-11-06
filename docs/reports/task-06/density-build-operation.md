# 06.3B — Density Build Operation

## A. Baseline и источники

Рабочий checkout: `D:/ZherlitsynEE/CADContour2D`, branch `main`, исходный HEAD
`bc28195971ddbe43ef36a12f38b5d49453bd85d6`. Исходные status и diff чистые;
пользовательских изменений не было. Более новый код не откатывался. Родительские
`D:/AGENTS.md` и `D:/ZherlitsynEE/AGENTS.md` отсутствовали; применён корневой AGENTS.

Основание реализации: пользовательский промпт 06.3B, README, requirements §§7–8,
architecture §15 и §§24–26, algorithms §6, implementation-plan 06,
[D format](../../density-map-format.md), отчёты 06.1–06.3A, существующие controller,
runner/native completion, import adapter, project state, builder/storage и тесты.
Канонические контракты обновлены согласованно с реализацией; C1, numerical policy,
Point Cache и D wire formats, manifest v1 и сериализация cell не менялись.

Повторное независимое принятие **06.3A — ACCEPTED WITH LIMITATIONS**, исправление
**B1/P1 и B2/P2** и отсутствие новых P0–P3 переданы пользователем в исходном промпте.
Числа Debug/Release **826 PASS / 0 FAIL / 1 SKIP**, builder **128 PASS** и
integrity/resource probes **134 PASS** относятся к этому историческому evidence,
не являются новыми проверками 06.3B. История
[отчёта 06.3A](density-builder.md) и существующие raw artifacts не переписаны;
новый независимый raw report не выдумывается.

Историческое наблюдение Open lifecycle — `Closing` вместо `Ready` при недостаточном
ожидании fixture — остаётся неразрешённым. Реализация 06.3B не объявляет его
исправленным и не перерабатывает Open/Recovery. Проверки shutdown ожидают нужный
terminal state, а не только `!operation.active`.

## B. Типы и production-интеграция

`ProjectController::buildDensity(DensityBuildRequest)` использует существующий
`OperationRunner` с `OperationKind::DensityBuild`; второго worker/scheduler нет.
Без зарегистрированного `DensityPublicationReceiver` C++ admission отказывает до
старта worker. GUI Build не добавлен: обычное приложение ещё не имеет production
каталога и полного persistence workflow.

Application target связан с существующим `cadcontour_density`. Новый адаптер рядом
с import adapter вызывает production `DensityBuilder::build` ровно один раз:
реальный immutable Point Cache → C1, один `scanAll` → D. QML, Canvas, mock dataset
и verification transport не используются для D.

`DensityBuildSnapshot` удерживает постоянный ProjectId, source CacheId/count,
binary64 bounds и immutable physical binding, cell, tile/group/event profile,
явную resource policy и уникальный fully qualified output scope. Внешний
`OperationSnapshot` добавляет id/kind, runtime stamp/context epoch и captured
receiver. Массив точек и исходный ASC/XYZ не копируются; worker не обращается к
mutable ProjectState/controller/QML.

`WorkerDensityProduct` остаётся в worker: directory guard, move-only BuildOutput
с PendingDataset и retained diagnostics. `ReadyOutcome::density` содержит только
`shared_ptr<const DensityReadyProduct>`: validated Mapping, metadata/provenance,
completed path, statistics и ссылку на immutable snapshot. DatasetId не назван
VersionId; временных `currentDensity`/`lastBuild` в ProjectState нет.

Runner проверяет единственность product и соответствие operation kind, затем
descriptor относительно actual PendingDataset и snapshot: DatasetId, source
identity/count, CompletePointCache, побитовые bounds/cell, mapping dimensions и
convention, tileSide, точный путь своего scope и все переданные statistics.
Проверка totals дополняет пространственные проверки metadata, не заменяет их.
Ready формируется целиком до публикации в mailbox; fallible подготовка возвращает
failed Ready и сохраняет worker cleanup. GUI не пересоздаёт Mapping и не выполняет
readback/hash/fullValidate D.

## C. Применимость и future-cell

Density gate отдельно проверяет id/kind, active/Pending, runtime project identity,
context epoch, постоянный ProjectId, source CacheId/count/physical binding и
побитовые binary64 bounds, captured receiver, Open/shutdown/cancel/revocation.
Descriptor обязан ссылаться на тот же snapshot. Receiver replacement/detach
отзывает публикацию старой операции; новый receiver не получает старый результат.

Build с cell `0.1` принимает карту `0.1`, даже если пользователь изменил будущий
cell на `0.5`. SetCell сохраняет validation, увеличение inputRevision, dirty и
Undo/Redo. Исключение в moveHistory ограничено фактическим SetCellCommand;
произвольные будущие команды его автоматически не наследуют. Другие operation
kinds используют прежнюю applicability. Accept D не подменяет текущий сеанс
устаревшим snapshot и не откатывает новые параметры/историю.

Выбор старой карты во время Build не препятствует принятой новой карте стать
выбранной. Fixture receiver хранит несколько реальных D descriptors и rough edits;
подготовленный candidate добавляет новый descriptor и выбирает его, сохраняя
прежние данные. Production каталог, VersionId и domain Undo карт — 06.4.

Истинная invalidation, Close/Open, source replacement, shutdown и receiver
replacement необратимо отзывают authority. Cell/receiver ABA не возвращает её.
Runtime identity/context epoch исключают Close/Open ABA с теми же сохранёнными
ProjectId/CacheId. Нового generation framework не введено; существующие счётчики
и проверки exhaustion сохранены.

## D. Publication, владение и native lifecycle

Последовательность публикации:

```text
admission → immutable snapshot → existing worker → production builder → typed Ready
→ initial gate → receiver.prepare → bounded validation → final gate
→ noexcept candidate commit + Accept → worker ownership finalization/cleanup
→ native completion + Qt join → terminal
```

Receiver binding удерживается со старта операции. Prepare работает вне decision
mutex, не меняет domain state/history/selection и может вернуть null candidate,
включая отказ до неподтверждённого разрушительного Precise reset. Borrowed lease
удерживает descriptor и worker-owned D до конца preparation. Reentrant Close,
Shutdown, cancel и detach во время prepare не освобождают заимствованный product;
GUI не ждёт worker под lease/mutex.

Candidate заранее готовит storage/retention. Final applicability непосредственно
предшествует `lease.accept`: под mutex повторно проверяются Pending/cancellation,
выполняются ограниченный noexcept owner exchange и запись Accept. Commit не делает
I/O, allocations, пользовательских callbacks, throwing history push или тяжёлого
destruction. Notifications и освобождение прежних owners выполняются вне mutex.
Каждая ветвь Ready получает Accept или Discard; повторный commit исключён.

После освобождения lease worker при Accept удерживает свой каталог и вызывает
`PendingDataset::releaseToOwner()`. Между Accept и handoff немедленные Close/Shutdown
не удаляют принятый D; файл доступен обычному независимому Reader. Дополнительных
rename/copy/readback после Accept нет. Managed retention сохраняет файл, но ещё
не сохраняет версию в `.pc2dproj`, Autosave или Recovery: это граница 06.5.

При failure/cancel/Discard PendingDataset уничтожается раньше directory guard.
Удаляются только собственные файлы и пустые созданные каталоги, без recursive
удаления foreign children. Locked/orphan files дают secondary cleanup warning;
`densityCleanupWarning` удерживает структурированный BuildError, не затирая primary
ошибку. Diagnostic context имеет shared lifetime, без stack/controller pointers.
Noexcept progress bridge перехватывает исключение, сохраняет причину и кооперативно
останавливает builder; поздний cancel не превращает обнаруженный failure в Cancel.
Внутренняя invalid-product validation отличается от исключений receiver.prepare:
даже его std::invalid_argument сохраняется как CandidatePreparationFailed, а
bad_alloc — как OutOfMemory. Этот случай дополнительно закреплён regression subcase.

`WorkerStatus::Succeeded` означает построенный D, Accept — принятое владение,
terminal Succeeded — завершённую операцию после native completion и Qt join.
Builder Finish/Complete не означает применения карты. Cancel до Accept отзывает
authority, после Accept не откатывает файл. Busy сохраняется до полного cleanup/join;
нет очереди, retry, перезапуска, terminate или вложенного event loop. Existing
native-handle observation и destructor drain без event delivery сохранены.

Autosave-before-Build запрашивает отмену и возвращает Busy; Build нужно повторить
явно после освобождения worker. Autosave-during-Build и Save/Open/Import сохраняют
existing Busy guards. Wrong-thread/reentrant admission отклоняется.

## E. Проверки и evidence

Команды, XML/logs, inventories и сводка фактических запусков сохраняются в
[evidence 06.3B](../../benchmarks/density063b/README.md). Пересекающиеся subsets
не суммируются; итоговый inventory сравнивается по exact case-sensitive unique
именам, discovery Debug и Release выполняется последовательно.

Первые новые Debug tests: **33 PASS / 2 FAIL** из 35. Обнаружены несоответствия
assertions/order в integration fixture; исходный неуспешный evidence сохранён.
После исправления и расширения промежуточный набор: **46 PASS / 0 FAIL**.
Это история отдельных запусков, не итог финального diff.

После последнего изменения классификации исключений prepare повторены `all` и
`all_qmllint`, последовательный discovery Debug/Release, adapter и полный production.
Фактические команды сборки в CMD-среде `build/task02-env.cmd`:

```bat
cmake --build --preset windows-debug --target all all_qmllint --parallel 4
cmake --build --preset windows-release --target all all_qmllint --parallel 4
```

Тесты в process-local среде с Qt bin в PATH и CMake/CTest из установленного Qt Tools:

```bat
build\windows-debug\density_operation_tests.exe --gtest_output=xml:build/density063b/adapter-debug-final.xml
build\windows-release\density_operation_tests.exe --gtest_output=xml:build/density063b/adapter-release-final.xml
ctest --preset windows-debug --show-only=json-v1
ctest --preset windows-release --show-only=json-v1
ctest --preset windows-debug -L production --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063b/production-debug.xml
ctest --preset windows-release -L production --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063b/production-release.xml
```

| Финальная проверка | Debug | Release |
|---|---|---|
| Build all + all_qmllint, включая GUI targets | PASS | PASS |
| Новый adapter, отдельный executable | 47 PASS / 0 FAIL | 47 PASS / 0 FAIL |
| Полный production, 1124 cases | 1123 PASS / 0 FAIL / 1 SKIP | 1123 PASS / 0 FAIL / 1 SKIP |
| Headless subset, 876 cases, внутри production | 875 PASS / 0 FAIL / 1 SKIP | 875 PASS / 0 FAIL / 1 SKIP |
| Builder / D storage / Mapping, внутри production | 128 / 115 / 14 PASS | 128 / 115 / 14 PASS |
| Operations / Import / nonzero-Z | 53 / 25 / 30 PASS | 53 / 25 / 30 PASS |
| Save / Open / Recovery | 45 / 84 / 68 PASS | 45 / 84 / 68 PASS |
| Automated UI/view-model/QML/file/lifecycle subset | 248 PASS | 248 PASS |

Поднаборы пересекаются и не суммируются. Единственный SKIP —
`Persistence.StoragePath.FileSymlinkEscape`, Windows privilege 1314; junction cases
прошли. До финального уточнения prepare первый полный Debug также дал
1123 PASS / 0 FAIL / 1 SKIP; его XML/log/provenance сохранены с `before-taxonomy-`.
Это отдельный запуск, не замена итогового. Исторический Open Closing/Ready FAIL
в текущих прогонах не повторился, исправленным не объявляется; assertions не ослаблены.

Discovery: 1263 case-sensitive unique names в каждой конфигурации, production 1124,
headless 876. Exact headless inventories совпадают. Полные/production exact имена
различаются четырьмя существующими `AscGui.Signals/StateNotificationDeletion` labels:
GTest печатает bytes member-function pointers из `tests/gui_lifecycle_tests.cpp`.
Raw различия сохранены; имена не нормализованы. Все commands/GTest filters после
замены только directory конфигурации совпадают. Первый assert полного равенства
дал `AssertionError: all`; это зафиксированное ограничение inventory, без исключения
тестов или изменения их имён. 139 non-production discovered cases не запускались.

Реальные integration fixtures проверяют literal counts singleton/duplicates и
multi-group partial tiles через независимый D Reader, totals и один scanAll,
повторный Build с разными DatasetId, сохранение previous files/edits/selection,
future-cell в Scan/Ready/prepare и Undo/Redo, source bits/epoch/receiver/Close-Open ABA,
Autosave preemption/Busy, отмену по фазам и до старта, prepare refusal/exceptions,
Ready allocation failure и malformed products. Специальные interleavings задаются
barriers/hooks, timeout используется только как защита.

Отдельные cases покрывают borrowed lease при Close/Shutdown, немедленные
Close/Shutdown после Accept до handoff, duplicate/stale Ready, bounded progress и
owner event-loop delivery, Unicode absolute bindings при смене CWD, locked-file
cleanup и сохранность foreign children. Generic Operations regressions проверяют
wrong-kind/null/mixed Density products и сохранение secondary cleanup error;
существующий набор Operations проверяет native/TLS/observation/destructor механизмы.
B1 short transfers/in-range CRC и B2 path cases остаются тестами production builder,
который адаптер вызывает напрямую; интеграция не обходит его protections.

Small pipeline fixture записывает admission/prepare/commit/native-join timing,
число progress notifications, source/events/output bytes/owned working peak и
retained descriptors в XML properties. Это адресная synthetic проверка latency и
ownership, не общий performance норматив. Финальный synthetic source — 3 measurements,
scanAll=1, events written/read=3/3, D=478 bytes, один retained descriptor. От старта
до prepare/commit/native-join terminal: Debug 31.065/31.075/31.936 ms,
Release 19.413/19.414/20.605 ms; admission 0.173/0.064 ms, progress notifications 10/5.
Owned working peak builder — 13 357 272 / 13 357 248 bytes; это не process memory.
Точные ns/байты и промежуточные measurements — в XML/summary evidence.

Реальные representative scans, большие performance/Peak Working Set, UNC/network
faults, физический disk failure и power-loss не проверялись в рамках 06.3B.
Windows file-symlink при privilege 1314 учитывается как SKIP, не PASS.
Отсутствующий Build UI не требует ручной приёмки и не заявлен реализованным.

## F. Ресурсы и responsiveness

DensityBuildRequest выбирает явный ограниченный `initialBuildResourcePolicy()`;
unlimited ceilings по умолчанию нет. Нулевое значение отдельного ceiling не
разрешает расход этого ресурса. Начальный заменяемый инженерный профиль:

| Ресурс | Ceiling |
|---|---:|
| Owned working RAM / metadata sub-ceiling | 32 / 8 MiB |
| CacheReader reservation | 1 MiB |
| D buffer / codec / sort reservations | 8 / 1 / 1 MiB |
| Logical scratch / output / combined disk | 4 / 4 / 8 GiB |
| Одновременно существующие files, включая source / handles | 16384 / 5 |
| Work units / mapping boundary checks | 100 млн / 10 млн |
| Tile side / group side / event capacity по умолчанию | 256 / 512 / 16384 |

Это ceilings builder allocation requests/reservations и logical file lengths,
не process memory cap, обещание свободного места или утверждённые production
maxima. Компоненты получают резервы внутри общей policy, а не каждый полный лимит.
Directory metadata/path objects, bounded transport/diagnostics, candidate storage,
Qt worker и native handles находятся вне builder accounting и учитываются
отдельно. Размер будущего каталога/подготовленного candidate относится к 06.4;
fixture копирует только descriptor entries, не raster или точки.

Worker выполняет mkdir/storage/cleanup и тяжёлую работу; GUI получает immutable
metadata. Progress использует существующий latest-value mailbox с фазой,
phase-local completed и optional total. Scan/Accumulation используют source count,
Grouping/Finish сохраняют unknown total; Finish не выдаётся за 100% или terminal.
Cleanup доступен и после решения, пока операция active. Нет per-point Qt events,
больших массивов/строк или O(N) очереди сообщений. Строки structured BuildError
ограничены существующим builder контрактом.

## G. Вердикт и граница этапа

**READY FOR INDEPENDENT REVIEW.** Production application-интеграция 06.3B реализована,
финальные Debug/Release build/lint и production tests прошли с указанным symlink SKIP.
Diff проверен; новый production каталог или GUI workflow не имитируются. Ограничения
inventory и real-scan/эксплуатационных проверок явно сохранены. Самостоятельное
принятие по собственным тестам не объявляется; push не выполнялся.

06.4 подключит production version catalogue к существующему typed receiver seam,
добавит VersionId, domain Undo и workflow выбора/Precise reset. 06.5 добавит D в
persistence/Autosave/Recovery. Canvas/LOD/ROI/Rough/threshold/morphology/PNG и GUI Build
не входят в этот результат. Весь 06.3 и задача 06 не объявляются завершёнными;
принятие 06.3B требует независимого ревью.
