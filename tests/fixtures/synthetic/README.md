# Synthetic fixtures

Входы Synthetic Point Cloud Generator по [канонической спецификации](../../../docs/tools/synthetic-cloud-generator.md). `geometry/` содержит идеальную геометрию в миллиметрах, `scans/` — независимые переиспользуемые сценарии измерения. Geometry и scan выбираются отдельно; файлы для каждой их комбинации не нужны. [Реализация](../../../tools/synthetic_cloud_generator/README.md) поддерживает все восемь geometry и все 12 scenarios ниже. Generated XYZ/ASC и manifest здесь не хранятся; большие облака воспроизводятся из двух JSON, seed и версии генератора.

| Geometry | Форма / назначение |
|---|---|
| [rectangle](geometry/rectangle.json) | 100 × 60 мм, четыре LINE; базовый scan и разбиение длинных интервалов |
| [circle](geometry/circle.json) | Центр (50, 50), Ø100 мм; самостоятельный внешний CIRCLE |
| [triangle](geometry/triangle.json) | Основание 100 мм, высота 60.01 мм; при Y = 60 интервал ≈0.0167 мм, меньше pointStep 0.07 мм |
| [rectangle_one_circle](geometry/rectangle_one_circle.json) | 100 × 60 мм, внутренний Ø20 мм; разделение scan line |
| [rectangle_multiple_circles](geometry/rectangle_multiple_circles.json) | 120 × 80 мм; диаметры 10, 10.4, 20, 14, 14.4 мм, независимые границы и будущие HoleGroup tests |
| [rectangle_cutout](geometry/rectangle_cutout.json) | 100 × 60 мм, внутренняя пустая область 30 × 20 мм из LINE |
| [line_arc_part](geometry/line_arc_part.json) | Прямоугольная часть 80 × 40 мм с правым полукруглым торцом R20; clockwise ARC через нулевой угол |
| [complex_part](geometry/complex_part.json) | Пластина 140 × 90 мм, четыре угла R10, три круглые пустые области и паз 36 × 16 мм; LINE/ARC/CIRCLE |

| Scan | Сценарий |
|---|---|
| [clean_horizontal](scans/clean_horizontal.json), [clean_vertical](scans/clean_vertical.json) | pointStep 0.07 мм, lineStep 1 мм, maxLineLength 200 мм, без offsets/defects |
| [short_segments_horizontal](scans/short_segments_horizontal.json) | maxLineLength 25 мм: прямоугольный интервал 100 мм разбивается на четыре segments |
| [clean_horizontal_vertical](scans/clean_horizontal_vertical.json) | Полный Horizontal, затем полный Vertical; pointStep 0.07 мм, lineStep 1 мм, без shifts/regions/defects и deduplication |
| [shifted_passes](scans/shifted_passes.json) | Три последовательные полосы по X; небольшие shifts и смещённые продолжения каждые 20 мм |
| [overlapping_passes](scans/overlapping_passes.json) | Две полосы с перекрытием X = 45..65 мм и различимыми scan lines |
| [double_scan](scans/double_scan.json) | Полный проход и повтор области (20, 10)..(80, 50) с offset |
| [missing_points](scans/missing_points.json) | Редкие короткие пропуски 1–3 samples внутри region (20, 10)..(100, 70), probability 0.01; endpoints сохраняются |
| [outside_grid_cloud](scans/outside_grid_cloud.json) | Отдельная сетка 10 × 10 мм, шаг 0.5 мм, X = 155..165 мм |
| [jagged_boundary](scans/jagged_boundary.json) | Локальная внешняя граница в области (-1, 15)..(18, 35), амплитуда 0.6 мм |
| [extra_table_fragment](scans/extra_table_fragment.json) | Отдельный участок 30 × 60 мм, X = 155..185 мм |
| [mixed_artifacts](scans/mixed_artifacts.json) | Перекрывающиеся полосы, локальный повторный scan, небольшие shifts, grid cloud и jagged boundary |

Все сценарии имеют фиксированный seed. Их regions заданы в абсолютных миллиметрах; данный набор совместим с восемью geometry выше, но для произвольной будущей детали расположение regions нужно проверять. Все детали лежат в пределах X = 0..140 мм, Y = 0..100 мм; внешний мусор размещён справа с зазором не менее 15 мм. Jagged Boundary выбирает только небольшой участок outer boundary. Повторные measurements не должны deduplicate; artifacts не меняют ground truth и не передают CADContour2D подсказки об ошибочных точках. CADContour2D не обязан автоматически классифицировать synthetic artifacts как ошибки сканирования: без контекста детали решение остаётся за пользователем.

[clean-output-hashes.json](clean-output-hashes.json) хранит SHA-256 и counts всех 8 geometry × 4 clean scenarios, фактически полученных generator 0.3.0 на windows-release до реализации artifacts. Финальная проверка generator 0.4.0 в Debug и Release сохранила все 32 hashes; восемь artifact scenarios воспроизводятся побайтово вместе с manifest. Baseline не обновляется автоматически; intentional изменение clean output требует отдельного подтверждения причины.

Набор готов для текущих задач разработки CADContour2D: clean clouds служат базовым алгоритмическим тестам, artifact clouds — проверкам workflow и устойчивости. Пользователь может вручную удалить/исправить проблемные данные, построить свой контур или пересканировать слишком плохую деталь. Synthetic data не заменяют representative real scans. Дополнительные datasets добавляются только по необходимости; generated outputs, manifests и measurements остаются в `build/` вне Git.

Отдельная performance пара: [large_plate.geometry.json](performance/large_plate.geometry.json) — пластина 3000 × 1000 мм с углами R50 и двумя круглыми отверстиями Ø200; [clean_horizontal.scan.json](performance/clean_horizontal.scan.json) — Horizontal, pointStep 0.07 мм, lineStep 1 мм, maxLineLength 200 мм. Inputs не содержат artifacts или randomness; XYZ ожидается порядка 1 GB. Эта пара не входит в малую integration matrix и не генерирует гигабайтный output при обычном запуске tests. Команда и фактический performance результат приведены в README инструмента.
