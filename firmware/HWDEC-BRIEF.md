# HWDEC: что именно нужно выяснить (бриф для внешней консультации)

Файл для пересылки: здесь нет ничего лишнего — только факты, точная точка блокировки и
конкретные вопросы, ответы на которые разблокируют работу.

## 1. Цель

Заставить **свой плеер** на XREAL One Pro декодировать H.264/H.265 **аппаратно** (VDEC/VPU),
а не программно через ffmpeg (сейчас 720p@60 тормозит). Плеер пишем сами, встраиваем в прошивку.

## 2. Платформа (проверено на устройстве)

* SoC: **Artosyn AR9481**, SDK `ar9481-ar-sdk-0.17.45-18-mp`, chipId `0x3536f93`, Linux, glibc 2.25.
* MPP-библиотеки в `/usr/lib`: `libmpi_sys.so`, `libmpi_vdec.so`, `libmpi_vb.so`, `libmpi_vpss.so`,
  `libmpi_vo.so`, `libmpi_scaler.so`, `libmpi_adec.so`, `libmpi_ao.so`, `libhal_vdec.so`,
  `libhal_vb.so`, `libhal_sys.so`, `libmpp_service.so`, `librpc_proxy.so`. У всех **пустой DT_SONAME**.
* API в стиле HiSilicon MPP: `AR_MPI_VDEC_CreateChn/SendStream/GetFrame/ReleaseFrame/AttachVbPool`,
  `AR_MPI_VB_*`. VDEC-вызовы идут через give-cmd ioctl `_IOWR('A',15,24)` = `0xc018410f` на `/dev/vdecN`.
* В системе работают три компонента (runit, каталоги `/etc/services/*`):
  * `mpp_service.app` — демон MPP (pid стабильный после бута);
  * `media_service` — вендорный медиапайплайн (камера/дисплей), он же первым инициализирует VDEC;
  * `pilot` — UI/рендер, ходит в кодеки через `libxr_codec.so` → `libxrlinux_codec.so` (dlsym `XrlinuxCodec*`).
* Firmware кодека вшита в `libmpp_service.so` (`ar_incbin_get_codec_firmware`), внешних .bin не требуется.

## 3. Что уже проверено (наши вызовы из **своего** процесса, aarch64, freestanding)

| Вызов | Результат |
|---|---|
| `AR_MPI_SYS_Init()` | `0` |
| `AR_MPI_SYS_GetVersion/GetChipId` (без Init) | ok: `ar9481-ar-sdk-0.17.45-18-mp`, `0x3536f93` |
| `AR_MPI_VDEC_Init(40)` | `0` |
| `AR_MPI_VB_CreatePool({blk=1 382 400, cnt=12})` | возвращает валидный id (10, 11, 12) |
| `AR_MPI_VB_SetConfig(&cfg648)` | `-1` (ioctl `_IOW('b',0,648)` = `0x42886200`) |
| `AR_MPI_VB_Init()` | `0` |
| `AR_MPI_VB_SetModPoolConfig(uid=3, &cfg)` | `-1` на «живом» демоне, `0` на только что загруженном |
| `AR_MPI_VB_InitModCommPool(uid=3)` | `0` (ioctl `_IOW('b',14,4)` = `0x4004620e`) |
| **`AR_MPI_VDEC_CreateChn(0..39, &attr)`** | **блокируется навсегда** (см. п. 4) |
| `AR_MPI_VDEC_AttachVbPool` **до** `CreateChn` | блокируется навсегда, демон после этого заклинен до перезагрузки |

Раскладка `VDEC_CHN_ATTR` восстановлена по дизассемблеру `AR_MPI_VDEC_CreateChn` и **отличается
от публичного реверса под Proxima-9311 на −4 байта** (на AR9481 нет поля `enVBSource`):

```
+0  u32 enType        96=H.264, 265=H.265, 26=JPEG, 1002=MJPEG
+4  u32 enMode
+8  u32 u32PicWidth   (максимальная, cap под буферы)
+12 u32 u32PicHeight
+16 u32 u32StreamBufSize   ← должен быть > 0x8000, иначе ошибка 0x80058403
+20 u32 u32FrameBufSize
+24 u32 u32FrameBufCnt
+28 u32 u32RefFrameNum
+32 u32 bTemporalMvpEnable
+36 u32 u32TmvBufSize
+40 u32 u32CmdQueueDepth
```
Всего структура 88 байт. При такой раскладке ошибка `0x80058403` **уходит** — то есть параметры
канала проходят валидацию, и дальше вызов уходит в ожидание.

## 4. Точка блокировки (главное)

`AR_MPI_VDEC_CreateChn` не возвращает управление никогда. Диагностика:

* `/proc/<pid>/wchan` = **`request_wait_answer`** → ожидание ответа **binder**-транзакции;
  `/proc/<pid>/syscall` = `ioctl(11, 0xc018410f, ...)`, fd 11 = `/dev/binder`.
* Демон `mpp_service.app`, запущенный вручную с перехватом stdout/stderr, за время нашего запроса
  **не печатает ни одной строки про VDEC** (в логе 12 000 строк — только аудио `AIO_HAL` и `GDC`);
  то есть запрос либо не доходит до кодека, либо обслуживается не тем процессом.
* Если `media_service` остановлен, `CreateChn` тоже висит; если он убит в момент наших проб,
  сервис остаётся зомби, а после перезагрузки всё оживает.
* **Чистый контрольный опыт:** тот же прогон без единого VB-вызова (`pd5`: только `SYS_Init` →
  `VDEC_Init(40)` → `CreateChn`), при **полностью живом** вендорном стеке (`mpp_service.app`,
  `media_service`, `pilot` запущены) — `CreateChn` всё равно висит. При этом стек после прогона
  остаётся рабочим, то есть дело не в VB и не в отсутствии сервера, а именно в отказе обслужить
  создание канала для стороннего процесса.

Рабочая гипотеза (не подтверждена): обслуживает модуль VDEC не демон, а **тот процесс, который первым
вызвал `VDEC_Init`** (у нас — `media_service`); тогда второй процесс либо не может получить канал,
либо должен как-то зарегистрироваться у него. Альтернатива: нашему процессу не хватает шага
инициализации, который делает `media_service` при старте.

## 5. Конкретные вопросы (на них нужен ответ)

1. В режиме, когда `mpp_service.app` запущен (не `mpp_service=lib`), что обязан сделать **сторонний
   процесс-клиент** до `AR_MPI_VDEC_CreateChn`, чтобы вызов вернулся? Есть ли регистрация/хендшейк:
   `ar_set_mpp_service_master_app`, поиск binder-сервиса по имени, `AR_MPI_SYS_Init` с параметрами?
2. Кто является RPC-сервером модуля VDEC: демон или первый клиент (`media_service`)? Может ли второй
   процесс создать **свой** канал, если сервер — первый клиент?
3. Обязательно ли до `CreateChn` создавать VB-пулы (`VB_SetConfig` → `VB_Init` → `InitModCommPool`)
   и какой `VB_UID` соответствует VDEC **на AR9481**? Вендорная обёртка `libxrlinux_codec.so`
   передаёт в свои вызовы константу **3** (см. `0x6443c`, `0x66714`), но у нас все uid на свежем
   демоне возвращают 0 — валидации по uid, похоже, нет.
4. `AR_MPI_VDEC_AttachVbPool` — обязателен ли он, и **строго ли после** `CreateChn`? У нас вызов до
   создания канала навсегда вешает демон.
5. Требует ли `CreateChn` открытого `/dev/vdecN`? В системе узла `/dev/vdecN` нет (есть только
   `/dev/vctrl` 14:22 и `/dev/mpp_que`), значит узел должен создавать кто-то ещё.
6. Что именно делает `media_service` при старте, что «включает» VDEC (порядок вызовов, модули,
   регистрация в binder)? Достаточно ли повторить этот порядок в своём процессе, чтобы стать
   единственным владельцем кодека?

## 6. Что бы кардинально помогло

* Заголовки SDK Artosyn для AR94xx: `ar_mpi_vdec.h`, `ar_mpi_sys.h`, `ar_mpi_vb.h` (структуры и
  сигнатуры), желательно `sample_vdec.c` из их SDK.
* Любая документация по режимам `mpp_service` (`lib` vs `app`) и по клиентской регистрации в нём.
* Описание API `XrlinuxCodec*` (`libxrlinux_codec.so`): структура конфига для
  `XrlinuxCodecCreateDecoder` — тогда мы пойдём вендорным путём и не будем угадывать MPI.
* Подтверждение/опровержение гипотезы из п.4 от того, кто знает эту платформу.

## 7. Как воспроизвести у нас

```
на очках: /usrdata/tmp/hwdec/
  pd4    — полный пробник: SYS_Init → VB config/Init/ModPool → VDEC_Init → CreateChn → SendStream → GetFrame
  pd5    — то же, но вообще без VB-вызовов (изолирует вопрос «работает ли второй клиент»)
  es.h264 — элементарный поток H.264 1280×720 из реального mp4 (аудио в контейнере — MP3 48 кГц)
  t5..t8.sh — сценарии прогона (остановка стека / лог демона / прогон при живом стеке)
наши доки: firmware/HWDEC.md (всё найденное), firmware/hwdec/ (исходники)
```
Сборка: `clang --target=aarch64-linux-gnu -fuse-ld=lld -nostdlib -O2`,
линковка со всеми `libmpi_*.so`/`libhal_*.so` по имени файла, `--dynamic-linker=/lib/ld-2.25.so`.
Запуск: `LD_LIBRARY_PATH=/usr/lib:/lib ./pd4 ./es.h264 120`.

---

## English brief (same content, for forwarding)

**Goal:** hardware H.264/H.265 decode (Artosyn VDEC) from our own player process on XREAL One Pro
(Artosyn AR9481, SDK `ar9481-ar-sdk-0.17.45-18-mp`), instead of software ffmpeg.

**What works** (verified on device, our own freestanding aarch64 binary linked against the vendor
`libmpi_*`/`libhal_*`): `AR_MPI_SYS_Init` → 0, `AR_MPI_VDEC_Init(40)` → 0,
`AR_MPI_VB_CreatePool` → valid id, `AR_MPI_VB_Init` → 0, `AR_MPI_VB_InitModCommPool(3)` → 0.
`AR_MPI_VB_SetConfig` → −1 (ioctl `_IOW('b',0,648)`).

**Blocker:** `AR_MPI_VDEC_CreateChn` never returns. The process sleeps in binder
(`/proc/<pid>/wchan` = `request_wait_answer`, syscall = `ioctl(/dev/binder, 0xc018410f)` — that is the
VDEC give-cmd `_IOWR('A',15,24)`). The `mpp_service.app` daemon, started manually with stdout captured,
logs nothing about VDEC during the request. Control test: the same probe without any VB calls
(only `SYS_Init` → `VDEC_Init(40)` → `CreateChn`) against the **fully running** vendor stack (daemon,
`media_service`, `pilot`) also hangs, and the stack survives — so it is not a VB provisioning problem
and not a missing server, but the server refusing to create a channel for a foreign process. Calling `AttachVbPool` before `CreateChn` wedges the
daemon permanently (only a reboot clears it). There is no `/dev/vdecN` node, only `/dev/vctrl` and
`/dev/mpp_que`.

`VDEC_CHN_ATTR` reconstructed from the disassembly is 88 bytes and **4 bytes tighter** than the public
Proxima-9311 reverse (no `enVBSource`): type@0, mode@4, maxW@8, maxH@12, streamBufSize@16 (>0x8000),
frameBufSize@20, frameBufCnt@24, refFrameNum@28, temporalMvp@32, tmvBufSize@36, cmdQueueDepth@40.
With this layout validation passes (error `0x80058403` disappears) and the call then blocks.

**Questions:**
1. In daemon mode (`mpp_service.app` running, not `mpp_service=lib`), what must an external client
   process do before `AR_MPI_VDEC_CreateChn` returns? Is there a registration/master handshake?
2. Is the VDEC RPC server the daemon or the first client that called `VDEC_Init` (here `media_service`)?
   Can a second process create its own channel?
3. Is pool provisioning (`VB_SetConfig`/`VB_Init`/`InitModCommPool`) mandatory before `CreateChn`, and
   which `VB_UID` is VDEC on AR9481? (The vendor wrapper passes constant 3.)
4. Must `AttachVbPool` be called after `CreateChn` (ours hangs if called before)?
5. Who is supposed to create `/dev/vdecN`?
6. What exactly does `media_service` do at startup to enable VDEC, i.e. the minimal sequence to become
   the sole owner?

**Most useful artifacts:** Artosyn SDK headers (`ar_mpi_vdec.h`, `ar_mpi_sys.h`, `ar_mpi_vb.h`) and a
`sample_vdec.c` for AR94xx; any doc on `mpp_service` lib/app modes and client registration; the config
struct used by `XrlinuxCodecCreateDecoder` in `libxrlinux_codec.so`.

**Related public reverse (different mode, useful reference, layout shifted +4):**
`github.com/Missing-Lynk/ml-userspace` → `docs/vdec-api.md`, `docs/mpp-buffers.md`,
`docs/decode-display-pipeline.md`.
