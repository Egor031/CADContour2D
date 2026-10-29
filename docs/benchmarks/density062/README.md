# D storage 06.2 — адресные Release-измерения

2026-10-29, один последовательный запуск шести synthetic trials после headless
регрессий, без параллельной сборки/тестов. Это warm-cache logical I/O, без заявлений
о physical cold-HDD, network, real scans или end-to-end C1.

- [release.json](release.json): исходные time/byte/memory counters.
- [environment.json](environment.json): CPU/RAM/OS/носитель и SHA-256 executable.
- [verification.json](verification.json): фактические результаты проверок и hashes
  локальных полных CTest logs/XML (сами большие логи оставлены в `build/`).
- [Отчёт](../../reports/task-06/density-storage.md), [нормативный format](../../density-map-format.md).

MSVC 19.51.36260 (`_MSC_FULL_VER=195136260`), C++20 x64 /O2, Qt 6.12.0,
zlib 1.3.2#1, CMake 3.30.5, Ninja 1.12.1, Windows 10.0.19045,
i5-7500 @3.40GHz, RAM 17 135 476 736 bytes. D: NTFS, Hitachi HDS721050CLA362 SATA HDD.
Наличие HDD не делает этот прогон cold-HDD измерением.

Storage side 256, T=96. Logical dimensions: 24576×256 и 1000003×1000001.
Те же 96 keys `(0..95,0)`, reverse ingest; те же counts и порядок samples.
Seed=6002, xorshift13/7/17. Compressible — все counts=1; entropy — младшие uint32
генератора; sparse — только samples 7 и 1234 равны 1. Contract SuppliedTiles v1.
Sort budget 160 bytes: два native descriptors (144 bytes), шесть merge passes;
wire index 7680 bytes. Нет RAM-array index. Размер final файла учитывает только D;
scratch/combined peaks — точные логические длины собственных файлов, не NTFS allocation.

| Profile / area | File bytes | Ingest ms | Sort/merge ms | Copy payload+index ms | Finish/readback ms | Open ms | Mapping в open ms | 5 reads ms | Full physical iteration ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| compressible / small | 34848 | 209.95 | 4.79 | 0.98 | 112.37 / 105.10 | 0.285 | 0.112 | 5.27 | 102.17 |
| compressible / large | 34848 | 202.26 | 4.76 | 0.88 | 117.24 / 110.58 | 9.224 | 9.018 | 5.18 | 103.50 |
| entropy / small | 25182144 | 710.40 | 5.02 | 18.13 | 139.12 / 111.07 | 0.310 | 0.112 | 5.66 | 104.99 |
| entropy / large | 25182144 | 726.64 | 4.56 | 35.66 | 159.76 / 115.41 | 9.006 | 8.795 | 5.03 | 103.57 |
| sparse / small | 35520 | 191.22 | 4.94 | 0.96 | 106.45 / 99.44 | 0.295 | 0.112 | 4.76 | 97.94 |
| sparse / large | 35520 | 177.94 | 4.40 | 0.82 | 109.96 / 103.61 | 8.983 | 8.792 | 5.14 | 92.79 |

Ingest timing включает bounded генерацию входного count buffer. Finish включает
sort/merge, copy, seal/flush и полный readback; readback — вложенная часть finish,
не дополнительная сумма. Input mapping, writer create и mapping total записаны отдельно.
Copy включает чтение sorted descriptors, spool payload и запись index.

Open во всех trials: 8064 read bytes = 320+64+96×80, zero decoded tiles.
Selective запросы keys 0/17/37/71/95: пять decoded tiles, без полного scan.
Full callback iteration: 96 physical tiles, decoded integrity и totals + diagnostic digest.
Парные digest/file bytes, T/index/open/decoded counters независимо сверены PowerShell.

Buffers reserve = 1 638 573 bytes; codec peak через custom allocator = 268 104 bytes,
sort array = 144 bytes. Caller count buffer = 524 288 bytes, отдельно от owned budget.
Это разные статьи internal accounting, не замер OS peak. Рабочий набор процесса
снят до/после trial (примерно 8,6–10,9 MB); `process_lifetime_peak_working_set`
до 10 964 992 bytes — Windows peak всего процесса к моменту строки, не peak отдельной
фазы или trial. Scratch peaks: 42144 / 25189440 / 42816 bytes по profiles;
combined disk peaks: 76992 / 50371584 / 78336. Пустая logical area их не увеличила.
Allocation/working-set accounting не включает все расходы Qt/allocator/OS.
Один trial на профиль/площадь, без статистических confidence/threshold claims.

В штатной x64 MSVC CMD-среде README, tools включены:

```bat
cmake --build --preset windows-release --target density_storage_benchmark --parallel 4
build\windows-release\density_storage_benchmark.exe D:/ZherlitsynEE/CADContour2D/build D:/ZherlitsynEE/CADContour2D/docs/benchmarks/density062/release.json
```

Команда перезаписывает явно указанный JSON результата. Каждый trial создаёт и
очищает собственный уникальный temporary directory; source/Point Cache не используются.
