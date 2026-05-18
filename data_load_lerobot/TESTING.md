# Probar el plugin LeRobot Loader a mano

Guía paso a paso para verificar el plugin de cero. Asume:

- Repo de plugins en `~/Work/pj-official-plugins` (rama `feature/data_load_lerobot`).
- `plotjuggler_core` local en `~/Work/PJ4/plotjuggler_core`.
- PlotJuggler 4 instalado (extensiones en `~/.local/share/PlotJuggler/PlotJuggler4/extensions/`).
- `conan` 2.x, `cmake`, `ninja`, `huggingface-cli` disponibles.

---

## 1. Tests unitarios rápidos (sin Arrow/FFmpeg, segundos)

Lógica pura: parseo del dataset, síntesis de timeline, aplanado de columnas.

```bash
cd ~/Work/pj-official-plugins/data_load_lerobot
FLAGS="-std=c++20 -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic"
INC="-I ~/Work/PJ4/plotjuggler_core/pj_base/include -I src"
for p in "dataset_model_test:dataset_model" "timeline_test:timeline" "flatten_plan_test:flatten_plan"; do
  t="${p%%:*}"; s="${p##*:}"
  g++ $FLAGS $INC tests/$t.cpp src/$s.cpp -lgtest -lgtest_main -pthread -o /tmp/lr_$s && /tmp/lr_$s
done
```

**Esperado:** los 3 binarios compilan con 0 warnings y `[ PASSED ]` (16 tests en total).

---

## 2. Compilar el plugin (`.so`)

`build.sh` no sirve aquí tal cual: clona `plotjuggler_core` por SSH y no reenvía
argumentos extra de cmake. Usamos cmake a mano apuntando CPM al `plotjuggler_core`
**local** (evita el SSH) y dejamos que conan compile Arrow/FFmpeg (lento la 1ª vez,
se cachea):

```bash
cd ~/Work/pj-official-plugins
BUILD=build/data_load_lerobot/Release

conan install data_load_lerobot --output-folder="build/data_load_lerobot" \
  --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja

cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/data_load_lerobot/conan_toolchain.cmake" \
  -DCMAKE_PREFIX_PATH="$PWD/build/data_load_lerobot" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPJ_BUILD_PLUGIN=data_load_lerobot \
  -DCPM_plotjuggler_core_SOURCE=$HOME/Work/PJ4/plotjuggler_core

cmake --build "$BUILD" --parallel
ctest --test-dir "$BUILD" --output-on-failure   # corre los 3 tests unitarios
```

**Esperado:** `liblerobot_source_plugin.so` + `data_load_lerobot.pjmanifest.json`
en `build/data_load_lerobot/Release/bin/` (o similar), ctest verde.

> 1ª compilación: conan construye Arrow + FFmpeg desde fuente (decenas de min).
> Subsiguientes: cacheado, rápido.

---

## 3. Instalar el plugin en PlotJuggler 4

```bash
DST=~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader
mkdir -p "$DST"
cp ~/Work/pj-official-plugins/build/data_load_lerobot/Release/bin/liblerobot_source_plugin.so "$DST"/
cp ~/Work/pj-official-plugins/data_load_lerobot/manifest.json "$DST"/
# (si el build emite el sidecar .pjmanifest.json, cópialo también)
ls "$DST"
```

Debe quedar junto a `parquet-loader/`, `mcap-loader/`, etc.

---

## 4. Conseguir un dataset LeRobot v2.1 de ejemplo

`lerobot/pusht` es pequeño y v2.1:

```bash
huggingface-cli download lerobot/pusht --repo-type dataset \
  --local-dir ~/datasets/pusht
# Verifica versión:
grep codebase_version ~/datasets/pusht/meta/info.json   # → "v2.1"
ls ~/datasets/pusht/data/chunk-000 | head
ls ~/datasets/pusht/videos/chunk-000                     # cámaras
```

---

## 5. Probar en la app (parte numérica — visible hoy)

1. Lanza PlotJuggler 4 (el binario instalado, o `~/Work/PJ4/run.sh`).
2. **File → Open** (o el botón de cargar datos) → filtro `*.parquet`.
3. Navega a `~/datasets/pusht/data/chunk-000/` y elige cualquier
   `episode_000000.parquet`.
4. Aparece el diálogo **LeRobot Dataset**:
   - Cabecera: ruta · `v2.1` · fps · nº episodios · cámaras.
   - Lista de episodios (`ep N · L frames · task`).
   - Selecciona 2–3 episodios (o **Select all**). Opcional: marca
     **Separate episodes** y pon `gap = 1.0 s`.
   - **OK** (deshabilitado si no hay selección).
5. **Qué verificar:**
   - Aparecen series `lerobot/observation.state.<joint>` y
     `lerobot/action.<...>` (aplanadas con nombres de `info.json`),
     más `lerobot/episode_index`, `lerobot/frame_index`, etc.
   - Arrastra `observation.state.*` a un plot: curva continua.
   - Con varios episodios: van **encadenados** en una sola línea de
     tiempo (sin solaparse). Con gap, hueco plano entre episodios.
   - `episode_index` es escalonado (0,0,…,1,1,…) y el cursor mueve
     todas las curvas a la vez.
   - Cambiar de dataset: botón **Change dataset…** (selector de carpeta).

> **Vídeo:** la app aún **no** muestra vídeo de ObjectStore (es trabajo
> upstream de pj_scene2D/pj_app, fuera de este plugin). El plugin SÍ
> empuja los frames correctamente; eso se valida headless (paso 6).

---

## 6. Verificar el vídeo (headless)

`tests/video_ingest_test` demuxea un mp4 real, aplica el bitstream filter
annex-b y comprueba que se hace `pushOwned` de cada frame con el timestamp
sintetizado correcto, contra un host mock (no necesita pj_scene2D).

```bash
LEROBOT_TEST_MP4=$(ls ~/datasets/pusht/videos/chunk-000/*/episode_000000.mp4 | head -1) \
  ctest --test-dir ~/Work/pj-official-plugins/build/data_load_lerobot/Release \
        -R lerobot_video_ingest_test --output-on-failure
```

**Esperado:** N entradas push con timestamps monótonos crecientes y
payloads no vacíos. (Sin `LEROBOT_TEST_MP4` el test se salta — `SKIPPED`.)

---

## 7. Reset / repetir

Para reprobar de cero: borra `~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader/`
y `~/Work/pj-official-plugins/build/data_load_lerobot/`, y repite desde el paso 2.

---

## Problemas conocidos

- **CPM clona por SSH** si no pasas `-DCPM_plotjuggler_core_SOURCE=` → usa la
  variante con el path local (paso 2).
- **mp4v / AV1**: el push funciona (solo demuxea), pero para *ver* esos
  códecs hace falta que el FFmpeg de **PlotJuggler 4** (no este plugin)
  incluya el decoder. avc1/H.264 y H.265 van out-of-the-box.
- 1ª build de conan (Arrow/FFmpeg desde fuente) es larga; es normal.
