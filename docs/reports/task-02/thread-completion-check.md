# Подтверждение завершения потока на Windows

## Наблюдение на установленном Qt

В Qt 6.12.0 `msvc2022_64`, MSVC 19.51, Windows x64 воспроизведено расхождение с [документацией QThread::wait](https://doc.qt.io/qt-6/qthread.html#wait): после сигнала finished метод `wait(0)` возвращает true, пока C++ `thread_local` destructor этого же worker заблокирован на управляемом барьере. Проверка выполнена в Debug и Release. Это наблюдение об установленном kit, а не утверждение обо всех версиях Qt или ОС.

В `tests/operation_tests.cpp` сохранены отдельные проверки:

- `Qt612WaitZeroTlsDiscrepancyReproducer` фиксирует именно Qt 6.12.0 и записывает property `qt_wait_zero_while_tls_blocked=true`. При другом Qt измерение пропускается; если поведение этого kit исправится, тест потребует пересмотра основания guard.
- `NativeThreadCleanupKeepsSlotEvenAfterFinishedSignal` проверяет production runner: независимое ожидание Windows возвращает TIMEOUT; execution context остаётся жив, новый запуск отклоняется как Busy, shutdown остаётся Closing до освобождения TLS-барьера. После полного выхода worker наступает Ready.

Воспроизводимые команды из настроенной x64 Developer CMD, с Qt bin в PATH:

```bat
build\windows-debug\operation_tests.exe --gtest_filter=Operations.Qt612WaitZeroTlsDiscrepancyReproducer:Operations.NativeThreadCleanupKeepsSlotEvenAfterFinishedSignal --gtest_output=xml:build/audit-20261013/tls-debug.xml
build\windows-release\operation_tests.exe --gtest_filter=Operations.Qt612WaitZeroTlsDiscrepancyReproducer:Operations.NativeThreadCleanupKeepsSlotEvenAfterFinishedSignal --gtest_output=xml:build/audit-20261013/tls-release.xml
```

Результат: 2/2 PASS в каждой конфигурации; обе XML-записи содержат `qt_wait_zero_while_tls_blocked=true`. Первоначальная проверка ожидала false и упала в обеих конфигурациях, что послужило основанием для согласованного пользователем локального уточнения механизма.

## Независимое подтверждение

Worker получает настоящий независимый HANDLE через [DuplicateHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle) из GetCurrentThread; Qt продолжает владеть собственными ресурсами. RAII-владелец закрывает независимый handle. Runner освобождает worker только после `WAIT_OBJECT_0` от [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject) и успешного Qt join. TIMEOUT/FAILED не разрешают освобождение.

Отказ DuplicateHandle даёт structured resource failure с GetLastError, вычисление не вызывается. Для безопасного drain worker удерживается в handshake до получения независимого handle; owner может использовать [OpenThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openthread) для этого ещё живого потока. Ошибка системного наблюдения удерживает ресурсы и Busy, логируется с кодом. При постоянной недоступности системного наблюдения Closing/destructor могут продолжать ожидание; небезопасного принудительного освобождения нет.

В общей Debug/Release регрессии проверены также injection WAIT_FAILED и DuplicateHandle/OpenThread failure, восстановление fallback без GUI delivery, ожидание TLS в destructor и действительность/RAII-закрытие handle. Обычный cancellation и публикация не изменены: это дополнительное подтверждение физического завершения, без второго worker и без повторного запуска вычисления.
