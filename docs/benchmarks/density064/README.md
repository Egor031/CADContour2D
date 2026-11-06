# Density versions 06.4 — evidence

Результат и ограничения: [density-versions.md](../../reports/task-06/density-versions.md).
Это проверки production catalogue/selection/history и v1 guard, без real scans
или утверждения Rough/Precise методов.

В `*.gz` сохранены точные исходные bytes XML/log/JSON; gzip не нормализует текст.
`artifacts.json` содержит SHA-256 сжатого и исходного содержимого. Binaries и scans
в репозиторий не включены.

- `build-*-final-comment.log.gz` — окончательные `all all_qmllint` Debug/Release
  после правки только receiver API-комментария. `build-*-v1-fix-final.log.gz` —
  успешные сборки перед полным production.
  Предыдущие build logs сохранены отдельно; initial collision log относится к
  ошибочным параллельным Ninja одного directory, а не к финальной сборке.
- `versions-*-final.xml.gz`, `versions-*-final.log.gz` — standalone новый набор.
- `recovery-regression-*.xml.gz`, `recovery-regression-*.log.gz` — существующие
  PartialRecovery cases после исправления v1 empty-history отказа.
- `production-debug.xml.gz`, `production-release.xml.gz` и logs — финальные полные
  production прогоны. Поднаборы standalone/regression входят в них и не суммируются.
- `versions-debug-initial.*.gz` — первоначальный FAIL Open fixture.
  `production-debug-before-v1-fix.*.gz` — первоначальные 3 FAIL PartialRecovery;
  `provenance-before-v1-fix.json` и соответствующий inventory относятся к этому коду.
- `inventory-*.json.gz` — raw discovery, `production-*-names.txt.gz` — raw production
  имена. Четыре старых GUI labels печатают pointer bytes и различаются между configs;
  имена не нормализованы. `inventory-summary.json` сравнивает также executable/filter
  commands после замены только build directory конфигурации.
- `provenance-before-final-comment.json` — sources/binaries полного production.
  Затем изменён только receiver API-комментарий: повторены build/lint и 47 cases
  в каждой конфигурации; полный production после комментария не повторялся.
- `provenance.json` — baseline HEAD, SHA-256 всех 152 финальных source/build/test/QML файлов и
  использованных test executables/GUI/crash probes. Сборка выполнялась штатным
  `build/task02-env.cmd` с process-local MSVC/Qt/CMake/Ninja environment; точные команды
  приведены в отчёте. Совокупный hash исходников не включает документацию/evidence.
- `results.json` — итоговые counts, exact XML/inventory coverage и сохранённые
  первоначальные failures. SKIP не считается PASS.

Бинарные hashes относятся к локальным Debug/Release output directories после
последней сборки, без копирования чужих binaries. Сохранённый baseline — родитель
implementation commit; сам commit включает соответствующие sources/tests/docs.
