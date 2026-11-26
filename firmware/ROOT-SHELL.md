# Root shell и SSH на очках (perf37)

Состояние: **собрано и проверено, на очки ещё не записано** (на устройстве сейчас
perf36). Этот файл — описание доступа, которое ставится вместе с образом.

## Учётные данные

| Что | Значение |
|---|---|
| Пользователь | `root` |
| Пароль | `Xreal#Root26` |
| Хеш в `/etc/passwd` | `$1$Xr7kQm2p$bCxzH1op71tPwoe.qBNMI0` (MD5-crypt, соль `Xr7kQm2p`) |
| Ключ | `root`-ключ ed25519: приватный `~/.ssh/xreal_onepro` на Mac, публичный лежит в `/etc/dropbear/authorized_keys` на очках |

Пароль зашит в образ хешем; резервный вход — по ключу, если в dropbear отключена
парольная аутентификация. После первого входа пароль лучше сменить (`passwd`), потому
что в этом файле он записан открытым текстом.

## Как подключиться

Очки отдают две CDC-ECM сети по USB-C; macOS поднимает их сам, адреса link-local:

| Mac | Очки |
|---|---|
| `en10` 169.254.1.10/24 | `169.254.1.1` (`usb1`) |
| `en11` 169.254.2.10/24 | `169.254.2.1` (`usb0`) |

```sh
ssh root@169.254.1.1                              # по паролю
ssh -i ~/.ssh/xreal_onepro root@169.254.1.1       # по ключу
scp -i ~/.ssh/xreal_onepro файл root@169.254.1.1:/usrdata/   # scp на очках есть
```

Проверка до входа: `ping 169.254.1.1` и `nc -z 169.254.1.1 22`.

## Что делает payload

`firmware/shell_bootstrap.c` компилируется в `libxreal_cpu_adapter.so`. Pilot
запускается от root и загружает этот адаптер, поэтому конструктор библиотеки
выполняется один раз на старте Pilot. Все шаги идемпотентны:

1. меняет хеш root в `/etc/passwd` (бэкап рядом: `/etc/passwd.hermes.bak`),
2. кладёт публичный ключ в `/etc/dropbear/authorized_keys`,
3. переименовывает маркер runit `/etc/services/dropbear/down` в `down.disabled` —
   после этого dropbear поднимается при каждой загрузке,
4. пишет `u` в `/etc/services/dropbear/supervise/control`, чтобы runsv запустил его
   сразу.

Отчёт о каждом шаге — в `/usrdata/shell-bootstrap.log`; файл читается по USB HID
командой экспорта, то есть результат виден, даже если SSH почему-то не поднялся.

Ограничения выбраны осознанно: используются только `open/read/write/close/rename` из
ABI устройства, без `fork`, потоков, `malloc` и stdio. Ошибка в этом коде не может
повесить или уронить рендер; худшее — шаг не выполнится и это будет видно в логе.

## Что доступно после входа

Шелл — busybox `ash` (`/bin/sh` в `/etc/passwd`). Есть апплеты: `ls cp mv rm mkdir sed
awk ps df mount dmesg kill ping tar gzip vi lsof chmod devmem i2cget i2cset insmod
rmmod mknod mkfifo setsid stty hexdump telnetd inetd tftp wget`, плюс отдельный
`/bin/scp`. Нет `bash`, `nc`, `top`, `ifconfig`, `route`, клиента `ssh`.

## Выключить или откатить

```sh
sv down /etc/services/dropbear          # остановить ssh
touch /etc/services/dropbear/down       # запретить автозапуск
sed -i "s|^root:[^:]*:|root:$1$GwSWKfha$5f2ott.UiOUkSGo3ceE9A0:|" /etc/passwd   # вернуть штатный хеш
```

Полный откат — прошить предыдущий образ (`perf36 cinema`, SHA256
`b5d3f4fa385882b91a4636048e1e62aa595358522780afef2f00c28d87e874e4`): HID-обновлятор
обслуживается отдельным демоном `command_service` и работает независимо от Pilot.
Изменения в `/etc` и `/usrdata` при откате образа не откатываются — их надо снять
руками по командам выше.

## Безопасность

Очки доступны только по USB link-local, но пароль в этом файле открыт. Не публиковать
этот файл в общедоступной ветке и не прикладывать его к релизу, пока пароль не сменён.
