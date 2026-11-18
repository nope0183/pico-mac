# Pico Mac — CVBS

Эмулятор Macintosh для Raspberry Pi Pico (RP2040) с монохромным композитным
видеовыходом CVBS/RCA. Формируется чересстрочный сигнал PAL или NTSC; видео и
синхронизация выводятся через PIO и DMA.

Проект основан на [evansm7/pico-mac](https://github.com/evansm7/pico-mac).
Исходные уведомления об авторских правах сохранены в файлах и зависимостях.

## Подключение CVBS

| GPIO Pico | Назначение | Резистор |
|---|---|---:|
| GP18 | Видео | 200 Ом |
| GP19 | Синхронизация | 510 Ом |

Подключите каждый GPIO через свой резистор к центральному контакту RCA; землю
Pico — к экрану RCA. По умолчанию используется `VIDEO_PIN=18`; сигнал SYNC всегда
выводится на следующий GPIO. При необходимости измените `VIDEO_PIN` при сборке.

Выход монохромный, без цветовой поднесущей. На RP2040 уровень 240 МГц является
разгоном; частоту можно изменить через `CVBS_SYSCLK_KHZ`, но она должна быть
кратна 30 000 кГц, чтобы сохранить точную частоту PIO.

## Сборка

Нужны Git, CMake, Make, компилятор ARM GCC и Python 3. Клонируйте репозиторий с
подмодулями и установите Pico SDK вместе с TinyUSB:

```sh
git clone --recursive https://github.com/nope0183/pico-mac.git
cd pico-mac
git clone --recursive https://github.com/raspberrypi/pico-sdk.git ../pico-sdk
export PICO_SDK_PATH="$PWD/../pico-sdk"
```

Для запуска требуются совместимые ROM и образ диска, подготовленные локально
через `umac`. ROM и образы дисков не входят в репозиторий:

```sh
make -C external/umac MEMSIZE=128
external/umac/main -r "/path/to/4D1F8172 - MacPlus v3.ROM" -W rom.bin

mkdir -p incbin
python3 tools/bin2c.py rom.bin incbin/umac-rom.h
python3 tools/bin2c.py /path/to/disk.img incbin/umac-disc.h
```

Соберите прошивку для PAL (по умолчанию) или NTSC:

```sh
cmake -S . -B build -DPICO_SDK_PATH="$PICO_SDK_PATH" -DCVBS_STD=PAL
cmake --build build --parallel
# Для NTSC используйте -DCVBS_STD=NTSC
```

Готовый `build/firmware.uf2` загрузите на Pico через BOOTSEL. Скрипт
`build-release.sh` выполняет конфигурацию и сборку при наличии подготовленных
заголовков ROM и диска: `CVBS_STD=NTSC bash build-release.sh` соберёт вариант
для NTSC.

Основные параметры CMake: `CVBS_STD` (`PAL`/`NTSC`), `CVBS_SYSCLK_KHZ`,
`VIDEO_PIN`, `MEMSIZE`, `USE_VGA_RES` и `USE_SD`. При изменении параметров
удалите старый каталог `build` или используйте новый.

## Вклад

Изменения видеотаймингов должны сохранять длительность поля в 312,5 строки для
PAL или 262,5 строки для NTSC. Это ограничение проверяется при компиляции.
