# Спецификация Synthetic Point Cloud Generator

## 1. Назначение и статус

Это каноническая спецификация вспомогательного инструмента разработки и тестирования CADContour2D. Генератор создаёт искусственные плоские облака, имитирующие проходы реального сканирования, для проверки импорта, cache, density map, rough-stage, reduced-cloud extraction, precise-stage, ручной обработки, regression tests и нагрузки.

Генератор не входит в production workflow и `CADContour2D.exe`. Реализация на C++20 находится в `tools/synthetic_cloud_generator/`: два JSON-входа, validation LINE/ARC/CIRCLE, Horizontal/Vertical, sampling, maxLineLength и streaming XYZ/ASC с manifest. Geometry и scan fixtures находятся в `tests/fixtures/synthetic/`.

Реализованы passes в порядке массива, включая Horizontal + Vertical, regions, offsets, continuation shifts, overlap/double scan и четыре synthetic artifacts: MissingPoints, OutsideGridCloud, JaggedBoundary и ExtraTableFragment. Независимые measurements сохраняются без deduplication; mixed case является обычной композицией этих механизмов. Cancellation и crash recovery пока не реализованы. Проверенные команды приведены в [README инструмента](../../tools/synthetic_cloud_generator/README.md).

Текущий контракт реализован и проверен в Debug и Release; инструмент готов к использованию при разработке CADContour2D. Новые fixtures или artifacts добавляются позднее только по необходимости.

```text
Geometry JSON + Scan Scenario JSON
                ↓
Synthetic Point Cloud Generator
                ↓
XYZ / ASC + manifest JSON
```

Geometry JSON является каноническим ground truth. Scan Scenario JSON задаёт только получение measurements. Одна и та же geometry используется с разными scan scenarios без изменения ground truth.

DXF не является входом генератора. NX import, DXF parser, чтение HEADER/TABLES/BLOCKS/OBJECTS и сборка контуров из DXF не требуются. POLYLINE/LWPOLYLINE, ELLIPSE, SPLINE/BSpline и INSERT не поддерживаются как вход генератора или его ground-truth primitives.

Решение отказаться от NX DXF принято после [исторического анализа fixtures](../nx-dxf-fixture-analysis.md): их содержимое имеет признаки экспорта проекции и отличается от ожидаемой геометрии. Происхождение конкретных эллипсов по DXF не доказано; это не основание расширять первую версию. DXF остаётся будущим транспортным выходом основной программы для NX по [requirements.md, §34](../requirements.md#34-экспорт-dxf).

## 2. Два независимых входа

Используются два отдельных UTF-8 JSON-файла. Geometry не включает scenario, scenario не включает geometry и не меняет её. Пути к обоим входам передаются при запуске; пользователь может выбирать сценарий независимо от детали.

Для обоих типов файлов первая версия использует `formatVersion = 1`. Поля и значения ниже регистрозависимы; числа должны быть представимыми конечными числами. Неизвестные поля, неизвестная версия или неизвестный тип объекта отклоняются, а не молча игнорируются. Используется штатный JSON parser Qt; отдельное обнаружение повторяющихся keys и собственный tokenizer/parser не требуются. Авторы входов должны использовать уникальные keys. Миграция несуществующих опубликованных версий не требуется.

Расстояния, координаты и размеры задаются в миллиметрах; направление осей — обычная декартова XY. Углы геометрических дуг — в радианах. Радиусы, шаги, длины и регулярные амплитуды положительны; randomAmplitude может быть нулевой. Координаты, углы и смещения могут иметь любой конечный знак; нулевое смещение допустимо и соответствует чистому скану. Seed и индексы — целые, а не физические размеры.

## 3. Geometry JSON

### 3.1. Корневые поля

| Поле | Значение / назначение |
|---|---|
| `formatVersion` | Обязательное целое `1` |
| `units` | Обязательная строка `mm`; другие единицы не конвертируются автоматически |
| `outerContour` | Ровно один объект контура по §3.2 |
| `innerContours` | Обязательный массив из `0..N` объектов контура по §3.2; для детали без пустых областей — пустой массив |

Других нормативных полей первой версии нет. В geometry запрещены scan step/direction, noise, defects, seed и параметры ScanPass. Не требуется сохранять CAD constraints, construction history, слои, виды, материалы или 3D body. Это описание идеальной плоской области, а не полноценный CAD file format.

### 3.2. Контур и примитивы

Итоговая геометрия соответствует [requirements.md, §24](../requirements.md#24-точная-геометрия): только **LINE + ARC + CIRCLE**.

Контур имеет одну из двух форм:

| `type` контура | Поля | Смысл |
|---|---|---|
| `CHAIN` | `segments`: непустой упорядоченный массив LINE/ARC | Замкнутая последовательность отрезков и круговых дуг |
| `CIRCLE` | `center`, `radius` | Самостоятельная аналитически замкнутая окружность |

`CHAIN` — контейнер последовательности, не четвёртый геометрический примитив и не DXF POLYLINE. CIRCLE не помещается в `segments` и не соединяется с другими primitives своего контура.

Точка `start`, `end` или `center` — массив ровно двух конечных чисел `[x, y]` в миллиметрах. Z отсутствует.

| Primitive `type` | Обязательные поля | Геометрический смысл |
|---|---|---|
| `LINE` | `start`, `end` | Отрезок от start к end; нулевая длина недопустима |
| `ARC` | `center`, `radius`, `startAngle`, `sweepAngle` | Направленная круговая дуга; `radius > 0`, `0 < abs(sweepAngle) < 2π` |
| `CIRCLE` | `center`, `radius` | Полная окружность с `radius > 0`, только самостоятельный контур |

Положительный `sweepAngle` задаёт движение против часовой стрелки, отрицательный — по часовой. Нулевой `startAngle` направлен вдоль +X. Начало и конец ARC вычисляются по единственному определению, без дублирования endpoints в JSON:

```text
P(t) = center + radius * (cos(t), sin(t))
start = P(startAngle)
end   = P(startAngle + sweepAngle)
```

ARC с полным оборотом не допускается: полная окружность задаётся CIRCLE. Направление обхода CHAIN может быть любым; сторона материала определяется явным положением в `outerContour`/`innerContours`, а не winding convention. Это не предписывает orientation convention основной precise model.

### 3.3. Замкнутость и topology

Каждый контур обязан быть замкнут. Для CHAIN конец каждого segment совпадает с началом следующего, включая последнее соединение с первым. Порядок уже задаётся JSON; генератор не переставляет и не разворачивает segments для поиска замкнутого контура.

Один geometry-файл описывает одну связную деталь:

- ровно один простой внешний контур;
- все внутренние контуры строго внутри внешнего;
- самопересечения, пересечения и касания независимых границ запрещены;
- наложение segments и возврат по уже пройденной границе запрещены;
- вложенные внутренние контуры и material islands не поддерживаются;
- общие endpoints соседних LINE/ARC своего контура разрешены и необходимы для C0.

Замкнутость является свойством определений geometry. Используются double, небольшие scale-aware coordinate tolerances и отдельный angular tolerance; строгий framework распространения погрешностей не требуется. Это не пользовательский fitting tolerance и не разрешённая величина реального зазора. Нельзя молча перемещать endpoints, закрывать разрывы, объединять независимые близкие границы или аппроксимировать неподдерживаемую кривую. Shared vertices определяются последовательностью контура. Tolerance не задаёт minimum geometry/interval size; действительно неразрешимая численная неоднозначность завершается диагностикой.

Внутренние контуры означают пустые области. Генератор не различает hole и cutout: эта семантика относится к CADContour2D. Идеальная поверхность — область внутри outerContour за вычетом внутренностей innerContours. Граничные точки принадлежат границе поверхности и могут быть измерены; внутри пустых областей точки основной детали не создаются.

## 4. Scan Scenario JSON

### 4.1. Корневые поля

| Поле | Значение / назначение |
|---|---|
| `formatVersion` | Обязательное целое `1` |
| `direction` | Обязательная строка `Horizontal` или `Vertical`; направление по умолчанию для passes |
| `pointStep` | Обязательное конечное число `> 0`, целевой шаг вдоль segment, мм |
| `lineStep` | Обязательное конечное число `> 0`, шаг между scan lines, мм |
| `maxLineLength` | Обязательное конечное число `> 0`, максимальная идеальная длина segment, мм |
| `seed` | Обязательное целое от `0` до `4294967295`; применяется ко всей случайности сценария |
| `passes` | Необязательный непустой массив объектов ScanPass по §4.2; отсутствие означает один проход всей детали без смещений |
| `defects` | Необязательный массив объектов по §7; отсутствие / пустой массив означает отсутствие injected defects |

Geometry, её primitives и hints для восстановления в scenario не включаются. Произвольного `scanAngle` нет. Типичные реальные значения: `pointStep ≈ 0.07 мм`, `lineStep ≈ 0.5–1.5 мм`; они не являются defaults или жёсткими допустимыми пределами. Основные шаги всегда задаются явно.

### 4.2. ScanPass

ScanPass описывает отдельное виртуальное измерение. Все passes наследуют положительные шаги корневого scenario; направление по умолчанию также наследуется, но может явно переопределяться через `direction` внутри pass. Поэтому один scenario может выполнять Horizontal и затем Vertical без изменения geometry или шагов.

| Поле ScanPass | Значение / назначение |
|---|---|
| `direction` | Необязательная строка `Horizontal` или `Vertical`; отсутствие наследует корневое направление |
| `region` | Необязательный прямоугольник по §4.3; отсутствие означает всю деталь |
| `lineOffset` | Необязательное конечное смещение grid origin scan lines, мм; default `0` |
| `longitudinalShift` | Необязательное конечное смещение всех measurements прохода вдоль scan line, мм; default `0` |
| `transverseShift` | Необязательное конечное смещение всех measurements прохода поперёк scan line, мм; default `0` |
| `continuationShift` | Необязательный объект с обязательными конечными полями `longitudinal` и `transverse`, мм; отсутствие означает оба `0` |

Для Horizontal продольное положительное направление — +X, поперечное — +Y. Для Vertical продольное — +Y, поперечное — +X.

Passes выполняются в порядке массива и могут частично перекрываться или повторно покрывать одну область. У повторных проходов могут различаться lineOffset и measurement shifts. Каждый проход создаёт независимый набор точек, даже если координаты совпадают. Automatic deduplication между проходами запрещена.

Пустой объект pass `{}` наследует направление и задаёт полный чистый проход. Пустой массив `passes` недопустим. Отсутствие `passes` сохраняет прежний один полный проход. Все поля таблицы реализованы; неизвестные поля отклоняются.

### 4.3. Прямоугольная область

`region` и области дефектов — объекты с обязательными точками `min` и `max`, каждая в формате `[x, y]`. Требуются `min.x < max.x` и `min.y < max.y`; все координаты конечны. Стороны параллельны XY. Произвольный polygon ROI не нужен для первой версии generator.

Область ScanPass ограничивает идеальное покрытие до применения смещений. После смещения measurements могут выходить за region или ground-truth границу: это ожидаемый scan artifact. Пустое пересечение pass region с деталью даёт ноль точек этого прохода, а не ошибку geometry.

## 5. Scan lines и sampling

Основная поверхность не заполняется обычной равномерной 2D grid. Генератор моделирует отдельные scan lines и продолжения.

Для Horizontal scan lines направлены вдоль +X, их координаты Y идут с шагом lineStep от `outerBoundingBox.min.y + lineOffset`. Для Vertical линии идут вдоль +Y, координаты X — от `outerBoundingBox.min.x + lineOffset`. Используется целочисленный индекс линии и выражение `origin + index * lineStep`, без накопительного прибавления float шага. Берутся все целые индексы, в том числе отрицательные, попадающие в bounding box идеальной детали и region данного pass; grid origin всегда общий bounding box geometry, а не origin region.

Для каждой линии:

1. Аналитически получить material intervals outer MINUS strict interiors inner.
2. Обрезать идеальные intervals по region pass, сохранив существующие contacts.
3. Разделить положительные intervals по maxLineLength.
4. Изменить выбранные contour endpoints посредством JaggedBoundary.
5. Применить base measurement shift и continuation shift.
6. Выполнить sampling измеренного segment по §5.2.
7. Применить MissingPoints к sampled points и передать оставшиеся writer.

После искажений clipping по ground-truth geometry повторно не выполняется.

Пустое пересечение не создаёт точек. Изолированное геометрическое касание с нулевой длиной создаёт ровно одну точку, без положительного segment: например, вершина треугольника или касательная к внешнему CIRCLE. Контакт, уже покрытый положительным interval, отдельно не дублируется. Положительные интервалы не отбрасываются по искусственному minimum length; около вершины треугольника создаются короткие segments. Если длина не представима с достаточной точностью вычислений, это ошибка вычисления, а не молчаливое исчезновение интервала.

### 5.1. Максимальная длина и продолжения

Для идеального интервала длиной L используются последовательные segments длиной не более maxLineLength: полные segments от начала интервала и оставшийся короткий хвост. Нет минимальной длины. maxLineLength — верхний предел, а не длина, которую следует искусственно заполнить точками вне детали.

`continuationShift` задаёт приращение measurement shift следующего segment внутри одного интервала. Для segment с индексом k (первый — `0`) смещение равно base shift прохода плюс `k * continuationShift`. Индекс начинается заново в каждом интервале каждой линии. Это моделирует ступеньки, overlap/gap и смещённые продолжения без ручного перечисления каждого segment.

Ограничение maxLineLength проверяется до measurement shifts и defect injection. Ground truth и идеальные интервалы при этом не изменяются; после jagged artifact измеренный segment может иметь другую длину.

### 5.2. Расстановка точек

Каждый segment положительной длины содержит обе граничные точки и внутренние точки с расстоянием между соседними точками `<= pointStep` до artifacts. Для длины l используется `n = ceil(l / pointStep)` равных промежутков и `n + 1` точек, с проверкой представимости n и координат. Численная коррекция счёта промежутков не должна нарушить верхний предел шага.

Общая точка двух продолжений записывается один раз только при совпадающем measurement endpoint внутри этого прохода. Если продолжения смещены, оба endpoints сохраняются. Это локальная обработка общей границы segments, а не automatic deduplication independent passes или overlap measurements.

Границы внутренних пустых областей включаются как endpoints интервалов; точки между ними не генерируются. Поперечное/продольное смещение measurements после построения идеальных интервалов может давать точки за идеальной границей, в том числе внутри идеальной пустой области; это не изменение geometry.

## 6. Несколько измерений и детерминированность

Double scan — нормальный вариант структуры реальных измерений, а не обязательный synthetic defect. Он задаётся несколькими passes без объединения их точек. Можно задавать одинаковые или различающиеся offsets, частичное перекрытие и повторное сканирование region.

Одинаковые байты обоих входных JSON, generator version и seed дают идентичные байты XYZ/ASC в поддерживаемом toolchain/profile. Manifest дополнительно зависит от output path context. В версии 0.4.0 используется std::mt19937_64 с seed derivation SplitMix64. Uniform [0,1) вычисляется из верхних 53 bits без std::uniform_real_distribution. Seed отдельного stream зависит от scenario seed, индекса pass, типа defect и его порядкового номера среди defects того же типа. Добавление независимого defect другого типа не меняет stream существующего. Изменение порядка passes или добавление того же типа перед существующим меняет его stream. RNG/derivation учитываются generator version и записываются в manifest. Скрытый seed по времени запрещён. Без случайных эффектов seed не влияет на output.

Порядок записи: passes в порядке массива; scan lines по возрастающей поперечной координате; intervals и segments вдоль положительного scan direction; точки вдоль segment. Дополнительные defect clouds записываются после основной поверхности в порядке defects. Случайное перемешивание точек не выполняется.

## 7. Synthetic defects

Defects добавляют известные тестовые артефакты и изменяют только measurements. Они никогда не изменяют geometry JSON, primitives или ground truth. Массив `defects` применяется в указанном порядке; repeated scans остаются отдельной моделью passes.

В первой версии используются следующие discriminated objects. Все перечисленные поля обязательны, кроме явно отмеченных необязательных. Новые типы добавляются по фактической необходимости, без заранее созданного каталога гипотетических артефактов.

### 7.1. Outside Grid Cloud

`type = OutsideGridCloud`; поля `region`, `pointStepX > 0`, `pointStepY > 0`.

Создаёт небольшую прямоугольную/квадратную сетку посторонних точек. Это отдельный artifact, не способ заполнения основной детали. Region должна находиться строго вне внешнего контура детали. Сетка идёт от min по X/Y с заданными шагами, до max включительно при попадании очередной точки на границу; шаг последней ячейки не подгоняется. Порядок — Y, затем X. Расположение и шаги задаются в миллиметрах и не зависят от основной scan grid.

### 7.2. Jagged Boundary

`type = JaggedBoundary`; поля `region`, `target`, `amplitude > 0`, `toothStep > 0`; необязательное `randomAmplitude >= 0` (отсутствие или ноль означают отсутствие случайной составляющей). `target` равен `outer` или `inner`; для `inner` обязательно целое `innerIndex` — индекс в geometry.innerContours, начиная с `0`. Для outer поле innerIndex не допускается.

Создаёт локальный рваный/зубчатый участок measured boundary выбранного контура в region. Участок выбирается по координатам ground-truth endpoints до measurement shifts; смещения прохода не меняют область выбора дефекта. Amplitude задаёт максимальный регулярный отступ в миллиметрах, toothStep — пространственный шаг зубцов; randomAmplitude задаёт дополнительную ограниченную составляющую с использованием общего seed. Изменяются только measurement endpoints, происходящие от выбранной ground-truth границы, а не искусственные границы region или разбиения maxLineLength. После их изменения точки соответствующих measured segments расставляются заново с тем же pointStep. Эффект распространяется на все passes, измеряющие выбранный участок.

Метод версии 0.4.0: в координатах scan (u вдоль линии, v поперёк) phase = (v - region.min.v) / toothStep, f = phase - floor(phase), displacement = amplitude * (1 - abs(2*f - 1)) + randomAmplitude * U[0,1). Выбранный левый endpoint material interval смещается на -displacement, правый на +displacement. Поэтому материал локально выступает за outer boundary или внутрь inner void; это измеренная форма, не изменение ground truth. При нескольких JaggedBoundary смещения складываются в порядке defects. Random draw выполняется только для подходящего endpoint. Короткий положительный interval не схлопывается; изолированный contact остаётся одной смещённой точкой. У contact используется смещение левого endpoint.

Это простая модель зубцов вдоль scan axis, без физической модели и перемещения по normal. При lineStep, кратном toothStep, регулярная составляющая может попадать на одну фазу; fixture должен выбирать шаги с учётом этого. Локальность означает выбор contour endpoints в region; повторный sampling удлинённого segment может изменить его внутренние samples.

### 7.3. Extra Table Fragment

`type = ExtraTableFragment`; поле `region`.

Создаёт дополнительный прямоугольный участок точек строго вне внешнего контура основной детали. Для первой версии применяется то же направление и корневые pointStep/lineStep/maxLineLength; fragment сканируется отдельным проходом без measurement shifts, с grid origin по его bounding box. Его интервалы не обрезаются geometry основной детали. Порядок scan lines/segments/points соответствует §5. Поддержка произвольной polygonal формы стола не требуется.

### 7.4. MissingPoints

`type = MissingPoints`; обязательное конечное `probability` в [0,1]; необязательные `region`, целые `minRunLength` (default 1) и `maxRunLength` (default 3), с ограничением 1 <= minRunLength <= maxRunLength <= 3. Region проверяется по измеренным sampled координатам после shifts. Отсутствие region означает весь pass. Неизвестные поля отклоняются; segment endpoints всегда сохраняются.

После sampling для каждой подходящей внутренней точки, если предыдущая серия закончена, U < probability запускает пропуск целой длины minRunLength..maxRunLength (выбор через RNG modulo диапазона). После серии минимум одна подходящая точка сохраняется до следующей попытки. На endpoint, выходе из region и новом segment состояние серии сбрасывается; серия может быть укорочена этими границами. Probability — вероятность начала серии на eligible point, а не доля всех удалённых samples. Линия целиком не удаляется. Память — небольшое состояние на defect текущего segment; второй проход по готовому cloud не нужен. Несколько MissingPoints применяются как объединение решений об удалении; итоговая объединённая серия может превышать предел отдельного instance.

## 8. Критическое правило по дефектам

CADContour2D не должен надёжно определять без контекста применения детали, является ли участок облака реальной геометрией, вырезом, выступом, мусором, ошибкой сканирования или столом. Решение об ошибочных точках остаётся за пользователем. Это не отменяет существующие локальные численные проверки, rough classification и robust estimation, но запрещает считать реальные features шумом только ради удобного fit.

Synthetic defects нужны для визуальной проверки, ROI, ручного удаления точек, ручной корректировки и проверки устойчивости дальнейших алгоритмов. Они **не создают требования automatic defect detection**. Тест не считается неуспешным только из-за отсутствия автоматического распознавания искусственного мусора.

В обычном тесте CADContour2D получает только XYZ/ASC. Geometry JSON, scenario JSON и manifest используются разработчиком/тестовой инфраструктурой, не передаются алгоритму как boundary, defect labels, пометки материала или подсказка о столе.

## 9. XYZ / ASC и streaming

Выход — текстовый `.xyz` или `.asc` без header, ASCII, одна строка `X Y 0`, миллиметры, locale-independent десятичная точка и LF. X/Y имеют ровно шесть знаков после точки; `-0.000000` нормализуется в `0.000000`. Формат закрепляется generator version и проверяется повторным чтением. Максимальная погрешность округления одной координаты — около 0.0000005 мм; sampling spacing задаётся до сериализации. В чистом проходе схлопывание положительного material interval или положительного зазора внутренней области при округлении даёт явную ошибку. После artifacts проверяется сохранение положительного measured segment; намеренно изменённые gaps и выход за ground truth не исправляются. Bounding box manifest рассчитывается по записанным округлённым координатам.

Большое облако не хранится целиком в RAM: interval → segment → ограниченный буфер точек → запись → освобождение временных данных. Geometry и компактная scenario могут находиться в памяти. Это позволяет генерировать миллионы и десятки миллионов точек без памяти, пропорциональной размеру output. Полный глобальный массив точек ради deduplication не создаётся.

Перед созданием datasets проверяются параметры, representability, arithmetic overflow, возможность вычислить количество samples и доступность ресурсов. Универсальный неподтверждённый RAM-limit не вводится.

Неудачная генерация не публикуется как готовый dataset и не уничтожает предыдущий корректный output. При существующем output или manifest текущая реализация отказывает без перезаписи. Оба файла готовятся во временных файлах рядом с target; после flush/close manifest публикуется первым, output последним. При обработанной ошибке временные файлы удаляются, при ошибке публикации output созданный manifest откатывается. Два rename не являются общей crash-atomic транзакцией: аварийное завершение может оставить manifest без output. Полноценные crash recovery и cancellation subsystem не входят в базовый этап.

## 10. Manifest и воспроизводимость

Базовая реализация всегда записывает `<output>.manifest.json` рядом с output. Успешная публикация соответствует полностью записанному output.

Manifest содержит как минимум:

- generator version;
- пути/идентификаторы geometry JSON и scan scenario JSON;
- SHA-256 исходных байтов обоих JSON;
- seed;
- число созданных точек, включая повторные измерения и artifacts;
- bounding box фактических output points, а не только ideal geometry;
- направление, pointStep, lineStep и maxLineLength;
- путь, формат и SHA-256 output.

Manifest содержит `passes` в порядке записи: resolved direction, region (null для полной детали), lineOffset, longitudinalShift, transverseShift, continuationShift и фактический pointCount после inline defects. Корневое `direction` хранит default, общие шаги задаются корневыми полями. `defects` сохраняет resolved parameters, standalonePointCount и для JaggedBoundary/MissingPoints randomStreams (id типа/ordinal/pass, seedHex). Поле rng фиксирует алгоритмы RNG/seed derivation. Общий count равен сумме pass pointCount и defect standalonePointCount; inline defects имеют standalonePointCount = 0. Input JSON целиком не дублируется.

Для пустого output число точек равно нулю, bounding box отсутствует (`null`), а не выдумывается. Metadata не должны требовать хранения всего облака: счётчик и bounds обновляются потоково. Имена выходных manifest fields остаются деталями интерфейса инструмента; входные JSON поля определены §§3–4, 7.

Концептуальное использование будущих fixtures:

```text
geometry/part_01.json + scans/clean_horizontal.json
    → part_01_clean_horizontal.xyz + manifest
geometry/part_01.json + scans/double_scan.json
    → part_01_double_scan.xyz + manifest
```

Это примеры путей и pipeline, не созданные файлы или проверенные команды.

## 11. Ошибки и диагностика

Чтение и generation завершаются понятной ошибкой и ненулевым exit code при:

- отсутствии / ошибке чтения любого из двух JSON;
- неверном JSON, неизвестных fields/version/type;
- неверных units, неконечных или непредставимых значениях;
- неверных primitives, нулевой LINE или недопустимых ARC/CIRCLE;
- отсутствии outerContour, открытом CHAIN или нарушении topology;
- недопустимом direction, steps, seed, passes, regions или defect parameters;
- ссылке JaggedBoundary на несуществующий внутренний контур;
- OutsideGridCloud/ExtraTableFragment region, пересекающей/касающейся основной внешней области;
- вычислительной неоднозначности, overflow, недоступных ресурсах;
- невозможности создать или полностью записать output/manifest.

Диагностика указывает входной файл и поле/индекс контура, segment, pass или defect, где обнаружена проблема. Некорректная geometry не исправляется молча. Частичный результат не становится success. Cancellation пока не реализована. Отсутствие точек у отдельного корректного pass не является failure.

CLI принимает `--geometry <file> --scan <file> --output <file.xyz|file.asc>`, а также `--help` и `--version`. Успешный stdout summary содержит число точек и output path; параметры воспроизведения сохраняются в manifest. Diagnostics выводятся в stderr, ошибки дают ненулевой exit code. Проверенные команды — в README инструмента. GUI не требуется.

## 12. Будущие fixtures и размещение

Предпочтительная логическая структура:

```text
tests/fixtures/synthetic/
├── geometry/
└── scans/
```

Geometry fixtures и все 12 scan scenarios реализованы и перечислены в [README fixtures](../../tests/fixtures/synthetic/README.md). Clean SHA-256 regression baseline сохранена отдельно для 8 geometry × 4 clean scenarios. Компактная performance geometry/config пара находится в `tests/fixtures/synthetic/performance/`. Пользователь не обязан вручную описывать каждую деталь.

Generated XYZ/ASC, manifests и временные measurements хранятся локально вне Git. Их воспроизводят из двух JSON, seed и generator version; в Git остаются inputs, tests и документация. Нагрузочные сценарии порядка 100k, 1M, 10M, 50M+ points полезны локально, но эти размеры не являются контрактом.

Clean datasets используются для базовых алгоритмических тестов; artifact datasets — для проверки пользовательского workflow и устойчивости обработки. CADContour2D не обязан автоматически классифицировать synthetic artifacts как ошибки сканирования. Без контекста детали решение остаётся за пользователем: он может вручную удалить или исправить проблемные данные, построить свой контур либо пересканировать деталь, если scan слишком плохой. Synthetic data не заменяют representative real scans.

Существующие [NX DXF fixtures](../../tests/fixtures/dxf/nx/) сохраняются без изменений. Они не являются основным положительным ground truth генератора. Возможное применение как negative/diagnostic fixtures решается отдельно, не требует DXF reader в generator.

Исходники инструмента отделены от production application в `tools/synthetic_cloud_generator/`. README инструмента описывает проверенные команды, поддерживаемый этап, reproducibility и ограничения. Основной README CADContour2D содержит краткий контракт и ссылку на эту спецификацию.

## 13. Проверки реализации

Минимальные геометрические тесты:

- rectangle: horizontal/vertical lines, spacing и boundary endpoints;
- triangle: положительные короткие segments у вершины, без minimum length;
- самостоятельный внешний CIRCLE: переменная длина scan intervals;
- внутренний CIRCLE и внутренний LINE/ARC cutout: несколько intervals и отсутствие ideal points внутри пустых областей;
- смешанный LINE/ARC contour: направления sweep, переход через нулевой угол, closure;
- отказ для открытого CHAIN, нулевого primitive, недопустимого типа и неверной topology;
- tangent scan line и попадание в вершину: одна point для изолированного контакта, отсутствие искусственных положительных интервалов и двойного счёта;
- maxLineLength: длинные intervals, короткий остаток, no artificial fill;
- shifted continuations: longitudinal/transverse shift, overlap/gap;
- multiple passes: region clipping, partial overlap, double scan без deduplication;
- OutsideGridCloud: отдельная сетка вне детали;
- MissingPoints: локальные короткие серии после sampling, endpoints/lines сохранены, probability=0 сохраняет исходный pipeline;
- JaggedBoundary: локальность, seed, неизменность ground truth;
- ExtraTableFragment: отдельный прямоугольный scan вне детали;
- одно geometry JSON с несколькими scenarios;
- same inputs + version + seed: идентичные output и hashes;
- повторное чтение ASC/XYZ, Z = 0, count/bounds/manifest consistency;
- неверные JSON и output I/O failures: понятная диагностика без публикации частичного результата;
- streaming большого output без массива всех generated points.

Synthetic tests не заменяют representative real scans для подтверждения методов CADContour2D. Главные приоритеты генератора: корректность geometry и scan model, детерминированность, ограниченная память, достаточная скорость. Real-time generation, SIMD и дополнительная многопоточность не обязательны.

## 14. Что генератор не должен делать

- Импортировать DXF/NX, реконструировать topology из CAD entities или реализовывать полный CAD file format.
- Поддерживать POLYLINE/LWPOLYLINE, ELLIPSE, SPLINE/BSpline, INSERT либо автоматически аппроксимировать неподдерживаемую geometry.
- Быть CAD-редактором или создавать GUI.
- Изменять ground truth ради artifacts, автоматически чинить closure/topology.
- Объединять geometry и scan configuration в один файл.
- Требовать произвольный scan angle, minimum segment length или artificial maxLineLength fill.
- Deduplicate independent passes и повторные measurements.
- Определять, какие artifacts основной алгоритм обязан автоматически распознать как defects.
- Передавать ground-truth подсказки CADContour2D в обычном тесте.
- Загружать весь большой output в RAM или смешивать scanning/defect injectors с production-кодом.
- Проектировать большой каталог будущих дефектов или добавлять новые primitives «на будущее».

Текущая версия включает весь согласованный pipeline: validation, analytic intervals и contacts, passes/regions, segmentation, measurement shifts и artifacts, sampling/MissingPoints, детерминированный streaming output и manifest. DXF, GUI, cancellation, crash-atomic transaction двух файлов, physical scanner model и automatic defect detection остаются вне текущего контракта.
