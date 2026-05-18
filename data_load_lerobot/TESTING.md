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

⚠️ `lerobot/pusht` en `main` **ya es v3.0** (el plugin lo rechazará con un
mensaje claro — comportamiento correcto). Hay que pedir el **tag `v2.1`**.
`huggingface-cli` está deprecado; usa `huggingface_hub` por Python:

```bash
python3 -c "
from huggingface_hub import snapshot_download
snapshot_download('lerobot/pusht', repo_type='dataset', revision='v2.1',
  local_dir='$HOME/datasets/pusht_v21',
  allow_patterns=['meta/*','data/chunk-000/episode_000000.parquet',
                  'videos/chunk-000/*/episode_000000.mp4'])
"
grep -o '\"codebase_version\"[^,]*' ~/datasets/pusht_v21/meta/info.json  # → "v2.1"
ls ~/datasets/pusht_v21/data/chunk-000
find ~/datasets/pusht_v21/videos -name '*.mp4'
```

(Quita `allow_patterns` para bajar el dataset completo — sigue siendo
pequeño. Si quieres más episodios añade sus `episode_0000NN.parquet`/`.mp4`.)

> Nota: el vídeo de pusht v2.1 está en **AV1**. El push headless (paso 6)
> funciona igual; *verlo* en la app requeriría que el FFmpeg de PlotJuggler 4
> incluya el decoder AV1 (libdav1d) — ver "Problemas conocidos".

---

## 5. Probar en la app (numérico + imágenes)

1. Lanza PlotJuggler 4 (el binario instalado, o `~/Work/PJ4/run.sh`).
2. **File → Open** (o el botón de cargar datos) → filtro `*.parquet`.
3. Navega a `~/datasets/pusht_v21/data/chunk-000/` y elige
   `episode_000000.parquet`.
4. Aparece el diálogo **LeRobot Dataset**:
   - Cabecera: ruta · `v2.1` · fps · nº episodios · cámaras.
   - Lista de episodios (`ep N · L frames · task`).
   - Selecciona 2–3 episodios (o **Select all**). Opcional: marca
     **Separate episodes** y pon `gap = 1.0 s`.
   - **OK** (deshabilitado si no hay selección).
5. **Qué verificar:**
   - Aparecen series aplanadas con nombres de `info.json`; en pusht v2.1:
     `lerobot/observation.state.motor_0`, `…motor_1`,
     `lerobot/action.motor_0`, `…motor_1`, más `lerobot/episode_index`,
     `lerobot/frame_index`, `lerobot/next.reward`, etc.
   - Arrastra `observation.state.*` a un plot: curva continua.
   - Con varios episodios: van **encadenados** en una sola línea de
     tiempo (sin solaparse). Con gap, hueco plano entre episodios.
   - `episode_index` es escalonado (0,0,…,1,1,…) y el cursor mueve
     todas las curvas a la vez.
   - Cambiar de dataset: botón **Change dataset...** (selector de carpeta).
6. **Imágenes de cámara:** en el árbol de datos aparece la cámara como
   object-topic (p.ej. `lerobot/observation.image`). **Arrástrala a una
   vista 2D vacía** (panel/placeholder de visualización). Debe abrirse un
   **2D View** mostrando el frame; al mover el cursor de tiempo la imagen
   avanza sincronizada con las curvas.

> El plugin **decodifica** cada frame (AV1/H.264/…) y lo re-encoda **JPEG**,
> registrándolo como `builtin_object_type:"kImage"`. Por eso PJ4 lo muestra
> con su pipeline JPEG built-in **sin parser** y **sin depender del set de
> códecs de PJ4** (pusht v2.1 es AV1; lo resuelve el plugin con dav1d
> propio). Multi-cámara: una vista 2D por cámara.

---

## 6. Verificar las imágenes (headless)

`tests/video_ingest_test` decodifica un mp4 real, re-encoda JPEG y, contra
un host mock, comprueba que **cada entrada es un JPEG válido** (SOI/EOI) al
timestamp sintetizado correcto, y que el primero **decodifica a dimensiones
reales**. No necesita pj_scene2D ni GUI.

```bash
LEROBOT_TEST_MP4=$(find ~/datasets/pusht_v21/videos -name 'episode_000000.mp4' | head -1) \
  ctest --test-dir ~/Work/pj-official-plugins/build/data_load_lerobot/Release \
        -R lerobot_video_ingest_test --output-on-failure
```

Verificado contra el mp4 real de pusht v2.1 (AV1): 3/3 OK — JPEGs válidos,
timestamps correctos, primer frame decodifica a dimensiones reales.
(Sin `LEROBOT_TEST_MP4` los tests data-driven se saltan — `SKIPPED`.)

---

## 7. Reset / repetir

Para reprobar de cero: borra `~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader/`
y `~/Work/pj-official-plugins/build/data_load_lerobot/`, y repite desde el paso 2.

---

## Problemas conocidos

- **CPM clona por SSH** si no pasas `-DCPM_plotjuggler_core_SOURCE=` → usa la
  variante con el path local (paso 2).
- **Códec del vídeo (AV1/H.264/HEVC/…)**: ya **no** es un problema. El
  plugin decodifica con su propio FFmpeg (AV1 vía libdav1d) y emite JPEG,
  así que PJ4 solo necesita su decodificador JPEG (turbojpeg, siempre
  presente). El set de códecs de PJ4 es irrelevante para estas imágenes.
- `swscaler: deprecated pixel format used` en logs: **benigno**. Es la
  conversión yuv420p→yuvj420p (full-range) para el encoder MJPEG; la
  imagen sale correcta.
- 1ª build de conan (Arrow/FFmpeg desde fuente, ahora con dav1d) es larga;
  es normal.
