# Аудит ядра и путь к NT

Дата: 29 сентября 2026. Загрузка по умолчанию — `cmd.exe`.

## Границы проверки

Удалены из рабочего дерева Windows-бинарники, экспериментальные Explorer/Calc,
упаковщик Windows DLL, IDA-скрипт и документация этого эксперимента. Убраны
связанные правила сборки и бинарно-зависимые патчи USER32/GDI32. Реализация
USER/GDI, сообщений и callbacks сохранена: это подсистема, а не проводник.
Остальные пользовательские изменения и переименования файлов сохранены.

Clang анализирует все C translation units в `kernel/` и `user/`, включая
подключённые `.inc`. Отдельно просмотрены границы подсистем и пути загрузки,
syscall/interrupt entry, переключения и завершения потоков. Это не доказательство
корректности каждой строки, всех ассемблерных путей или совместимости со всеми
Windows-программами. Для Clang отключён только `core.FixedAddressDereference`
в ядре: он не понимает намеренные отображения страниц по фиксированным адресам.
Отсутствие его предупреждений не подтверждает безопасность этих отображений.

## Исправлено

- Добавлены dispatcher timers: notification/synchronization, одноразовые и
  периодические, rearm/cancel, ordered deadline list и optional kernel DPC.
  Expiry выполняется в clock DPC до timeout/reschedule; пропущенные периоды
  объединяются без catch-up storm. Сигнал synchronization timer потребляется
  одним wait, notification остаётся до следующего set; cancel не сбрасывает
  сигнал. Caller-owned timer/DPC должны жить до завершения их queued work.
  Native anonymous Timer objects поддерживают create/set/cancel/query и waits,
  проверяют access masks; delete снимает активный timer до освобождения памяти.
  Query возвращает zeroed x64 basic info, signed remaining time и signal state.
  Native syscall IDs сверены с [Win7 SP1 x64 table](https://github.com/j00ru/windows-syscalls/blob/master/x64/csv/nt.csv),
  signatures/structure — с [phnt](https://github.com/winsiderss/phnt/blob/master/ntexapi.h).
  Добавлены kernel32 CreateWaitableTimerA/W, SetWaitableTimer, CancelWaitableTimer.
  NtDelayExecution/Sleep теперь блокируется на timer, а не крутит KeYield;
  zero delay только уступает CPU. Stack-owned timers снимаются при reaping
  потока до освобождения kernel stack. Conversion округляет вверх и обрабатывает
  INT64_MIN; wait deadline складывается без overflow, invalid timeout pointer
  больше не превращается в бесконечное ожидание.
  Ограничения: 10 ms resolution, absolute/shared time пока since-boot, не UTC
  NT epoch; names/security attributes/inheritance, wake/resume и timer APC
  не реализованы и отклоняются явно. Existing alertable waits/SleepEx и user
  timer-queue/USER timers ещё требуют отдельной работы. Host checks используют
  настоящий Ke/Ps/Ob/Syscall код с fake clock/context switch: one-shot/periodic,
  DPC, wait-all, blocking delay, closing during wait, 1000 armed-close cycles,
  query/access/error/OOM и stack-owner cleanup. QEMU smoke проверяет boot/shell/
  PS2, а не весь native timer ABI в ring 3. Guest test exe не добавлены.
- Добавлена resident UP FIFO-очередь DPC: duplicate insert не заменяет args,
  pending вызов можно удалить, callback может поставить себя повторно. Queue
  membership снимается до вызова; после callback объект не разыменовывается
  (проверено освобождением внутри routine под ASan). При lowering ниже DISPATCH
  выполняется до 64 callbacks, остаток обслуживается на следующем IRQ/lowering;
  постановка с PASSIVE не означает синхронный вызов. На работающей ОС очередь
  обслуживается, в частности, IRQ таймера 100 Hz. PIC по-прежнему маскирует IRQ
  на время DPC; нет software interrupt, threaded DPC, SMP или binary KDPC ABI.
  Таймерный ISR только считает ticks/quantum и ставит DPC: shared time и wait
  deadlines обрабатываются в DPC с актуальным tick count, до reschedule.
  Keyboard/mouse ISR сохраняют сырые bytes в bounded queues; decoding перенесён
  в DPC. Overflow сбрасывает decoder framing/modifier state, чтобы потерянный
  break code не оставлял Shift нажатым. Исправлены Caps+Shift и два Shift.
  Hardware ISR больше нельзя сигналить/reset event; DPC — можно.
  FIELD_OFFSET/CONTAINING_RECORD используют builtin offset вместо UB через
  NULL member access. Host checks теперь завершаются ошибкой при UBSan report.
  Проверены FIFO/cancel/requeue/budget/IF/lifetime и реальные input decoders;
  QEMU smoke дополнен командой, набранной через эмулируемую PS/2 клавиатуру.
- Добавлен UP execution contract: PASSIVE/APC/DISPATCH/HIGH IRQL, spin locks,
  восстановление исходного IF, проверки неверных raise/lower, recursive lock,
  release без acquire и блокирующего wait выше APC. Poll-only wait допускается
  до DISPATCH. Публикация потоков использует spin lock; прочие старые короткие
  sections пока остаются под CLI. Таймер запрашивает reschedule, а переключение
  выполняется после восстановления IRQL на выходе из IRQ; IRQL сохраняется
  отдельно для каждого suspended thread context. На PIC все IRQ выполняются
  на HIGH и DISPATCH+ блокирует все IRQ: это консервативная UP модель, не APIC
  priority threshold и не SMP. Полный executive IRQL contract ещё не реализован.
  Host checks проверяют настоящий Irql.c/Dispatcher.c с эмуляцией IF; QEMU —
  реальный boot, timer preemption и shell/process smoke, не все scheduler paths.
- USER callbacks больше не расходуют новый global stack/thunk на каждый вызов.
  Поток переиспользует private slot своего процесса: unmapped lower guard,
  RW/NX stack и RX thunk, обновляемый через direct map с сериализацией fetch.
  CREATESTRUCT хранится в том же slot вне callback home space. Reaping удаляет
  mappings через CR3 владельца и освобождает frames без смены current CR3.
  Частичный allocation/map failure откатывается; reserve-only regions и чужие
  mapped pages не перезаписываются. Сохранённый return slot восстанавливается
  и при нулевом значении; callback target проверяется на user access/NX.
  Ошибки dispatch при создании окна больше не маскируются успешным HWND.
  Проверены 1000 повторных callbacks без роста frames, creation chain/refusal,
  two-process isolation, concurrent slot exhaustion/reuse, termination in-flight
  и injected allocation/map failures. Старые bump-accessors loader удалены.
  Nested/reentrant callbacks и полноценный Windows user-callback protocol ещё
  не реализованы; CFG bootstrap arena отдельно остаётся legacy RWX, не CFG
  enforcement. QEMU smoke проверяет boot/shell, не настоящую GUI-программу.
- USER/GDI lifecycle: создание процесса больше не сбрасывает глобальные GDI
  objects. GDI lookup/delete/get-entry проверяют kernel-owned owner и generation;
  PID публикуется реальный, DC_ATTR берётся из kernel metadata и используется
  повторно. Selected GDI objects нельзя удалять до deselection/DC deletion.
  Process teardown освобождает bitmap storage и очищает kernel caches без
  обращения к чужим user VA. USER classes/cursor cache привязаны к процессу;
  окна и cursor/accelerator handles получают generation и ownership checks.
  Очереди индексируются по сохранённому TID (не по `tid - 1`), освобождаются
  вместе с окнами/DC при thread reaping. Input event имеет отдельную queue
  reference; conditional close не закрывает переиспользованный client handle.
  Shared cells очищаются через page tables владельца и direct map, без смены
  CR3 и без временного открытия user write permissions; проверено пересечение
  двух страниц и отказ без частичной записи при отсутствующей второй странице.
  Host-проверки охватывают two-process isolation, высокие TID, stale handles,
  selected bitmap lifetime, DC_ATTR reuse и cleanup под чужим current process.
  Это ещё не полноценный desktop/session manager: cross-thread SendMessage,
  cross-process GUI contracts, nested callbacks, window hierarchy
  и compositor остаются отдельной работой. Fixed table limits сохранены;
  slots с исчерпанным generation retire вместо оживления старых handles.
- Убраны return-0 заглушки отсутствующих imports. Обычные imports возвращают
  DLL/entrypoint/ordinal-not-found с диагностикой DLL и символа. Delay imports
  сохраняют MSVC-трамплины загрузки по первому вызову; проверяются границы
  таблиц, исходных трамплинов и module-handle slots без загрузки optional DLL.
  Проверяются границы import/export tables, strings и thunk arrays; ограничены
  циклические forwarders. Невыровненные export arrays читаются через `memcpy`.
  Неудачная runtime-загрузка откатывает новые модули, TLS registrations и
  loader context, освобождает image frames, не публикует DLL в PEB list.
  Исправлен partial image allocation/map/relocation cleanup. Host-тесты
  проверяют повторный отказ и успешную загрузку после исправления зависимости.
  Это не полный fault-safe loader и не реализация CFG enforcement.
- `MmProbeForRead` теперь проверяет user access; права страницы учитывают
  ограничения всех уровней page tables. NX больше не попадает в физический
  адрес 2-MiB страницы.
- VM allocation проверяет переполнение, диапазон user space, размер с учётом
  невыравненного начала и пересечение с отображёнными страницами вне таблицы
  регионов. `NtProtectVirtualMemory` проверяет выходной указатель до изменений;
  `NtDelayExecution` проверяет interval и не отрицает signed minimum.
- Исправлены утечки frames при неудачном отображении страницы процесса,
  создании адресного пространства и расширении pool. Проверено переполнение
  размера pool/object allocation. Это не полное устранение всех OOM-путей.
- Отвергаются нулевые и повторяющиеся объекты ожидания, чтобы wait-all не
  потреблял один сигнал дважды.
- Парсер Multiboot проверяет размеры tags, шаг memory-map entries, переполнение
  регионов и обязательный end tag вместо бесконечного прохода по плохим данным.
- Framebuffer pixel/glyph/rectangle операции ограничены экраном; арифметика
  прямоугольников не переполняется. PIT не получает нулевой divisor из-за
  слишком большой запрошенной частоты.
- ATA read failure не оставляет частично перезаписанный cache buffer под
  старым LBA. Проверены rollback и однократная write-fault injection.
- PE mapper проверяет файловые границы headers, sections и directories до
  отображения; relocation blocks и DIR64 targets проверяются перед записью.
  Это не полная валидация внутренних import/export/TLS/resource structures.
- Исправлены чтение неинициализированного хвоста диагностического имени,
  форматирование строки с завершающим `%` и неверный код
  `STATUS_NOT_IMPLEMENTED` в ntdll.
- В Win64 ntdll `SIZE_T` исправлен с 32 на 64 бита. Critical sections теперь
  используют один native 40-byte layout в ntdll/kernel32; фиктивные успешные
  MSVCRT enter/leave заменены forwarders. Проверены рекурсия, ownership и
  конкурентный счётчик четырёх host threads.
- `FreeSid` освобождает выделенную VM. Token APIs больше не выдают фиктивные
  handles и членство во всех группах: возвращают unsupported error.
- Повторные перенаправления shell заменяют и закрывают предыдущий handle,
  а не переполняют массив из трёх элементов. Ошибка открытия очищает уже
  открытые handles. Исправлена небезопасная арифметика file offset/mapping size.
- Make отслеживает смену debug/release. Boot helper использует private temporary
  directory и cleanup trap вместо `mktemp -u`.

## Что есть и чего не хватает по подсистемам

| Подсистема и код | Рабочая основа | Недоработки / следующий контракт |
| --- | --- | --- |
| Architecture: `arch/x86_64` | Long mode, GDT/TSS, IDT, syscall, FXSAVE, user exception dispatch | Audit SYSRET canonical return и SWAPGS/NMI paths; guard pages kernel stacks; полноценные XSAVE/CPU feature negotiation |
| HAL: `hal` | Legacy PIC/PIT, COM1, PS/2, framebuffer | ACPI, APIC/IOAPIC, SMP, PCI enumeration, power/shutdown, реальные hardware error/timeout results |
| Ke: `ke/Thread.c`, `Dispatcher.c`, `Irql.c`, `Dpc.c`, `Timer.c` | UP scheduler, basic IRQL/spin locks, FIFO DPC, deferred input/timeouts, dispatcher timers, IRQ-tail preemption, events, semaphores, mutants, waits/timeouts | APC queues, alertable waits, UTC clock/high-resolution/coalescable timers, full executive IRQL contract; APIC/SMP locking. Большинство коротких sections ещё под CLI |
| Mm: `mm`, VM services в `ke/Syscall.c` | PMM, page tables, per-process CR3, eager reserve/commit/decommit/protect/query | VAD для всех mappings, page-granular protection/state, atomic rollback failed commit, demand faults, guard one-shot semantics, PFN refs, Sections, COW, working sets, pagefile. WRITECOPY пока writable, не COW |
| Ex: `ex/Pool.c` | Tagged pool, guard bytes, coalescing | Полный synchronization contract, paged pool, quota/accounting, безопасная обработка повреждений; нет NT executive worker/related infrastructure |
| Ob: `ob` | Types, lifetime, directory namespace, local handles/access masks | Generic access mapping по типам, security descriptors, symbolic links, handle attributes/inheritance/protection и полная namespace locking |
| Ps: `ps` | Process isolation, handles/exit/wait, separate TEB/TLS, process teardown | Thread stack reserve/commit/guard parameters, reclamation отдельных user stacks/TEB/TLS, lifecycle notifications, suspend/resume/context rights. Main-thread exit сейчас тесно связан с process exit |
| Io/VFS: `io/File.c`, `Fat.c`, `Ata.c`, `BlockCache.c` | Vnodes, offsets/share modes, dirs, FAT LFN, sector cache/rollback | DRIVER_OBJECT/DEVICE_OBJECT/IRP, device stacks, completion/cancel, async I/O, MDLs, mount/backend dispatch. Нынешний vnode слой прямо вызывает FAT и не является полным generic VFS |
| Cm: `cm/Registry.c` | In-memory keys/values и native services | Persistent hives, enumeration/delete/notify, security, concurrency; reboot теряет изменения registry |
| Loader: `ldr` | PE32+, strict imports/forwarders/rebase, rollback failed runtime load, process module lists, static TLS, bootstrap | Fault-safe validation всех вложенных structures; API-set schema вместо семейных alias heuristics, SxS, unload/refcount, thread attach/detach notifications; CFG enforcement |
| Rtl/exception: `rtl`, `user/ntdll/Rtl` | Memory/string/formatting, heap, x64 exception/unwind surface | Полная unwind conformance, chained/prolog/epilog edge cases, exception reentrancy, C++ object lifetime; проверять независимыми ABI/SEH fixtures |
| Win32k: `win32k` | Owner/generation checks, process-local classes/cursor cache, reusable thread queues and teardown, reusable guarded callback stacks/RX thunks and cleanup, window records, DCs/bitmaps/brushes/text/blits | Full cross-thread/process GUI contracts, nested/reentrant callbacks and Windows dispatcher protocol, window hierarchy/notifications, session/desktop ownership, hit-test/z-order/clipping/composition. Fixed capacity tables; font bitmap-only, layout tied to one USER ABI |
| Win32 runtime: `user/kernel32`, `advapi32`, `msvcrt` | Own DLLs поверх native API | File mappings — private user array, не Section objects; FLS values process-global, callbacks/cleanup неполны; registry/security/COM/timers частичны; audit remaining success-shaped shims |
| Shell/console: `user/cmd`, `cmdutils`, `io/Console.c` | External commands, history/editing, vars/batch, pipes/redirection | Full cmd grammar, robust oversized-input/EOF/error behavior, multiple consoles/resize. Pipeline launch smoke не доказывает передачу больших объёмов данных через pipe |
| Security и IPC | Se/port подсистем нет; Ob хранит granted masks | Tokens, SID/ACL access checks, privileges/impersonation, object security; LPC/ALPC ports и subsystem coordination |

## Чем это отличается от полноценного NT

Наличие каталогов Ke/Mm/Ob/Ps/Io и нескольких `Nt*` функций ещё не означает
совместимое NT executive. Microsoft описывает executive через сервисы объектов,
памяти, процессов, I/O, configuration и security; часть этих контрактов здесь
пока отсутствует. [Kernel-mode architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/).

Для Windows-драйверов нужны реальные driver/device objects и dispatch entry
points, связанные с IRP major functions, а не только прямой вызов ATA/FAT.
[Driver objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/introduction-to-driver-objects).
IRQL определяет допустимые операции и контекст driver routines; ISR должна
передавать отложенную работу DPC. Отключение прерываний на одной CPU не заменяет
этот контракт. [Hardware priorities](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/managing-hardware-priorities),
[ISR/DPC](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/writing-an-isr).

Оригинальные GUI-приложения требуют ещё пользовательских Win32 DLL, USER/GDI,
COM и shell/runtime contracts: это отдельная цель, не критерий готовности ядра.
Windows kernel-driver binary compatibility также пока не реализована.

## Приоритеты следующего цикла

1. **P0: граница user/kernel и lifecycle.** Fault-safe copy/probe, проверка всех
   указателей/lengths/handles, SYSRET safety, cleanup after fault/termination/OOM,
   GUI lifetime/isolation. Неподдерживаемое должно явно завершаться ошибкой.
2. **P1: Ke execution model.** Развивать UP IRQL/DPC/timer contract, затем APC,
   alertable waits. Это фундамент I/O, не работа над новой файловой системой.
3. **P1: I/O manager.** Driver/device objects, IRP stack locations, dispatch,
   completion/cancel, async read/write/event. ATA/FAT остаются существующими
   backend, меняется контракт вокруг них.
4. **P1: Mm Sections.** Единая карта всех regions, page-granular state, PFN
   lifetime, anonymous/file Sections и mappings; затем demand paging/COW.
5. **P2: Se и IPC.** Tokens/access checks, security descriptors, ports;
   persistent Cm hives. После стабилизации — user-mode system processes/services.
6. **P2: совместимость приложений.** Зафиксированный native ABI, fixtures
   PE/TLS/SEH, строгая import диагностика и расширение Win32 без byte patches.

Предлагаемый следующий большой этап — **APC и alertable waits**, после P0 hardening.
Писать новый desktop до исправления process ownership и lifecycle в USER/GDI
сейчас снова построило бы оболочку поверх нестабильных контрактов.

## Воспроизводимая проверка

`make check` — sanitizer host checks (paging, Multiboot, graphics, block cache,
formatter, UP IRQL/spin locks/wait limits/DPC/input decoders/timers/native timer handles/blocking delay, PE bounds/import binding/rollback, console blink, USER/GDI ownership/lifetime and callback reuse/cleanup, native critical section); `make audit` — all C units.
`make test-release` — clean optimized build + host checks + QEMU smoke.
`python3 tools/QemuSmoke.py --repeat 3` — повторные boots на отдельных копиях
диска, ввод через PS/2, проверка shell markers и exit codes, внешних PE, нескольких redirects,
create/read/copy/delete files и mkdir/cd/rmdir. Не тестирует настоящий power-cut,
reboot persistence, весь wait/SEH ABI или Windows applications. На диске ОС
нет тестовых exe. Логи — `build/checks/`.
