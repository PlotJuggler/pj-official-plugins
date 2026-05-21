# Probar el plugin LeRobot Loader a mano

Guía paso a paso para verificar el plugin de cero. Asume:

- Repo de plugins en `~/Work/pj-official-plugins` (rama `feature/data_load_lerobot`).
- `plotjuggler_core` local en `~/Work/PJ4/plotjuggler_core`.
- PlotJuggler 4 compilado en `~/Work/PJ4` desde la rama `feature/file-backed-video-lerobot`
  (es la que cablea `FileVideoSource` en `Media2DDockWidget` y enciende libdav1d en el
  FFmpeg del host — sin ella, los topics de cámara no se renderizan).
- Extensiones en `~/.local/share/PlotJuggler/PlotJuggler4/extensions/`.
- `conan` 2.x, `cmake`, `ninja`, `huggingface_hub` (Python) disponibles.

---

## 1. Compilar el plugin (`.so`)

El plugin solo necesita Arrow + Parquet + nlohmann_json (no FFmpeg — el plugin
ya no decodifica vídeo, lo hace el host vía `FileVideoSource`). `build.sh` no
sirve aquí porque clona `plotjuggler_core` por SSH y no reenvía argumentos
extra; usamos cmake a mano apuntando CPM al `plotjuggler_core` **local**.

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
```

**Esperado:** `liblerobot_source_plugin.so` (≈ 18 MB) + sidecar
`lerobot_source_plugin.pjmanifest.json` en `build/data_load_lerobot/Release/bin/`.

> 1ª compilación: conan construye Arrow desde fuente (varios minutos).
> Subsiguientes: cacheado, segundos.

---

## 2. Tests unitarios

Tres tests GTest cubren lo headless-verificable: parseo de `meta/info.json` +
`episodes.jsonl` + `tasks.jsonl`, aplanado y dedupe de nombres de columnas
vector, y la serialización de `__pj_fanout` del diálogo.

```bash
ctest --test-dir build/data_load_lerobot/Release -R lerobot --output-on-failure
```

**Esperado:** `100% tests passed, 0 tests failed out of 3` (16 casos en total).

---

## 3. Instalar el plugin en PlotJuggler 4

Solo dos artefactos van a la carpeta de extensiones: el `.so` y el sidecar
`.pjmanifest.json`. El `manifest.json` del repo es **input** de `cmake`
(se embebe en el `.so` y se transforma en el sidecar); no se instala.

```bash
DST=~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader
mkdir -p "$DST"
SRC=~/Work/pj-official-plugins/build/data_load_lerobot/Release/bin
cp "$SRC/liblerobot_source_plugin.so" "$DST/"
cp "$SRC/lerobot_source_plugin.pjmanifest.json" "$DST/"
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
pequeño. Si quieres más episodios, añade sus `episode_0000NN.parquet`/`.mp4`.)

> El vídeo de pusht v2.1 está en **AV1**. PJ4 lo decodifica con su propio
> FFmpeg (libdav1d, habilitado en `feature/file-backed-video-lerobot` por el
> commit `45bb5c4`). El plugin no decodifica nada — solo registra el path.

---

## 5. Probar en la app (numérico + vídeo)

1. Lanza PlotJuggler 4: `~/Work/PJ4/run.sh`.
2. **File → Open** → filtro `*.json`.
3. Navega a `~/datasets/pusht_v21/meta/` y elige `info.json`. (El plugin
   solo reclama `.json` para no solapar con `data_load_parquet` en parquets
   sueltos — el dataset se identifica por `meta/info.json`, no por uno de
   sus parquets.)
4. Aparece el diálogo **LeRobot Dataset**:
   - Cabecera: ruta · `v2.1` · fps · nº episodios · lista de cámaras.
   - Lista de episodios (`ep N · L frames · task`).
   - Selecciona uno o varios episodios (multi-select). **OK**
     queda deshabilitado si no hay selección.
5. **Series escalares** (DataEngine):
   - Aparecen series aplanadas con nombres de `info.json`. En pusht v2.1:
     `lerobot/observation.state.motor_0`, `…motor_1`,
     `lerobot/action.motor_0`, `…motor_1`, más `lerobot/episode_index`,
     `lerobot/frame_index`, `lerobot/next.reward`, etc.
   - Arrastra `observation.state.*` a un plot: curva continua.
6. **Multi-episodio** (`__pj_fanout`):
   - Cada episodio seleccionado se carga como **DatasetId propio**.
     En el catálogo aparecen como `pusht_v21/ep_3`, `pusht_v21/ep_5`, …
     (no se concatenan en un único timeline — cada uno tiene su reloj
     desde 0).
7. **Vídeo de cámara** (`FileVideoSource` en el host):
   - En el árbol del catálogo, cada cámara aparece como **object-topic**
     bajo su episodio: p.ej. `lerobot/observation.image`.
   - **Arrástrala a una vista 2D vacía** (placeholder en el dock).
   - PJ4 lee el `video_file_path` del metadata del topic, abre el MP4
     directamente con `FileVideoSource` (`FfmpegBackend` + libdav1d para
     AV1, seek lazy + ThumbnailCache).
   - Mueve el cursor de tiempo: el frame del vídeo sigue al cursor en
     sincronía con las curvas. Multi-cámara → una vista 2D por cámara.

> El plugin **no** decodifica vídeo. Solo registra el topic con
> `media_class:"video"` + `video_file_path:"/.../episode_*.mp4"`. El
> ObjectStore no recibe bytes (`entryCount == 0` por diseño — ARCH §4.5
> *"File-based video does not go through ObjectStore"*).

---

## 6. Reset / repetir

Para reprobar de cero: borra `~/.local/share/PlotJuggler/PlotJuggler4/extensions/lerobot-loader/`
y `~/Work/pj-official-plugins/build/data_load_lerobot/`, y repite desde el paso 1.

---

## Problemas conocidos

- **CPM clona por SSH** si no pasas `-DCPM_plotjuggler_core_SOURCE=` → usa
  la variante con el path local (paso 1).
- **Códec del vídeo (AV1/H.264/HEVC/…)**: lo decodifica el FFmpeg del **host**
  (PJ4), no el plugin. AV1 requiere que la build de PJ4 incluya libdav1d (la
  rama `feature/file-backed-video-lerobot` lo trae). Si la cámara aparece en
  el árbol pero al arrastrarla a la vista 2D no se ve nada, comprueba que el
  PJ4 que ejecutas es de esa rama y reconstruye con `conan install` para
  re-pillar libdav1d.
- **Diálogo no aparece al cargar `info.json`**: el filtro `*.json` y el
  diálogo se enganchan por la extensión registrada en el manifest del
  plugin. Comprueba que `~/.local/.../extensions/lerobot-loader/` contiene
  el sidecar `.pjmanifest.json` y que lleva `"file_extensions": [".json"]`.
