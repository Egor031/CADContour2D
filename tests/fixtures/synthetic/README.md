# Synthetic fixtures

Входы Synthetic Point Cloud Generator по [канонической спецификации](../../../docs/tools/synthetic-cloud-generator.md). `geometry/` содержит идеальную геометрию в миллиметрах, `scans/` — независимые переиспользуемые сценарии измерения. Geometry и scan выбираются отдельно; файлы для каждой их комбинации не нужны. [Базовая реализация](../../../tools/synthetic_cloud_generator/README.md) поддерживает все восемь geometry с clean_horizontal, clean_vertical и short_segments_horizontal; остальные scenarios относятся к следующему этапу и явно отклоняются. Generated XYZ/ASC и manifest здесь не хранятся; большие облака воспроизводятся из двух JSON, seed и версии генератора.

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
| [shifted_passes](scans/shifted_passes.json) | Три последовательные полосы по X; небольшие shifts и смещённые продолжения каждые 20 мм |
| [overlapping_passes](scans/overlapping_passes.json) | Две полосы с перекрытием X = 45..65 мм и различимыми scan lines |
| [double_scan](scans/double_scan.json) | Полный проход и повтор области (20, 10)..(80, 50) с offset |
| [outside_grid_cloud](scans/outside_grid_cloud.json) | Отдельная сетка 10 × 10 мм, шаг 0.5 мм, X = 155..165 мм |
| [jagged_boundary](scans/jagged_boundary.json) | Локальная внешняя граница в области (-1, 15)..(18, 35), амплитуда 0.6 мм |
| [extra_table_fragment](scans/extra_table_fragment.json) | Отдельный участок 30 × 60 мм, X = 155..185 мм |
| [mixed_artifacts](scans/mixed_artifacts.json) | Перекрывающиеся полосы, локальный повторный scan, небольшие shifts, grid cloud и jagged boundary |

Все сценарии имеют фиксированный seed. Их regions заданы в абсолютных миллиметрах; данный набор совместим с восемью geometry выше, но для произвольной будущей детали расположение regions нужно проверять. Все детали лежат в пределах X = 0..140 мм, Y = 0..100 мм; внешний мусор размещён справа с зазором не менее 15 мм. Jagged Boundary выбирает только небольшой участок outer boundary. Повторные measurements не должны deduplicate; artifacts не меняют ground truth и не передают CADContour2D подсказки об ошибочных точках.
