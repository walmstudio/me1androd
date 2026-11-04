# v0.10.13: производительность (по me1_perf_plan, ред. 3)

## Что изменено

| # | Файл | Что сделано |
|---|------|-------------|
| P-1 | `runtime/main_exe.c`, `runtime/hle_thread.c` | **Главный поток игры теперь закрепляется.** `run_entry` вызывает `hle_cpu_place(2)`. Раньше закреплялся только поток загрузчика (`hle_thread_init`), а настоящий main наследовал общую маску, и `hle_cpu_tidy` сбрасывал её на 0-7. Из-за этого EAS держал main на малых ядрах. |
| P-2 | `hle_thread.c` | Новая политика ядер. main и render-поток идут на большие ядра (big). Прочие игровые потоки при ≥ 3 малых ядрах идут на малые, иначе на все ядра. Раньше на 8 Gen 3 им доставались только 2 малых ядра. `hle_cpu_tidy` не трогает «тяжёлые» потоки. С `ME1_CPU_PRIME=1` main получает prime-ядро в одиночку, а render — остальные большие. |
| P-3 | `hle_thread.c`, `hle_d3d9.c` | ADPF (`APerformanceHint`, Android 13+, через dlsym). Создаётся сессия на main и render, цель равна 1/fps_limit. Каждый кадр отправляется отчёт о фактической работе без учёта сна лимитера. |
| P-4 | `hle_hb.c` | Телеметрия в `[hb]`: для каждого потока выводятся `cpu N @F MHz, oncpu X%, rq Y%`, в заголовке — `gpu F MHz` (kgsl / MTK ged / Mali). |
| P-5 | `AndroidManifest.xml` | Добавлены `appCategory="game"` и `isGame="true"`, versionCode 32 / 0.10.13. |
| N1 | `hle_d3d9_vk.h` | Утечка command buffer'ов: вторичные `xcmd[]` теперь освобождаются в `slot_begin`. Массив увеличен с 31 до 64. |
| N3 | `hle_d3d9_vk.h` | `cprims` считает примитивы с учётом instancing (`prims * ni`). |
| R1 | `hle_d3d9.c` | Избыточные `SetRenderState`/`SetSamplerState` (значение не меняется) проверяются без мьютекса устройства. Отключается через `ME1_D3D_FASTSTATE=0`. |
| R3 | `rt.c` | Кэш `dispatch_call` на 64K записей (прямое отображение) перед `rt_module_at` + bsearch. |
| Q6 | `rt.h` | `rep stosw/stosd` выполняется нативным заполнением (memset или векторизуемый цикл). Добавлен release-fence перед быстрыми `rep movs`/`stos`. |
| — | `hle_thread.c` | `ME1_SPIN_HEAVY=1`: на main/render `Sleep(0)` только уступает ядро (без nanosleep). |
| — | `hle_d3d9.c` | В лог выводится `[d3d] fps limit N`, а в строку `[d3d] frame` добавлено `limit N`. |

Чего здесь пока нет (по плану, следующие шаги): кэширование регистров в локальных переменных в `tools/recomp.py`, снижение TSO-барьеров, EXEOPT `-O2`, статические VB/IB на GPU, настоящие occlusion queries, прогрев пайплайнов.

## Новые переменные окружения

| Переменная | По умолчанию | Смысл |
|---|---|---|
| `ME1_CPU_PIN=0` | вкл | Отключает размещение по ядрам целиком. Регистрация потоков для ADPF остаётся. |
| `ME1_CPU_PRIME=1` | выкл | main один на prime-ядре(ах), render на остальных больших. |
| `ME1_UCLAMP=n` | выкл | `sched_setattr` util_min=n (1..1024) для main/render. Ядро может отказать, тогда в логе будет ошибка. |
| `ME1_HEAVY_NICE=n` | выкл | `setpriority(nice=n)` для main/render (например, -4). |
| `ME1_ADPF=0` | вкл | Отключает performance hint. |
| `ME1_SPIN_HEAVY=1` | выкл | `Sleep(0)` на main/render работает как `sched_yield`. |
| `ME1_D3D_FASTSTATE=0` | вкл | Отключает R1. |

## Что смотреть в логе

```
[hle] CPU: 8 cores, fast 3-7 (...), slow 0-2 (...), prime 7: main and render threads -> 3-7, other game threads -> 0-2 ...
[hle] pinned tid 5301 (main) -> 3-7          <- ГЛАВНОЕ: раньше этой строки для main не было
[hle] pinned tid 5333 (render) -> 3-7
[adpf] session: main tid 5301, render tid 5333, target 33.3 ms
[d3d] fps limit 30 (ME1_FPS_LIMIT)
[hb]   tid 1 (main): run 97% ... | cpu 5 @2900 MHz, oncpu 88%, rq 6% | stack: ...
```
Если main по-прежнему на малом ядре (≈1.2 ГГц) или `rq` высокий, пришлите лог. Если ADPF недоступен (Android 12-), в логе будет `[adpf] performance hint API unavailable`.

## Исправления сборки

- `MainActivity.java`: `Build.SOC_MODEL` (API 31) отсутствует в android-29.jar, поэтому поле теперь читается через reflection.
- `GameActivity.java` (`onGameRestart`): в коде были несуществующие `Intent.cloneIntent()` и `PendingIntent.FLAG_ACTIVITY_NEW_TASK`. Заменены на `new Intent(getIntent()).addFlags(FLAG_ACTIVITY_NEW_TASK)` и `FLAG_ONE_SHOT | FLAG_IMMUTABLE`. Исходники 0.10.12 из-за этих ошибок не компилировались.
- `tools/recomp_all.sh`: генерация всего `out/` одной командой.
- `tools/ilt_seeds.py` + `tools/seeds/MassEffect_ilt_funcs.txt` заменяют отсутствовавший `../analysis/ilt_funcs.txt`. MassEffect.exe собран с инкрементальной линковкой: таблица из 76236 thunk'ов `jmp` находится по адресу 10901005. Без этих seeds сканер C++ EH находит 459 FuncInfo и 4 landing-цели, с ними — 12194 и 22.

## Сборка

```
python3 -m pip install --user capstone
tools/recomp_all.sh <Binaries игры> <папка с msvcr80/msvcp80/d3dx9_31>   # out/, ~6 мин
make -j2 game-lib                                                        # NDK r27c в /data/tools/android-ndk-r27c, ~25 мин на 2 ядрах
make build/android/libme1spv.so GLSLANG_SRC=<glslang-15.0.0, собранный NDK в build-android>
W=<рабочая папка> android/gpu/build_gpu.sh                               # Mesa zink + libadrenotools (опционально)
D3DX_DLL=<путь>/d3dx9_31.dll android/build_apk.sh                        # SDK build-tools 30.0.3, android-29.jar, JDK 17
```
