# Density Map versions / Project State — 06.4

## Baseline и границы

Работа начата на чистом `main`, HEAD
`03541ca8f9ce9aa39b54ba7b9bf81940ff3ed531`, origin
`https://github.com/Egor031/CADContour2D.git`. Это более новый checkout, чем
указанный в задании `42147bb3c10df8008e1f8485255d8370bd02a7bf`; отката не было.
Изучены README, AGENTS, относящиеся разделы requirements/architecture/algorithms,
implementation-plan, ui-design и отчёт Density Build Operation. Принятие 06.3A/B
с ограничениями уже записано в baseline; исторические evidence не переписаны.
Широкий повторный аудит D/C1/mapping не выполнялся.

06.4 добавляет production catalogue/selection/history и временную защиту v1.
Manifest v2, round-trip D, GC, GUI Build/Canvas/LOD, ROI, Rough detection/editing,
Precise pipeline, PNG/TIFF, C2 и codecs не реализованы. 06.5–06.8 не закрыты.

## Identity, данные и владение

Единственная истина каталога — Qt-free `ProjectState`. Его ordered vector содержит
shared references на постоянные `DensityVersion`; active — optional `VersionId`.
Новый ненулевой UUID генерируется на application boundary; test factory позволяет
вернуть null/коллизию или отказать. VersionId, ProjectId, CacheId и DatasetId —
разные типы. Duplicate VersionId/DatasetId отвергаются до публикации.

Descriptor содержит собственные DatasetId/binding, source CacheId/count/bounds,
binary64 Mapping из Ready, C1/CompletePointCache/D v1 provenance, tile/group/event
parameters, resource policy и все scalar Build/source-reader/D statistics.
Mapping не создаётся повторно; D/cache/source не читаются при выборе/prepare/commit.
Отдельный Rough owner каждой версии пока пуст; фиктивная геометрия не добавлена.
Будущие команды могут ссылаться на VersionId и устойчивый owner, без индексов каталога.
Future cell не меняет уже построенные карты.

Каждый принятый Build добавляет новую версию, даже с теми же параметрами/counts,
и выбирает её. Старые версии не заменяются, не дедуплицируются и не удаляются.
Accepted bindings не владеют открытыми handles и ничего не удаляют при уничтожении.
Смена выбора, очистка redo/history, Close, Import replacement и destructor не удаляют D.
Непринятый PendingDataset очищается прежним worker protocol до directory guard.

Каталог требует O(V) shared references плюс descriptors/Rough; история — O(H)
малых records с cell/VersionId. Один prepare временно копирует O(V + H) references/
records. Нет raster/point copies или per-version handles. Эти расходы находятся
вне builder RAM accounting; произвольный лимит количества версий не вводился.
Непредставимые размеры, revision/history/identity exhaustion и allocation failures
проверяются до публикации.

## Конкретная атомарность и история

Прежний QUndoStack заменён локальной типизированной `ProjectHistory`; внешние
SetCell/canUndo/canRedo/undo/redo contracts сохранены. Records различают SetCell и
DensitySelection, содержат before/after values или optional stable IDs. Новое
действие после Undo подготавливает только текущий prefix плюс одну запись.

Production VersionReceiver принадлежит ProjectSession; candidate удерживает strong
session lease, receiver — weak back-reference. Reentrant Close не оставляет dangling
state/history pointers и не создаёт цикл владельцев. Receiver для нового сеанса
создаётся лениво при первом public Build; explicit registration/detach seam сохранён.

Prepare вне command/decision gate проверяет source/session и Ready descriptors,
готовит version/Rough/binding, новый catalogue vector, новую history branch и candidate.
Никакого D readback/hash/copy. Подтверждение reset ничего не уничтожает. После
callbacks снова проверяются session/source/receiver, active choice generation,
Rough revision и Precise context. Поздние future-cell edits внутри Build confirmation
не отзывают Build: catalogue/history переподготавливаются на актуальном состоянии
до commit. Отказ этой повторной подготовки также сохраняет reset/state/history.

Final applicability предшествует существующему `ReadyProductLease::accept`. Под
decision mutex выполняются только O(1) noexcept операции: vector swaps, active ID,
history index/generation, domain/choice revision, consumption reset authority и Accept.
Ни allocations/I/O/callbacks/signals, ни удаления старых vector owners под mutex нет.
Они остаются candidate и освобождаются после gate. Notifications наблюдают уже
согласованные catalogue/active/dirty/history. Throwing QUndoStack::push после Accept
не используется. После Accept нет обязательного fallible шага применения state.

Build создаёт одну запись смены выбора относительно active непосредственно перед
Accept. Undo возвращает прежний ID, для первого Build — null, сохраняя новый каталог,
D и Rough. Redo выбирает тот же ID без worker. SetCell/selection/Build хронологичны;
rejected и no-op не меняют history/redo/dirty/reset. Selection и её Undo/Redo, как и
future cell, не отзывают DensityBuild; исключение ограничено этими typed commands.
Relevant invalidation остаётся необратимой, Undo ABA не восстанавливает authority.

Сохранена цепочка snapshot → C1/D → typed Ready → applicability → prepare → final
applicability → noexcept commit/Accept → worker retention/handoff → native completion/
Qt join → terminal. Busy не снимается по Builder Finish или только по Accept.

## Precise guard и временный отказ persistence

Select/Undo/Redo/Build используют один C++ policy boundary с
NeedsConfirmation/Cancel/Confirm/Stale. Request привязан к действию, project/runtime/
session epoch, before/target IDs, choice generation, Rough revision, Precise generation
и history context. Direct/history consent требует неизменного history context;
Build допускает только описанный rebase future-cell history. Undo проверяет guard
до сдвига cursor. Повторный active choice и future cell reset не требуют.

В 06.4 имеются только authority/policy seam и fault-test context; настоящих Precise
данных, reduced cloud, предупреждений о несуществующем pipeline и GUI reset нет.
Подтверждённый переход потребляет reset authority, history содержит только IDs,
поэтому последующий Undo не восстанавливает Precise. Подключение реальных Precise
owners потребует подготовки их retirement до gate, без fallible destruction под ним.

`ProjectState::hasUnpersistableV1State`, `persistence::isV1Representable` и structured
`UnrepresentableState` защищают прямой captureSnapshot до v1 projection. Controller
Save/Save As/Autosave возвращают `PersistenceVersionsUnsupported` до worker и до
публикации. При каталоге matchesSavedContent=false, включая active=null после Undo.
Manifest/recovery bytes, baseline, dirty и D references сохраняются. Autosave timer
получает тихий отказ, без worker/recovery writes или циклических dialogs.

UI показывает отдельную причину недоступности Save и достоверный статус каталога/
active cell. Отказ Save в Close/Exit/Open/Import завершает workflow, сохраняя проект;
Cancel и явный Discard доступны. Проекты без карт используют прежний v1 workflow.
Sidecar или частичное сохранение не добавлены; сохранение версий — 06.5.

## Проверки и evidence

Проверенное окружение — MSVC 19.51.36260 / Windows x64, Qt 6.12.0 msvc2022_64,
CMake 3.30.5, Ninja 1.12.1; тот же `build/task02-env.cmd`, что в предыдущих этапах.
Он вызывает штатный vcvars64 и задаёт process-local QT_ROOT/VCPKG_ROOT/PATH.
Глобальное окружение и зависимости не менялись. Qt configure сохранил прежние
нефатальные optional TaskTree/Vulkan warnings.

Фактически выполнены в этой среде:

```bat
cmake --build --preset windows-debug --target all all_qmllint --parallel 4
cmake --build --preset windows-release --target all all_qmllint --parallel 4
build\windows-debug\density_version_tests.exe --gtest_output=xml:build/density064/versions-debug-final.xml
build\windows-release\density_version_tests.exe --gtest_output=xml:build/density064/versions-release-final.xml
ctest --preset windows-debug -R UnavailableCacheRestoresMetadataAndOnlyExplicitRecheckEnablesCommands --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density064/recovery-regression-debug.xml
ctest --preset windows-release -R UnavailableCacheRestoresMetadataAndOnlyExplicitRecheckEnablesCommands --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density064/recovery-regression-release.xml
ctest --preset windows-debug --show-only=json-v1
ctest --preset windows-release --show-only=json-v1
ctest --preset windows-debug -L production --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density064/production-debug.xml
ctest --preset windows-release -L production --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density064/production-release.xml
```

Финальный код проверен после исправления v1 Undo regression:

| Проверка | Debug | Release |
|---|---|---|
| Build all + all_qmllint | PASS | PASS |
| Новый executable, 47 cases | 47 PASS / 0 FAIL | 47 PASS / 0 FAIL |
| PartialRecovery regression subset | 3 PASS / 0 FAIL | 3 PASS / 0 FAIL |
| Production, 1171 cases | 1170 PASS / 0 FAIL / 1 SKIP | 1170 PASS / 0 FAIL / 1 SKIP |

Поднаборы входят в полный production и не суммируются с ним. Новые cases:
11 domain, 28 production pipeline/history/lifecycle/reset и 8 persistence/UI guards.
Единственный SKIP — `Persistence.StoragePath.FileSymlinkEscape`, Windows privilege
1314; не PASS. Финальные XML содержат по 1171 уникальному имени, exact coverage
соответствует собственному production inventory каждой конфигурации.

Discovery: 1310 case-sensitive unique names в каждой конфигурации, production 1171.
139 non-production cases не запускались. Четыре прежних GUI labels с pointer bytes
различаются между Debug/Release; raw имена не нормализованы и tests не исключались.
Executable/filter commands совпали после замены только build directory конфигурации.

[Сохранённый комплект XML/logs/inventories/provenance](../../benchmarks/density064/README.md),
[итоговые машинные результаты](../../benchmarks/density064/results.json),
[источники и binaries SHA-256](../../benchmarks/density064/provenance.json).
Gzip сохраняет исходные bytes; hashes проверены обратным распаковыванием. Финальный
source aggregate (152 source/build/test/QML файла):
`db4143415571b3fc69ff31f08dfa9f0acb190ae550e0914431a7f6a34f6db88d`.
Binaries получены локальными builds на указанном baseline + implementation diff;
документация/evidence не входят в source aggregate. В репозиторий binaries не добавлены.

После полного production-прогона изменён только устаревший API-комментарий о receiver.
Build/all_qmllint и 47 новых cases повторены в Debug/Release — PASS. Полный production
после этой правки комментария не повторялся. `provenance.json` относится к финальным
sources/binaries; [provenance-before-final-comment.json](../../benchmarks/density064/provenance-before-final-comment.json)
сохраняет точные source/binary hashes полного production-прогона.

Новые production tests вызывают public controller и прежний runner/C1/D builder с
реальным маленьким Point Cache. Независимый Reader проверяет literal boundary counts
в нескольких tiles/groups; expected не вычисляется production Mapping. Source удалён,
cache и предыдущие D SHA-256 сохраняются после повторных Build и нового cell. Есть
first-Build Undo null, смешанная история, redo truncation, Ready/prepare late edits,
relevant invalidation, identity null/collision, history bad_alloc/exhaustion, отказ/reset
stale/Cancel/Confirm, nested selection ABA и отказ rebase уже после Confirm.
Reentrant Close/Shutdown/detach даёт Discard и worker cleanup; accepted D переживает
Close до handoff и destruction/import replacement. Exclusive native locks доказывают
отсутствие D reopen/readback/hash в gate/handoff и открытых handles в каталоге.
Duplicate Ready не создаёт вторую версию/команду; notifications видят settled state.

Inherited production regressions дополнительно проверяют Scan interleavings,
receiver/source ABA, malformed/stale Ready, cancellation phases, locked/foreign cleanup,
worker/native/TLS observation и v1 late edits/recovery conflict. Эти проверки не
объявляются новыми cases 06.4. Resource XML properties: sizeof descriptor **760 bytes**,
version **768 bytes** в обеих конфигурациях, отдельно shared control block, path storage,
empty Rough allocation и O(V + H) vectors; это не process memory measurement или cap.

Промежуточный адресный Debug run: 39 PASS / 1 FAIL из 40. Open fixture
ожидала dirty prompt до выбора manifest; после исправления сначала выбирается файл,
затем строго проверяется отказ Save и сохранность текущего проекта. Assertions
не ослаблены. Initial XML/log сохранены отдельно от финальных.

Два промежуточных build failures вызваны ошибочно параллельными Ninja builds одного
Debug directory (C1083 Permission denied на object и LNK1104 на core library).
После этого основной исполнитель допускает только один Ninja на build directory;
Debug/Release builds в разных directories могут выполняться параллельно, discovery
и полные CTest-прогоны конфигураций выполняются последовательно.
Это не failure компиляции исходного кода; ошибки и их причина не скрываются.

Первый полный Debug production run: **1167 PASS / 3 FAIL / 1 SKIP** из 1171.
Три существующих PartialRecovery GUI cases выявили регрессию нового moveHistory:
проверка пустой истории предшествовала отказу при недоступном Cache, поэтому Undo
возвращал NoChange вместо прежнего Rejected/DatasetUnavailable. Порядок исправлен:
пустая история при недоступном Cache сохраняет прежний отказ; существующая typed
команда выбора версии по-прежнему может работать без Cache. Существующие тесты
не изменены и не ослаблены; это адресные regression tests для сохранения v1 поведения.
Первоначальные XML/log/provenance хранятся с `before-v1-fix`, финальные — отдельно.

Known Open Closing/Ready observation остаётся ограничением, а не объявленным fix.
Representative real scans, cold HDD/UNC, network/physical disk faults, power-loss,
большой process/Peak Working Set и Windows symlink privilege отдельно не проверялись.
Synthetic/injected tests не подтверждают Rough/Precise методы на реальных scans.
Native ручная GUI-приёмка нового состояния каталога не заявляется: GUI Build отсутствует.
Применимые Save/Close/Open/Import/Recovery/QML workflow проверяются автоматическими
production tests; ручной повтор для проекта без карт: Import → cell → Save → dirty
Close/Cancel/Discard, затем Open/Recovery. Доступ к новым версиям сейчас через C++ API.

## Статус

**READY FOR INDEPENDENT REVIEW.** Окончательные Debug/Release builds/lint,
новые cases, v1 regression и полный production прошли с указанным symlink SKIP.
Известное Open observation и непроверенные эксплуатационные условия сохранены
как ограничения. Итоговый diff проверен; самостоятельное принятие этапа не объявляется.
Изменение подготовлено отдельным implementation commit, push не выполнялся.
06.5–06.8 и весь пользовательский workflow не объявляются готовыми.
