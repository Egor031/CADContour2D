# Algorithms

## Навигация

- [Статусы](#2-статус-алгоритмических-решений), [численные правила](#3-общие-численные-правила), [координаты](#4-системы-координат).
- [Импорт](#5-импорт-ascxyz), [density map](#6-построение-density-map), [raster mask](#8-получение-рабочей-raster-mask).
- [Rough contour](#9-поиск-rough-outer-contour), [holes](#11-поиск-rough-holes), [groups](#12-группировка-rough-holes), [cutouts](#13-поиск-rough-cutouts).
- [Extraction](#14-reduced-point-cloud-extraction), [precise pipeline](#15-общая-схема-precise-processing), [boundary support](#16-supporting-boundary-points), [segmentation](#17-segmentation-precise-contour).
- [Fitting](#19-line-fitting), [robust estimation](#23-robust-estimation), [Fixed](#26-fixed-geometry-constraints), [one-sided validation](#29-односторонний-допуск).
- [Hole refinement](#32-precise-hole-refinement), [optimization](#35-geometry-optimization), [topology](#36-проверка-топологии).
- [Тестирование](#42-тестирование-алгоритмов), [готовность](#47-критерий-готовности-алгоритма), [Open decisions](#45-указатель-основных-open-decisions).

## 1. Назначение документа

Этот документ определяет алгоритмические контракты проекта:

- входы и выходы вычислительных операций;
- обязательные свойства результата;
- используемые геометрические и численные ограничения;
- критерии успешного и неуспешного результата;
- текущие алгоритмические подходы;
- открытые вопросы, требующие экспериментов на реальных данных.

Документ является источником истины для математических и вычислительных методов:

- raster analysis;
- выделения границ;
- rough detection;
- filtering;
- fitting;
- segmentation;
- robust estimation;
- primitive selection;
- проверки геометрии;
- оптимизации количества CAD-примитивов.

Функциональное поведение определяется в `requirements.md`.

Архитектурные границы, владение данными, потоки выполнения и persistent formats определяются в `architecture.md`.

Если конкретный математический метод ещё не подтверждён, он должен быть обозначен как `Open decision`, а не фиксироваться как окончательное решение.

---

## 2. Статус алгоритмических решений

Для алгоритмических решений используются три статуса.

### 2.1. Required

Обязательное свойство результата, следующее из требований или архитектуры.

Такое свойство не может быть изменено локальной заменой алгоритма.

Пример:

```text
Required:
- Fixed geometry не изменяется автоматически.
- Precise contour соблюдает односторонний допуск.
- Контур сохраняет C0 continuity.
```

### 2.2. Current approach

Текущий выбранный алгоритмический подход.

Он может быть заменён после сравнительных экспериментов при сохранении обязательных контрактов.

### 2.3. Open decision

Решение, для которого пока недостаточно реальных данных или экспериментальных оснований.

Open decision не является окончательно выбранным методом. Правила временной реализации и документирования определены в [AGENTS.md, §4](../AGENTS.md#4-алгоритмические-статусы).

---

## 3. Общие численные правила

Все физические вычисления выполняются в model coordinates:

```text
1.0 = 1 мм
```

Необходимо различать:

```text
numerical tolerance
geometric fitting tolerance
classification threshold
noise threshold
grouping tolerance
```

Это разные величины.

Не допускается использование одного глобального фиксированного `epsilon` для:

- сравнения координат;
- классификации отверстий;
- проверки касательности;
- fitting;
- группировки размеров;
- проверки допуска.

Допуски должны соответствовать:

- физическому смыслу операции;
- масштабу геометрии;
- `cell`;
- пользовательскому tolerance;
- точности исходных данных.

Floating-point equality не должна использоваться для геометрических решений без контекстного допуска.

---

## 4. Системы координат

Алгоритмы используют две рабочие системы координат.

### 4.1. Model coordinates

Физические координаты детали в миллиметрах.

В них выполняются:

- precise fitting;
- reduced-cloud extraction;
- измерение диаметра;
- расстояния;
- геометрические допуски;
- topology validation.

### 4.2. Density-map coordinates

Целочисленные координаты PNG.

Один пиксель соответствует:

```text
cell × cell мм
```

Raster algorithms могут работать в pixel coordinates, но результаты rough geometry должны преобразовываться обратно в model coordinates.

View/screen coordinates не используются вычислительными алгоритмами.

---

## 5. Импорт ASC/XYZ

### Purpose

Потоково разобрать исходный текстовый point cloud и передать точки в persistent cache без загрузки всего файла в RAM.

### Input

```text
.asc / .xyz
```

Строка:

```text
X Y Z
```

### Output

- последовательность точек для cache writer;
- bounding box;
- количество точек;
- информация о наличии ненулевого `Z`;
- ошибки входного формата.

### Required

- файл читается последовательно;
- полное облако не хранится в RAM;
- `.asc` и `.xyz` обрабатываются одинаково;
- `Z` считывается даже при дальнейшем использовании только `X/Y`;
- наличие хотя бы одного `Z != 0` должно быть обнаружено;
- некорректная строка должна отличаться от нормального EOF.

### Current approach

Потоковый parser с небольшим буфером чтения.

Для каждой корректной строки:

```text
parse X
parse Y
parse Z
validate
update metadata
write X/Y to cache
```

Конкретная реализация parser определяется кодом и профилированием.

Использование дорогостоящего parsing-подхода с большим количеством временных строк или потоковых объектов на каждую точку следует избегать, если измерения подтверждают существенные затраты.

### Failure conditions

- невозможно открыть файл;
- некорректная строка;
- отсутствует одно из трёх значений;
- значение невозможно представить используемым числовым типом;
- ошибка записи cache;
- отмена пользователем.

---

## 6. Построение Density Map

### Purpose

Преобразовать point cloud в фиксированную raster density map с заданным физическим размером `cell`.

### Input

- persistent point cache;
- bounding box;
- `cell > 0`;
- параметры encoding.

До построения проверяются конечность параметров, `cell > 0`, представимость raster dimensions и требуемых объёмов хранения, arithmetic overflow и возможность использования ресурсов. Невозможность построения означает безопасный failure без публикации частичного результата.

### Output

- PNG;
- `cell`;
- origin;
- width;
- height;
- параметры построения.

### Required

Каждой точке соответствует ровно одна raster cell.

```text
1 pixel = cell × cell мм
```

Zoom не участвует в вычислении.

Изменение `cell` означает полное построение новой density map.

### Rasterization

Для точки `(x, y)` должен существовать однозначный mapping:

```text
model coordinates → integer cell coordinates
```

Mapping обязан одинаково использоваться:

- при построении PNG;
- при преобразовании rough geometry;
- при отображении соответствия raster/model data.

#### Open decision: orientation and boundary convention

Необходимо окончательно определить:

- origin density map;
- направление raster Y;
- обработку точки на правой/верхней границе bounding box;
- точные правила `floor/ceil`.

Это должно быть единым правилом всего проекта.

### Накопление плотности

Для каждой cell вычисляется:

```text
count = количество точек в cell
```

Представимость счётчиков и арифметики накопления проверяется; overflow не должен искажать density map.

Алгоритм не должен требовать хранения исходного point cloud целиком.

### Point count → intensity

#### Required

Если:

```text
countA < countB
```

то отображаемая интенсивность B не должна быть меньше интенсивности A.

#### Open decision

Не определены окончательно:

- PNG bit depth;
- линейное или нелинейное отображение;
- saturation;
- нормализация;
- использование глобального максимума;
- сохранение дополнительной информации о mapping.

Это необходимо определить до окончательной реализации raster detection, поскольку rough algorithms анализируют именно PNG.

---

## 7. Общие правила Rough Stage

### Required

Rough algorithms используют:

```text
Density PNG
```

а не исходные реальные точки.

Rough stage предназначен для приблизительного определения:

- внешнего контура;
- отверстий;
- внутренних вырезов.

Высокоточное CAD fitting на этом этапе не выполняется.

Результат rough stage хранится в model coordinates.

Raster noise не должен автоматически превращаться в большое количество геометрических объектов.

---

## 8. Получение рабочей raster mask

Многие rough algorithms требуют разделения:

```text
foreground / probable material
background / empty area
```

### Input

- density PNG;
- выбранная пользователем ROI;
- raster-analysis parameters.

### Output

Рабочая mask или эквивалентное внутреннее представление. Rectangular/lasso ROI ограничивает анализ, не изменяя исходную density PNG; raster boundary ROI и model mapping реализуются согласованно.

### Required

Алгоритм должен быть устойчив к:

- отдельным шумовым пикселям;
- небольшим локальным разрывам;
- изменению плотности точек внутри детали.

При этом фильтрация не должна без достаточной причины уничтожать реальные геометрические особенности.

### Open decision

Не выбран окончательно метод получения mask:

- user-controlled threshold;
- automatic threshold;
- adaptive threshold;
- иной критерий.

Также не определены окончательно morphology/filtering operations.

Выбор должен быть сделан по реальным density maps.

---

## 9. Поиск Rough Outer Contour

### Purpose

Получить приблизительную замкнутую границу выбранной детали.

### Input

- density PNG или рабочая mask;
- выбранная ROI;
- `cell`;
- rough parameters.

### Output

Замкнутая rough-полилиния в model coordinates.

### Required

- результат должен описывать внешнюю границу выбранной детали;
- внутренние отверстия и вырезы не становятся частью внешнего контура;
- результат должен быть замкнут;
- точные LineSegment/CircularArc/Circle на этом этапе не требуются; rough polyline не является итоговым CAD-примитивом или DXF POLYLINE/LWPOLYLINE;
- при неоднозначности нескольких независимых кандидатов операция сообщает об этом пользователю, а не выбирает автоматически самую большую деталь.

### Current approach

Общая последовательность:

```text
density PNG
    ↓
foreground classification
    ↓
connected-region analysis
    ↓
outer raster boundary
    ↓
polyline simplification
    ↓
model coordinates
```

Конкретные OpenCV operations не являются контрактом документа.

### Polyline simplification

Не требуется сохранять вершину для каждого raster boundary pixel.

Rough contour должен быть достаточно подробным для:

- ручной корректировки;
- последующего определения области выборки реальных точек.

Но он не должен без необходимости содержать тысячи близко расположенных вершин.

#### Open decision

Не определён окончательный simplification method и его физически осмысленный tolerance.

---

## 10. Повторный поиск Rough Outer Contour

Повторный вызов является полным новым автоматическим построением.

Алгоритм получает текущие raster inputs и не пытается сохранить соответствие с вручную изменёнными вершинами предыдущего результата.

Предупреждение, Undo внутри rough-stage и замена project state относятся к application layer. При изменении rough-входов после начала precise-stage application layer выполняет подтверждённый полный reset без Undo по [requirements.md, §21](requirements.md#21-переходы-между-этапами).

---

## 11. Поиск Rough Holes

### Purpose

Найти приблизительно круглые внутренние пустые области.

### Input

- density PNG / рабочая mask;
- текущий rough outer contour;
- `cell`;
- minimum hole diameter;
- optional maximum hole diameter;
- пользовательские исключения текущего rough-state для обычных последующих операций.

Диапазон размеров проверяется до поиска; заданные границы должны быть допустимыми конечными значениями. Неограниченная верхняя граница является допустимым состоянием параметра.

Текущий rough outer contour может содержать ручные изменения пользователя и должен использоваться в текущем состоянии.

### Output

Набор rough circles:

```text
center
radius
quality
```

в model coordinates.

### Candidate extraction

Рассматриваются внутренние пустые connected regions, расположенные внутри текущего outer contour.

Кандидаты вне текущего диапазона исключаются не только из holes: их области не становятся автоматически cutouts, не используются для reduced-cloud extraction и не создают precise-объекты. То же правило действует для вручную удалённых отверстий при обычных последующих операциях.

Явный полный повторный Find Rough Holes строит новый набор без сопоставления со старым. Ручные перемещения, удаления, добавления и размеры прежнего набора не сохраняются; удалённое отверстие может быть найдено заново. Текущий размерный фильтр применяется снова. Предупреждение и замена состояния относятся к application layer; канон поведения — [requirements.md, §§13–18](requirements.md#16-повторный-поиск-отверстий).

### Circle classification

Кандидат должен оцениваться на соответствие окружности.

Возможные признаки:

- area;
- perimeter;
- circularity;
- residual circle fit;
- разброс расстояний boundary points до fitted center.

#### Open decision

Не выбран окончательный критерий circularity и порог принятия.

Выбор должен быть проверен на:

- реальных отверстиях;
- произвольных внутренних вырезах;
- raster noise;
- неполных или слабо выраженных отверстиях.

### Rough circle fitting

На rough stage нужна только приблизительная оценка:

```text
Cx
Cy
R
```

Precise fitting выполняется позднее по реальным точкам.

---

## 12. Группировка Rough Holes

### Purpose

Объединить близкие по rough-размеру отверстия в HoleGroup для организации, редактирования и определения областей выборки.

### Input

Набор rough holes.

### Output

```text
HoleGroup
    rough group size
    member holes
```

Положение центров остаётся индивидуальным. Принадлежность к HoleGroup сохраняется на precise-stage; rough group size не требует общего одинакового precise measured size (§32).

### Required

Группировка не должна использовать универсальный неаргументированный `epsilon`.

Критерий должен иметь физический смысл в миллиметрах.

### Алгоритмические вопросы

Необходимо определить:

- абсолютную составляющую допуска;
- при необходимости относительную составляющую;
- минимальный размер группы;
- первоначальный выбор rough group size;
- обработку неоднозначной принадлежности;
- поведение одиночного отверстия.

### Open decision

Точный clustering method и tolerance model требуют проверки на реальных данных.

---

## 13. Поиск Rough Cutouts

### Purpose

Найти внутренние пустые области, которые не являются отверстиями и достаточно велики, чтобы считаться реальными вырезами.

### Input

- density PNG / mask;
- текущий rough outer contour;
- текущие rough holes и сведения об исключённых rough-областях;
- `cell`;
- minimum significant size parameters.

### Output

Набор замкнутых rough cutout contours.

### Required

Область должна:

- находиться внутри outer contour;
- не быть уже классифицированным отверстием или исключённой областью (§11);
- превышать минимальный значимый размер.

Одиночные и очень маленькие пустые regions могут классифицироваться как noise. Удалённое или отфильтрованное отверстие не становится cutout автоматически; явное создание/переклассификация пользователем разрешено.

### Minimum significant size

Критерий должен быть связан с физическим масштабом:

```text
мм
мм²
или величина, однозначно производная от cell
```

Нельзя использовать необоснованное постоянное число raster pixels.

### Open decision

Не определены окончательно:

- shape/area criterion;
- правила morphology;
- минимальный физический размер;
- обработка сложных или частично разорванных областей.

---

## 14. Reduced Point Cloud Extraction

### Purpose

Получить рабочий набор реальных измеренных точек около текущей активной rough geometry. Явный Build Reduced Point Cloud принимает этот rough-state для перехода на precise-stage, без отдельного Confirmed Rough Geometry и без присвоения Fixed.

### Input

- persistent point cache;
- spatial access API;
- rough outer contour;
- rough cutouts;
- активные rough holes и сведения об исключённых областях;
- максимальное расстояние выборки в миллиметрах.

Проверяются допустимость конечного параметра расстояния, представимость размеров, overflow и ресурсы. При невозможности операции частичный результат не публикуется.

### Output

Reduced Point Cloud.

### Required

Отбираются точки только около геометрии, необходимой precise stage. Исключённые rough-области не используются для выборки (§11).

Point cache остаётся неизменным.

Одна исходная точка не должна дублироваться в reduced cloud при попадании одновременно в несколько selection regions.

### Общий критерий

Для контуров:

```text
distance(point, rough geometry) <= maxDistance
```

maxDistance означает максимальное расстояние от rough geometry до исходной точки, а не полную ширину полосы. Это обозначение в документе не фиксирует C++ API.

Для rough circle используется расстояние до самой окружности, а не до центра.

### Polyline distance

Для rough polyline расстояние определяется до ближайшего сегмента с корректной обработкой вершин.

### Spatial access

Сначала spatial index или эквивалентный механизм должен ограничить объём чтения кандидатными областями.

После этого выполняется точная геометрическая проверка расстояния.

Не следует читать весь многогигабайтный cache, если index позволяет определить необходимые области.

### Open decision

Точный persistent spatial index определяется отдельно архитектурой/cache design.

Алгоритм extraction должен зависеть от абстрактной возможности spatial range query, а не от конкретного disk layout.

---

## 15. Общая схема Precise Processing

Precise stage работает с реальными точками.

Концептуальный pipeline:

```text
Reduced Point Cloud
+
Rough Geometry
+
Fixed Geometry
+
User Tolerance
        ↓
Boundary/support extraction
        ↓
Segmentation
        ↓
Primitive candidates
        ↓
Robust fitting
        ↓
Primitive selection
        ↓
Topology / continuity construction
        ↓
Required geometry / one-sided validation
        ↓
Precise CAD Geometry
```

Порядок внутренних шагов может уточняться. Refinement включает выбор подходящих primitives и избегает заведомой избыточности, но не запускает отдельный дополнительный optimization pass. Geometry Optimization выполняется только явно по §35.

Обязательные свойства результата сохраняются независимо от конкретного метода.

Итоговые кандидаты первой версии ограничены LINE (`LineSegment`), ARC (`CircularArc`) и CIRCLE (`Circle`). Это распространяется на fitting, ручное редактирование, Geometry Optimization, topology и validation. Контур — замкнутая последовательность LINE/ARC либо самостоятельный CIRCLE. SPLINE/BSpline и ELLIPSE не поддерживаются; расширение требует отдельного изменения требований. Если этим набором невозможно удовлетворить обязательные ограничения, возвращается failure, а не другой тип кривой.

---

## 16. Supporting Boundary Points

### Purpose

Выделить из reduced point cloud точки, которые действительно описывают геометрическую границу.

### Problem

Reduced cloud содержит точки в полосе около rough geometry и может включать:

- собственно boundary points;
- точки поверхности внутри материала;
- noise;
- посторонние точки.

Использовать все точки полосы напрямую для fitting нельзя без проверки.

Облако получается линейным сканированием; наложение соседних линий может создавать небольшие выступающие участки measured boundary. Эти точки сами по себе не являются noise и рассматриваются как потенциальная граница детали. Tolerance позволяет заменить дискретную границу компактными primitives, но не удалить материал ради более простого fit.

### Required

Boundary extraction должен:

- учитывать rough geometry как начальное приближение;
- работать локально;
- не смещать границу внутрь материала из-за большого количества внутренних точек;
- сохранять реальные углы;
- работать на внешних и внутренних контурах.

### Open decision

Не выбран окончательно метод boundary extraction.

Кандидаты должны исследоваться на реальных сканах.

Возможные классы подходов могут включать:

- local neighbourhood analysis;
- projection relative to rough contour;
- nearest/extreme points по локальной нормали;
- другие методы.

Ни один из них пока не является обязательным.

---

## 17. Segmentation Precise Contour

### Purpose

Разделить последовательность boundary data на участки, которые разумно представить отдельными CAD-примитивами.

Типы сегментов цепочки первой версии:

```text
LineSegment
CircularArc
```

Circle представляет самостоятельный замкнутый контур, внешний или внутренний; он не является сегментом цепочки.

### Required

Segmentation должна:

- сохранять реальные углы;
- позволять касательные переходы;
- учитывать Fixed Geometry;
- способствовать минимизации разумного числа примитивов;
- работать в пределах пользовательского tolerance.

### Признаки возможной границы сегмента

Могут учитываться:

- резкое изменение направления;
- изменение локальной кривизны;
- ухудшение fit текущего primitive;
- геометрический corner;
- граница Fixed object;
- изменение типа подходящего primitive.

### Open decision

Конкретный segmentation method не выбран.

Требуются эксперименты с реальными контурами.

---

## 18. Junction Classification

Финальная geometry различает:

```text
Corner
Tangent
```

### Corner

Направление соседних касательных может различаться.

Алгоритм не должен сглаживать такой junction автоматически.

### Tangent

Соседние элементы должны иметь согласованные направления касательных в общей точке.

### Fixed geometry

Если junction type задан пользователем или связан с Fixed object, автоматический алгоритм обязан его соблюдать.

### Open decision

Не определено, насколько активно программа должна автоматически классифицировать неизвестные junctions как `Corner` или `Tangent`.

Если используется автоматическая классификация, критерий должен быть основан на локальной геометрии и проверен экспериментально.

---

## 19. Line Fitting

### Input

- supporting points участка;
- endpoints/junction constraints;
- Fixed neighbours;
- user tolerance;
- material side information.

### Output

`LineSegment` candidate.

### Required

Candidate должен:

- удовлетворять fitting quality;
- соблюдать required endpoints;
- учитывать Corner/Tangent junctions;
- не нарушать Fixed geometry;
- проходить one-sided tolerance validation.

### Open decision

Конкретный robust line fitting method пока не выбран и требует проверки на representative real scans.

Обычный unconstrained least-squares fit не является достаточным критерием принятия, так как геометрически хороший симметричный fit может оказаться на запрещённой стороне материала.

---

## 20. Circle Fitting

### Input

- supporting points предполагаемой окружности;
- rough center/radius;
- tolerance;
- fixed state.

### Output

```text
center
radius
fit metrics
```

### Required

- full circle сохраняется как `Circle`;
- fixed circle не изменяется;
- one-sided material constraint обязателен.

Для отверстия material constraint проверяется по всей итоговой окружности и удаляемой области с фактическими center/radius. Уменьшение radius не является достаточным доказательством, если меняется center.

### Open decision

Не выбран окончательный:

- initial estimator;
- robust estimator;
- refinement method.

---

## 21. Circular Arc Fitting

### Input

Boundary points участка.

### Output

```text
center
radius
start angle
end angle
```

### Required

CircularArc принимается только при соблюдении качества и всех обязательных ограничений. Если участок не удаётся представить LINE/ARC в пределах допуска, другой тип кривой не вводится автоматически.

Необходимо учитывать:

- orientation;
- endpoints;
- Tangent constraints;
- one-sided tolerance;
- соседние primitives.

### Open decision

Критерий достаточной angular extent и устойчивости arc fit требует проверки.

---

## 22. Границы геометрической модели

### Required

Первая версия строит только LINE/ARC/CIRCLE по [requirements.md, §24](requirements.md#24-точная-геометрия). Неподдерживаемые SPLINE/BSpline и ELLIPSE не являются fallback при неудачном fitting.

Сложный участок может быть разделён на несколько LINE/ARC, только если результат сохраняет tolerance, one-sided constraint, Fixed Geometry, C0, Corner/Tangent semantics и допустимую topology. Это fitting measured data, а не разрешение автоматически аппроксимировать неподдерживаемые входные CAD entities.

Если обязательные ограничения невозможно соблюсти, результат — CannotSatisfyTolerance или иной соответствующий failure (§§31, 37–38). Расширение набора primitives требует отдельного изменения требований.

---

## 23. Robust Estimation

### Purpose

Снизить влияние ошибочных и посторонних точек без уничтожения реальной геометрии.

### Required

Алгоритм должен различать по смыслу:

```text
measurement noise
outlier
real corner / real feature
```

Резкое изменение геометрии и выступ отдельной линии сканирования сами по себе не являются выбросами. Если прилегающий мусор нельзя надёжно автоматически отличить от реальных измеренных точек, он не исключается только ради удобного fit; его удаление остаётся пользователю precise-stage.

Ручно удалённые точки отсутствуют в текущем reduced dataset и после сохранения/открытия проекта не возвращаются. Явное полное перестроение cloud создаёт новый набор без обязательного переноса старых удалений; межэтапный reset также удаляет эти правки. Механизм хранения определяется архитектурой, не fitting.

### Open decision

Конкретный robust estimator не выбран.

Возможные подходы должны сравниваться по реальным данным.

---

## 24. Fitting Quality Metrics

Для fitted primitive должны рассчитываться диагностические показатели качества.

Следует предусмотреть как минимум возможность оценить:

```text
maximum deviation
RMS deviation
percentile deviation
supporting point count
one-sided violation
```

### Acceptance vs diagnostics

Не все метрики обязаны участвовать непосредственно в acceptance criterion.

Необходимо различать:

```text
diagnostic metric
```

и:

```text
required acceptance condition
```

Например низкий RMS сам по себе не доказывает отсутствие локального превышения пользовательского tolerance.

---

## 25. Primitive Selection

### Purpose

Выбрать наиболее простое разумное CAD-представление участка среди допустимых кандидатов.

### Required priority

Выбор ограничен LINE/ARC/CIRCLE. Если допустимые кандидаты одинаково удовлетворяют обязательным ограничениям, предпочтение отдаётся компактному устойчивому представлению: прямолинейный участок — LINE, круговой — ARC, полная окружность — самостоятельный CIRCLE. Соседние совместимые LINE/ARC могут объединяться; допустимость проверяется для всего результата.

Однако простота не имеет приоритета над:

- tolerance;
- one-sided constraint;
- topology;
- Fixed Geometry.

### Selection factors

Могут учитываться:

- fitting error;
- maximum deviation;
- primitive complexity;
- number of resulting objects;
- continuity with neighbours;
- stability of fit.

### Open decision

Точная objective function или heuristic пока не определена.

Не требуется решать доказанно глобальную задачу абсолютного минимума.

---

## 26. Fixed Geometry Constraints

Fixed Geometry относится к precise stage, а не к подтверждению rough-state. Пользователь может менять разрешение автоматических изменений: `Automatic → Fixed → Automatic`; снятие Fixed не запускает fitting.

### Required

Автоматический алгоритм:

- не изменяет Fixed object;
- не перемещает его endpoints;
- не изменяет его форму;
- учитывает связанные junction types;
- строит свободную геометрию относительно него.

Пример:

```text
Fixed Line
    +
Free boundary
        ↓
fit adjacent free primitive
while preserving Fixed Line
```

### Conflict

Если одновременно невозможно удовлетворить:

- Fixed Geometry;
- topology;
- Tangent constraint;
- one-sided tolerance;
- user tolerance,

Fixed Geometry не изменяется автоматически.

Операция должна вернуть невозможность корректного автоматического построения данного участка.

---

## 27. Контурная непрерывность

### C0 continuity

Любые два соседних элемента контура должны иметь одну общую конечную точку.

Не должно существовать двух независимых endpoints, которые только находятся очень близко друг к другу.

### Corner

Для `Corner` требуется C0.

Направления касательных могут различаться.

### Tangent

Для `Tangent` требуется:

```text
C0
+
совпадение направления касательных
```

Геометрическая непрерывность должна существовать в domain geometry, а не только визуально.

### Closure

Замкнутый контур должен иметь корректное соединение последнего и первого primitive.

---

## 28. Представление стороны материала

Односторонний допуск требует знать, с какой стороны boundary находится материал.

### Outer contour

Material находится внутри замкнутого outer contour.

Безопасное автоматическое отклонение направлено наружу.

### Hole / inner cutout

Material находится снаружи соответствующего внутреннего контура, но внутри детали.

Безопасное автоматическое отклонение уменьшает удаляемую область.

### Required

Для каждого precise contour должна быть однозначно известна orientation/material side.

### Open decision

Необходимо окончательно зафиксировать orientation convention замкнутых контуров, например через winding direction или отдельный semantic flag.

Алгоритмы не должны полагаться на случайный порядок точек без формального правила.

---

## 29. Односторонний допуск

### Purpose

Исключить автоматическое построение геометрии, которое удалит больше материала, чем подтверждают измеренные данные.

### Outer contour

Разрешено отклонение только наружу от материала.

Не допускается автоматический заход внутрь material region.

### Hole / cutout

Разрешено только уменьшение автоматически удаляемой области.

Автоматический контур не должен расширять отверстие или вырез в материал.

### Required validation

Каждый автоматически полученный precise candidate должен пройти:

```text
Geometric fit validation
+
One-sided material validation
```

До этого он не считается корректным результатом.

### Deviation metric

Необходимо определить signed deviation:

```text
positive = safe/material-preserving side
negative = forbidden/material-removing side
```

Orientation и тип контура учитываются при вычислении signed deviation; физический смысл безопасной и запрещённой стороны сохраняется. Конкретная метрика остаётся Open decision.

### Open decision

Не выбран способ технической проверки one-sided constraint относительно дискретного measured cloud. Физический контракт уже определён в [requirements.md, §26](requirements.md#26-точность-и-направление-допустимого-отклонения); выступы измеренных линий не объявляются noise автоматически.

Нужно определить:

- какие points считаются reference boundary;
- как интерполируется граница между измеренными точками;
- как проверяется curve между supporting samples;
- какая sampling density используется для validation.

---

## 30. Constrained Correction

После первоначального symmetric fit может потребоваться корректировка на безопасную сторону.

Допустимы два класса подходов:

```text
constrained fitting сразу
```

или:

```text
unconstrained fitting
→ material-side correction
→ validation
```

### Required

Независимо от внутреннего метода окончательный результат обязан удовлетворять one-sided tolerance.

### Open decision

Какой из подходов будет основным, определяется экспериментами для каждого типа primitive.

---

## 31. Failure: CannotSatisfyTolerance

Если невозможно одновременно обеспечить:

- user tolerance;
- one-sided material constraint;
- Fixed Geometry;
- required continuity,

алгоритм не должен возвращать нарушающую геометрию как успешную.

Результат должен иметь смысл:

```text
CannotSatisfyTolerance
```

или эквивалентный структурированный failure.

Пользователь после этого может вручную исправить геометрию или изменить параметры.

---

## 32. Precise Hole Refinement

На precise-stage используются текущие активные rough holes, принятые запуском Build Reduced Point Cloud. Глобальный поиск новых отверстий и восстановление исключённых областей не выполняются.

### Input

- rough hole;
- corresponding reduced points;
- HoleGroup membership и rough group size как начальная информация;
- user tolerance;
- Fixed state.

### Output

Refined Circle с индивидуальным measured center/radius и сохранённой принадлежностью к HoleGroup.

### Required

Center и radius/diameter каждого свободного отверстия уточняются по измеренным точкам. Rough group size не является жёстким общим диаметром precise holes; в DXF экспортируется индивидуальный precise measured size.

One-sided constraint проверяется по всей итоговой окружности с её фактическими center/radius. Уменьшение radius само по себе недостаточно при смещении center.

Fixed hole не изменяется автоматически. HoleGroup остаётся логической сущностью precise-stage.

### Open decision

Конкретный circle fitting, использование rough-приближения и geometric validation method требуют экспериментов. Принадлежность к группе не разрешает нарушать Fixed или one-sided tolerance.

---

## 33. Precise Cutout Refinement

### Input

- текущий rough cutout, принятый запуском Build Reduced Point Cloud;
- corresponding reduced boundary points;
- tolerance;
- fixed geometry.

### Output

Замкнутый precise CAD contour.

### Processing

Применяются те же общие этапы:

```text
boundary extraction
segmentation
primitive fitting
primitive selection
continuity
required topology/continuity и one-sided validation
```

Отдельный optimization pass в refinement не входит (§35).

### Required

- cutout остаётся замкнутой последовательностью LINE/ARC либо самостоятельным CIRCLE;
- автоматически удаляемая область не увеличивается в material region;
- Fixed Geometry не изменяется.

---

## 34. Precise Outer Contour Refinement

### Input

- rough outer contour;
- corresponding reduced points;
- fixed elements;
- user tolerance.

### Output

Замкнутая последовательность LINE/ARC либо самостоятельный CIRCLE.

### Required

- используются только `LineSegment`/`CircularArc` в цепочке либо самостоятельный `Circle`;
- контур замкнут;
- C0 всегда соблюдается;
- Tangent junctions удовлетворяют tangent constraint;
- Corner не сглаживается;
- Fixed Geometry сохраняется;
- automatic geometry не заходит в material region;
- количество объектов минимизируется разумно, но не ценой нарушения качества.

---

## 35. Geometry Optimization

Geometry Optimization является отдельной явно запускаемой пользователем операцией над уже корректной precise geometry. Refinement не запускает её неявно (§15).

### Purpose

Снизить сложность уже корректной precise geometry.

### Возможные преобразования

```text
Line + Line → Line
Arc + Arc → Arc
несколько простых совместимых primitives → один primitive
замкнутая цепочка совместимых Arc одной окружности → Circle
удаление избыточного короткого элемента
```

### Required

Каждое преобразование должно сохранять:

- user tolerance;
- one-sided tolerance;
- Fixed Geometry;
- C0;
- Tangent constraints;
- Corner semantics;
- topology;
- замкнутость.

После отдельной optimization результат повторно проходит необходимые geometry, topology/continuity и one-sided validation. Если обязательное свойство нарушается, преобразование не принимается.

### Fixed Geometry

Fixed primitive не заменяется и не объединяется способом, изменяющим его геометрию без явного действия пользователя.

### Open decision

Не определены окончательно:

- порядок simplification passes;
- objective function;
- минимальная практическая длина primitive;
- критерий объединения соседних элементов.

---

## 36. Проверка топологии

После automatic refinement или отдельной Geometry Optimization выполняется topology validation по [requirements.md, §27](requirements.md#27-непрерывность-контуров).

### Required

Проверяется допустимый набор LINE/ARC/CIRCLE. Для цепочки LINE/ARC проверяются порядок primitives, общие endpoints, closure, отсутствие разрывов и соблюдение Corner/Tangent junctions. CIRCLE является самостоятельным аналитически замкнутым контуром; его радиус должен быть положительным конечным числом.

Для первой версии допускаются один outer contour и `0..N` holes/cutouts внутри него. Самопересечения, взаимные пересечения и касания независимых контуров/границ, а также вложенные material islands запрещены.

Касающиеся области не считаются двумя независимыми границами. Допустимое представление — единая корректная форма, когда это возможно без нарушения остальных Required; иначе результат не принимается. Общие endpoints соседних primitives одного контура являются допустимыми C0-соединениями.

### Open decision

Geometric intersection/touch validation methods и численные tolerances проверки определяются реализацией и экспериментами. Поддерживаемая topology policy уже задана и не является Open decision.

---

## 37. Недостаток данных

Алгоритм не должен строить произвольную геометрию при недостаточных измерениях.

Возможные причины:

- слишком мало supporting points;
- большой разрыв в данных;
- слишком короткий observed segment;
- неоднозначный fit;
- сильный noise.

Должен существовать результат уровня:

```text
InsufficientData
```

а не искусственно уверенная geometry.

---

## 38. Неоднозначная геометрия

Если несколько существенно разных primitives удовлетворяют данным примерно одинаково и нельзя надёжно выбрать один, алгоритм может:

- использовать более простой вариант, если он удовлетворяет всем обязательным ограничениям;
- либо вернуть ambiguous result, если выбор существенно влияет на форму.

### Open decision

Необходимо определить практический порог, когда разница между competing models считается значимой.

---

## 39. Производительность

Алгоритмы проектируются для:

- слабого CPU;
- ограниченной RAM;
- HDD;
- отсутствия обязательной GPU.

### General rules

- исходный cloud не загружается целиком;
- большие datasets обрабатываются streaming/block-wise;
- spatial queries используются для ограничения объёма I/O;
- не создаются тяжёлые объекты на каждую исходную точку;
- временные большие buffers имеют ограниченное время жизни;
- complexity оценивается относительно фактического количества обрабатываемых points.

### Parallelism

Parallel processing допускается, но не является обязательным условием корректности.

Число threads и конкретный threading mechanism не определяются этим документом.

---

## 40. Cancellation

Длительный алгоритм должен иметь безопасные точки проверки cancellation request.

После cancellation частичный результат не считается корректным результатом операции.

Конкретный механизм передачи cancellation определяется архитектурой приложения.

---

## 41. Детерминированность

При одинаковых:

- input data;
- parameters;
- Fixed Geometry;
- algorithm version,

желательно получать одинаковый результат.

Если используется randomized algorithm, например probabilistic robust estimator, его randomness должна быть управляемой настолько, чтобы:

- тесты могли быть воспроизводимыми;
- расхождения можно было диагностировать.

Конкретная seed policy определяется реализацией.

---

## 42. Тестирование алгоритмов

Алгоритмические тесты должны использовать как минимум два класса данных.

### 42.1. Synthetic data

Геометрия с известным правильным ответом:

- line;
- rectangle;
- circle;
- arc;
- line-arc junction;
- Corner;
- Tangent;
- сложный замкнутый контур из LINE/ARC;
- самостоятельный внешний CIRCLE и внутренние CIRCLE;
- отказ при невозможности соблюсти обязательные ограничения набором LINE/ARC/CIRCLE.

Следует добавлять контролируемые:

- noise;
- outliers;
- пропуски points.

Преимущество synthetic tests — известная ground truth geometry.

Для Synthetic Point Cloud Generator каноническая geometry задаётся отдельным Geometry JSON, условия измерения — Scan Scenario JSON. Pipeline и контракт описаны в [спецификации генератора](tools/synthetic-cloud-generator.md). DXF/NX не являются входом генератора. Artifacts изменяют measurements, а не ground truth; CADContour2D получает только XYZ/ASC. Такие сценарии проверяют ROI, ручное редактирование и устойчивость алгоритмов, но не требуют автоматического defect detection.

### 42.2. Real scan data

Необходимы реальные облака со сканера.

Они нужны для выбора:

- raster thresholds;
- noise criteria;
- circularity thresholds;
- segmentation;
- robust estimator;
- grouping tolerance.

До проверки representative real scans допускаются synthetic tests, unit tests и experimental prototypes. Synthetic data сама по себе не подтверждает окончательный выбор rough/precise algorithm для реальных scans.

Большие scans не требуется помещать в репозиторий. Конкретные datasets для экспериментов могут храниться отдельно.

---

## 43. Критерии сравнения экспериментальных алгоритмов

При сравнении кандидатов следует оценивать:

```text
геометрическую ошибку
one-sided violations
количество CAD-primitives
стабильность результата
число ошибочных classifications
runtime
peak memory, если измерялась
I/O volume, если существенно
```

Не следует выбирать алгоритм только по runtime, если он хуже соблюдает геометрические требования.

Приоритет:

```text
1. корректность;
2. отсутствие опасного удаления материала;
3. стабильность;
4. компактность CAD geometry;
5. производительность.
```

---

## 44. Экспериментальные данные в документе

Этот документ хранит выбранные методы, ключевые основания, ограничения и физически значимые параметры, а не полный журнал экспериментов. Правила обновления и отчётности агента определены в [AGENTS.md, §19](../AGENTS.md#19-поддержка-документации).

---

## 45. Указатель основных Open decisions

Это краткий указатель основных локальных Open decision, а не исчерпывающий список всех деталей реализации.

- [Density encoding и raster mapping (§6)](#6-построение-density-map).
- [Raster foreground classification и filtering (§8)](#8-получение-рабочей-raster-mask).
- [Rough contour simplification (§9)](#9-поиск-rough-outer-contour).
- [Hole circularity и rough circle estimate (§11)](#11-поиск-rough-holes).
- [Hole grouping method и tolerance (§12)](#12-группировка-rough-holes).
- [Minimum cutout size и morphology (§13)](#13-поиск-rough-cutouts).
- [Boundary/support extraction (§16)](#16-supporting-boundary-points).
- [Precise segmentation (§17)](#17-segmentation-precise-contour).
- [Автоматическая junction classification (§18)](#18-junction-classification).
- [Line, circle и arc fitting (§§19–21)](#19-line-fitting).
- [Robust filtering/estimation (§23)](#23-robust-estimation).
- [Primitive selection (§25)](#25-primitive-selection).
- [Orientation convention (§28)](#28-представление-стороны-материала).
- [Reference boundary, интерполяция, validation metric и sampling (§29)](#29-односторонний-допуск).
- [Constrained fitting/correction (§30)](#30-constrained-correction).
- [Geometry Optimization passes и критерии (§35)](#35-geometry-optimization).
- [Intersection/touch validation и численные tolerances (§36)](#36-проверка-топологии).

Point-cache layout, spatial index, persistence и DXF compatibility остаются инженерными Open decisions в [architecture.md, §45](architecture.md#45-указатель-основных-open-decisions).

---

## 46. Решения, которые алгоритм не может менять самостоятельно

Локальная реализация алгоритма не должна:

- использовать исходный point cloud вместо PNG на rough stage;
- использовать PNG вместо real points на precise stage;
- автоматически изменять Fixed Geometry;
- нарушать one-sided tolerance ради лучшего symmetric fit;
- сглаживать Corner;
- игнорировать Tangent constraint;
- превращать весь precise contour в полилинию без необходимости;
- вводить итоговые primitives вне LINE/ARC/CIRCLE или использовать неподдерживаемую кривую как fallback;
- автоматически запускать следующую тяжёлую операцию;
- менять `cell` без полного перестроения density map;
- возвращать нарушающую обязательные ограничения geometry как `success`.

Изменение этих принципов требует пересмотра требований или архитектуры проекта.

---

## 47. Критерий готовности алгоритма

Алгоритмическая операция считается достаточно определённой для production implementation, когда для неё известны:

1. Input.
2. Output.
3. Preconditions.
4. Parameters.
5. Required guarantees.
6. Failure conditions.
7. Geometry validation.
8. Numerical tolerances.
9. Resource expectations.
10. Проверенный на representative data метод.

До определения и проверки конкретных методов и validation допускается экспериментальная prototype implementation с synthetic/unit tests. Required не ослабляются: известное нарушение не возвращается как success, а неподтверждённый метод не объявляется экспериментально подтверждённым только по synthetic data.

---

## 48. Основные алгоритмические принципы

1. Rough stage работает по фиксированной density PNG.
2. Precise stage работает по реальным точкам reduced cloud.
3. Rough geometry является приближением, а не точной CAD geometry.
4. Реальные точки отбираются около текущей активной rough geometry по явному Build Reduced Point Cloud; отдельного Confirmed state нет.
5. Precise contour — замкнутая цепочка LINE/ARC либо самостоятельный CIRCLE; огромная полилиния через все measured points не является целью fitting.
6. Более простой primitive предпочтительнее сложного только при соблюдении всех требований качества.
7. Fixed Geometry имеет приоритет над automatic fitting.
8. Все связанные primitives должны обеспечивать C0 continuity.
9. `Tangent` дополнительно требует согласования касательных.
10. `Corner` не должен автоматически сглаживаться.
11. Односторонний допуск является обязательным условием принятия automatic precise geometry.
12. Нарушающий material-preservation result не считается успешным.
13. Noise filtering не должен уничтожать реальные геометрические особенности.
14. Robust fitting не заменяет проверку one-sided tolerance.
15. Низкий RMS сам по себе не доказывает корректность geometry.
16. Алгоритм не должен скрывать неоднозначность или недостаток данных искусственно уверенным результатом.
17. Оптимизация числа CAD-объектов выполняется только после выполнения обязательных геометрических ограничений.
18. Производительность оптимизируется с учётом слабого CPU, ограниченной RAM и HDD, но не ценой нарушения геометрических требований.
