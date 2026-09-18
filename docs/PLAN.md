# Original architecture plan

This is the plan approved before any code was written (2026-09-02), preserved
verbatim below. It is the source of the *why* behind most of the non-obvious
decisions in this codebase: the JSONL format investigation, the chunking
rationale, the prefix conflict rule, the phase breakdown.

**Read [HANDOFF.md](../HANDOFF.md) first.** It preserves the empirical Phase 1
handoff and now records the Phase 2 closeout. The original plan below remains
historical; consult the current git state alongside the durable docs for
subsequent work.

Known supersessions (see HANDOFF.md for detail):

- §4 lists `casi config` as the only non-core command. Phase 1 added `casi
  exclude` / `casi include` for project selection -- a need surfaced after
  this plan was written, when the question "does this sync everything under
  every repo?" came up before Phase 1 started.
- §5.1's shared root-name namespace and shared `sync.exclude` policy landed
  in Phase 2. The canonical configuration lives on its own authoritative ref
  (`refs/heads/casi/config`), with optimistic updates and legacy local-exclude
  migration; see `docs/DESIGN.md` and HANDOFF.md.
- §7.1's file tree names modules that ended up organised slightly differently
  once written (e.g. `core/ctx.c` for the "everything a command needs, opened
  once" context, `util/jsonl.c` for the line-scanning helpers). The actual
  tree is in HANDOFF.md and is authoritative.
- §8's integration test list remains the target. `roundtrip` is now covered
  by `two_machines`; `large_session` and an autonomous `ssh_remote` CI job
  are still missing.
- §11's Phase 3 scope was partially delivered earlier than planned: conflict
  parking, `pull --theirs`, `--dry-run`, `--porcelain`, and atomic
  materialization are already implemented. The remaining Phase 3 scope is
  stat-cache/append-tail hashing, `large_session`, `casi conflicts`, GC/repack,
  and an interruption audit.

Everything else below reflects what was actually built.

---

<!-- original plan follows unchanged -->

# casi — Coding Assistant Session Interchange

## Contexto

Hoy las sesiones de Claude Code se sincronizan entre las tres máquinas del usuario con
`~/.local/bin/claude-sync.sh`: un mesh SSH + `rsync --update` con reescritura de paths por
`sed`. Funciona, pero tiene límites estructurales documentados en `SYNC.md` (fuera de este
repo, en la laptop del usuario):

- **Requiere que las máquinas estén en la misma red y prendidas al mismo tiempo.** Si `nemo-pc`
  está apagada cuando trabajás en la Mac, esa sesión no viaja hasta que vuelvan a coincidir.
- **`rsync --update` es last-write-wins por mtime, sin detección de conflicto.** Si la misma
  sesión creció en dos máquinas, una pisa a la otra en silencio.
- **Reescribe archivos enteros.** Una sesión de 137 MB se re-transfiere y se re-escribe completa
  en cada corrida, y hay que hackear el mtime para que no se retroalimente.
- **Un solo prefijo por máquina.** No modela que esta laptop tiene layout anidado
  (`~/src/work/bookit/<repo>`) y las otras dos aplanado (`~/src/<repo>` + symlink farm).
- **Sin integridad ni deduplicación.** Un `sed` a medias sobre un JSONL de 137 MB deja el archivo
  corrupto sin forma de detectarlo.
- **N×N configuración.** Sumar una máquina implica editar la tabla `M_*` en las tres copias del
  script.

`casi` reemplaza eso con un hub git: cada máquina hace push/pull contra un remote git cualquiera
(GitHub, Gitea, un bare por SSH, un pendrive con `file://`). Se gana asincronía, integridad de
contenido, deduplicación, transferencia incremental y detección real de conflictos —
manteniendo una UX de cinco comandos y sin exponer nunca branches ni commits.

---

## 1. Investigación: formato real de las sesiones de Claude Code

Todo lo de abajo está verificado contra `~/.claude/` en esta máquina (Claude Code 2.1.258,
13 proyectos, 47 sesiones, 390 MB).

### 1.1 Layout en disco

```
~/.claude/projects/
  -Users-fdipietro-src-bookit/              # nombre = cwd de arranque, codificado
    b8552166-…-d49b373dfc6b.jsonl           # la sesión: 1 sessionId por archivo
    b8552166-…-d49b373dfc6b/                # sidecar de ESA sesión
      subagents/agent-a37655e2faa29b599.jsonl
      subagents/agent-a37655e2faa29b599.meta.json
    memory/                                 # memoria por proyecto (markdown)
      MEMORY.md
      feedback_no-push-automatico.md
```

Fuera de `projects/` hay estado por sesión que **no** se sincroniza (ver §1.5):
`~/.claude/file-history/<uuid>/`, `session-env/<uuid>/`, `shell-snapshots/`,
`history.jsonl`, `~/.claude.json`.

### 1.2 El nombre de carpeta es una función con pérdida — nunca invertirlo

Claude Code deriva el nombre del directorio del `cwd` de arranque sustituyendo caracteres no
alfanuméricos por `-`. Verificado empíricamente:

| cwd real | carpeta |
|---|---|
| `/Users/fdipietro/src/etl_fsearch` | `-Users-fdipietro-src-etl-fsearch` |
| `/Users/fdipietro/src/bookit/bookit-knowledge` | `-Users-fdipietro-src-bookit-bookit-knowledge` |
| `/Users/fdipietro/src/bookit-knowledge` | `-Users-fdipietro-src-bookit-knowledge` |

`/` y `_` colapsan ambos a `-`. La función **no es inyectiva**: dos paths distintos pueden dar
la misma carpeta. Consecuencia de diseño dura: **casi nunca decodifica el nombre de carpeta.**
Guarda el path absoluto real (leído del campo `cwd`) en metadata explícita, y al materializar en
otra máquina *re-codifica* desde el path canónico usando el encoder del provider.

Además: `-Users-fdipietro-src-bookit/` contiene sesiones con 29 valores distintos de `cwd`
(subdirectorios del repo). El nombre de carpeta refleja el `cwd` **de arranque**; el campo `cwd`
de cada registro refleja el directorio actual, que muta con `cd` durante la sesión. El path
canónico del proyecto = `cwd` del primer registro que lo tenga.

> **Nota post-implementación:** esta regla exacta (`[^a-zA-Z0-9]` → `-`, por code point Unicode,
> no por byte) fue confirmada en vivo durante la fase 1 provocando una sesión con un path que
> distinguía las dos hipótesis en danza (un `.` en el nombre). Ver `src/path/encoding.c` y su
> comentario de cabecera para el detalle y la evidencia.

### 1.3 Formato del JSONL

Una línea = un objeto JSON. `type` discrimina. Distribución real sobre 47 sesiones:

| type | qué es | ¿lo necesita el resume? |
|---|---|---|
| `assistant` / `user` | turnos de conversación (`message`, `uuid`, `parentUuid`, `toolUseResult`) | sí |
| `attachment` | adjuntos y resultados grandes referenciados | sí |
| `system` | comandos locales (`/model`, etc.) | sí |
| `queue-operation`, `mode`, `permission-mode` | estado de UI | sí (baratos) |
| `ai-title`, `custom-title`, `last-prompt`, `atis-latch` | estado mutable, reescrito por **append** | sí |
| `file-history-snapshot`, `file-history-delta` | punteros al backup de archivos (`~/.claude/file-history/`) | punteros sí, contenido no |
| `cost-state` | métricas de la sesión | sí (baratas) |

Campos presentes en casi todo registro con contenido: `sessionId`, `uuid`, `parentUuid`,
`timestamp`, `cwd`, `gitBranch`, `version`, `userType`, `entrypoint`, `isSidechain`.

**Propiedades que el diseño explota:**

- **Append-only en la práctica.** El estado mutable (títulos, modo) se re-emite como registro
  nuevo al final en vez de reescribirse: 903 registros `custom-title` en una sola sesión. Los
  timestamps son casi monótonos (339 inversiones sobre 32.154 líneas, por concurrencia de
  escritura, no por reescritura).
- **1 sessionId por archivo**, verificado en las tres sesiones más grandes.
- **Tamaños muy dispares**: mediana ~100 KB, máximo 137 MB / 32.154 líneas.
- **Paths absolutos embebidos en el contenido**: 8.534 ocurrencias de `/Users/fdipietro` en los
  40 archivos chicos. No alcanza con reescribir `cwd`.

El diseño **no asume** append-only estricto: si el prefijo cambia (`/rewind`, compactación), se
detecta y se trata como divergencia (§6.2), nunca se corrompe.

### 1.4 Sidecar `subagents/`

`<uuid>/subagents/agent-<id>.jsonl` (transcript del subagente) + `agent-<id>.meta.json`
(`agentType`, `description`, `toolUseId`, `parentAgentId`, `spawnDepth`). Archivos chicos.
Viajan con la sesión.

### 1.5 Qué NO se sincroniza (decidido)

| Ruta | Por qué no |
|---|---|
| `~/.claude/file-history/<uuid>/` | Copias de archivos reales del repo, pesadas, atadas a paths locales; solo sirven para `/rewind` en esa máquina. |
| `~/.claude.json` | Estado global: `oauthAccount`, `machineID`, `userID`, caches de experimentos. Sincronizarlo es activamente dañino. |
| `session-env/`, `shell-snapshots/`, `history.jsonl` | Estado efímero de la terminal local. |
| `~/.claude/CLAUDE.md`, `settings.json`, `skills/` | Fuera de alcance en v1 — el usuario ya los maneja aparte a propósito. Punto de extensión futuro como "colección de perfil". |

---

## 2. Arquitectura general

```
                      casi (binario C, ~200-400 KB)
  ┌──────────────────────────────────────────────────────────────────┐
  │ cmd/       init · push · pull · sync · status · config           │
  ├──────────────────────────────────────────────────────────────────┤
  │ core/                                                            │
  │   scan     recorre el provider, aplica el stat-cache             │
  │   chunk    normaliza → parte en chunks → OIDs                    │
  │   tree     arma el árbol git en memoria (git_treebuilder)        │
  │   sync     3-way a nivel sesión: base / local / remoto           │
  │   conflict aparta divergencias                                   │
  ├──────────────────────────────────────────────────────────────────┤
  │ provider/  vtable · claude_code.c        ← seam de extensibilidad │
  │ path/      roots · normalize · encoding                          │
  │ util/      fs · buf · str · jsonl · config · error · log         │
  ├──────────────────────────────────────────────────────────────────┤
  │ libgit2 1.9.x — ODB, treebuilder, refs, transporte SSH/HTTPS/file│
  └──────────────────────────────────────────────────────────────────┘
```

### 2.1 Sin worktree, nunca

El repo interno es **bare**. `casi` escribe blobs con `git_blob_create_from_buffer`, arma árboles
con `git_treebuilder`, crea commits con `git_commit_create`, y transfiere con `git_remote_fetch` /
`git_remote_push`. **Jamás usa `git_checkout_*` ni un índice.**

Al materializar, escribe directo en `~/.claude/projects/...` con su propia capa `fs_`.

Esto no es un detalle: elimina de raíz `core.autocrlf`, el bit de modo Unix, los symlinks y la
semántica de checkout de libgit2 — que son exactamente los cuatro dolores de portar a Windows.

### 2.2 Ubicaciones

| Qué | Ruta (XDG, con fallbacks) |
|---|---|
| Config | `~/.config/casi/config` (formato INI git, leído con `git_config_*`) |
| Repo interno bare | `~/.local/share/casi/repo.git` |
| Stat-cache | `~/.local/share/casi/index` |
| Conflictos apartados | `~/.local/share/casi/conflicts/` |
| Override para tests | `$CASI_HOME`, `$CASI_CLAUDE_HOME` |

Usar `git_config_*` de libgit2 para nuestro propio archivo de config ahorra escribir un parser y
da `casi config remote.origin.url <url>` con semántica idéntica a git, gratis.

### 2.3 El seam de providers

```c
typedef struct casi_provider {
    const char *name;                                    /* "claude-code" */
    int (*discover)(casi_ctx*, casi_item_list *out);     /* enumera sesiones + colecciones */
    int (*read_item)(casi_ctx*, const casi_item*, casi_buf *out);
    int (*materialize)(casi_ctx*, const casi_item*, const casi_buf*);
    int (*encode_project_dir)(const char *canonical_path, casi_buf *out);
    int (*normalize)(casi_ctx*, casi_buf *inout);        /* paths locales → canónicos */
    int (*denormalize)(casi_ctx*, casi_buf *inout);      /* canónicos → paths locales */
} casi_provider;
```

El árbol remoto namespacea por provider (`sessions/claude-code/...`), así que agregar Cursor o
Codex CLI es implementar el vtable + registrar el nombre. La v1 registra uno solo.

---

## 3. Formato del repositorio remoto

### 3.1 Chunking: la decisión que hace viable el modelo

Git guarda blobs enteros. Una sesión de 137 MB que crece 100 KB generaría un blob nuevo de
137 MB en cada push. Inaceptable.

**Solución:** partir cada JSONL, ya normalizado, en chunks de ~1 MiB cortando siempre en límite
de línea, desde el byte 0, determinísticamente. Como el archivo es append-only, todos los chunks
previos son byte-idénticos → mismos OIDs → el árbol nuevo solo agrega el chunk de cola.

Deduplicación perfecta sin lógica de deltas, sin ventana rodante, sin CDC.

**El orden importa: normalizar primero, chunkear después.** Así dos máquinas con paths distintos
producen **blobs idénticos** para el mismo contenido de sesión. La dedup y la regla de prefijo
(§6.2) funcionan entre máquinas, no solo dentro de una.

### 3.2 Layout del árbol

```
casi.json                          # {"format":1,"crypto":"none","providers":["claude-code"]}
sessions/claude-code/<pid>/<sid>/
    meta.json                      # {sessionId, projectPath:"casi://src/bookit",
                                   #  chunkCount, totalBytes, originMachine, updatedAt}
    chunks/000000 000001 …         # slices del JSONL normalizado
    subagents/agent-<id>.jsonl
    subagents/agent-<id>.meta.json
projects/claude-code/<pid>/
    meta.json                      # {projectPath:"casi://src/bookit"}
    memory/MEMORY.md …
```

- `<pid>` = SHA-256 hex del path canónico del proyecto. Hex puro: sin caracteres ilegales, sin
  nombres reservados de Windows (`CON`, `NUL`, `AUX`…), sin problemas de mayúsculas. El nombre
  legible vive en `meta.json` y `casi status` lo imprime.
- `<sid>` = el UUID de sesión tal cual.

### 3.3 Un branch por máquina (interno, invisible)

`refs/heads/casi/<machine>`, donde `<machine>` es la etiqueta de `core.machine`.

- **`casi push` nunca puede ser rechazado por no-fast-forward** — cada máquina es dueña de su ref.
  Sin loop de reintentos, sin carreras entre máquinas.
- **`casi pull` hace fetch de `refs/heads/casi/*`** y toma la unión de sesiones con la regla de
  prefijo. Los objetos se comparten entre branches, así que N branches no cuestan N× espacio.
- El usuario nunca ve esto. `casi status` habla de *sesiones* y *máquinas*, jamás de refs.

Refs locales adicionales: `refs/casi/base` = último estado con el que se reconcilió, para
distinguir "borré esto" de "nunca lo tuve" cuando se implementen los borrados (fase 6).

---

## 4. Diseño de CLI

```
casi init [--remote <url>] [--machine <nombre>]
casi push [--dry-run] [-v] [--porcelain]
casi pull [--dry-run] [-v] [--porcelain] [--theirs <session-id>]
casi sync                       # pull seguido de push — el comando de uso diario
casi status [--porcelain]
casi config <clave> [<valor>] | --list | --unset <clave>
```

Plumbing documentado pero fuera del `--help` principal: `casi doctor` (diagnostica roots sin
mapear, remote inalcanzable, repo corrupto), `casi gc`, `casi conflicts`.

Globales: `-v/--verbose`, `-q/--quiet`, `--no-color`, `--casi-dir`, `--claude-home`.

**Exit codes:** `0` ok · `1` error · `2` uso incorrecto · `3` hay conflictos · `4` red/auth ·
`5` roots sin mapear.

### 4.1 Config (`~/.config/casi/config`, formato INI de git)

```ini
[core]
	machine = mac-air
	provider = claude-code
[remote "origin"]
	url = git@github.com:francodipietro/mis-sesiones.git
[root "src"]
	path = /Users/fdipietro/src
[root "bookit"]
	path = /Users/fdipietro/src/bookit
[sync]
	memory = true
	subagents = true
[crypto]
	mode = none
```

### 4.2 Salida de `casi status`

```
máquina: mac-air   remote: git@github.com:francodipietro/mis-sesiones.git

  para subir      4 sesiones   (bookit 3, casi 1)          +2.1 MB
  para bajar      1 sesión     (aid-followup 1)            +340 KB
  al día         42 sesiones

  ⚠ 1 conflicto: bookit / b8552166  — divergió con nemo-pc
      copia remota en ~/.local/share/casi/conflicts/
      resolvé con: casi pull --theirs b8552166
```

Ni una palabra sobre commits, branches, refs u objetos.

---

## 5. Normalización de paths

### 5.1 Named roots — no un prefijo, una tabla

El problema real (de SYNC.md) no es solo `$HOME` distinto: es que esta laptop tiene
`~/src/work/bookit/<repo>` anidado y las otras dos `~/src/<repo>` aplanado. Un prefijo único no
lo modela.

Cada máquina declara sus paths locales para **nombres lógicos compartidos**:

| máquina | `root "src"` | `root "bookit"` |
|---|---|---|
| esta laptop (Linux) | `/home/fdipietro/src/work` | `/home/fdipietro/src/work/bookit` |
| `nemo-pc` | `/home/nemo/src` | `/home/nemo/src` |
| `mac` | `/Users/fdipietro/src` | `/Users/fdipietro/src/bookit` |

Forma canónica: `casi://<root>/<resto>`. `$HOME` es un root implícito (`casi://~/...`) con la
prioridad más baja.

- **Normalizar** (push): match por **prefijo más largo** sobre la tabla ordenada por longitud
  descendente. `/Users/fdipietro/src/bookit/hotels-engine` → `casi://bookit/hotels-engine`
  (gana `bookit` sobre `src`).
- **Desnormalizar** (pull): sustitución directa por el path local de ese root.
- Un `casi://<root>/` desconocido en el pull → warning nombrando el root y exit code `5`, con la
  línea exacta de `casi config root.<name>.path <path>` para arreglarlo. Nunca escribe basura.

Los nombres de root vistos en el remote se listan en `casi.json`, así que sumar una máquina es
`casi init` + declarar sus paths locales para los roots que ya existen — no hay que tocar la
config de las otras máquinas (contra el N×N del script actual).

### 5.2 Tres lugares donde hay que reescribir

1. **El nombre del directorio de proyecto.** Se *re-codifica* desde el path canónico con el
   encoder del provider. Nunca se decodifica el nombre de la máquina de origen (§1.2).
2. **El campo `cwd`** de cada registro.
3. **Paths absolutos embebidos** en `message`, `toolUseResult`, `content`, y en los `.md` de
   `memory/`.

### 5.3 Sustitución a nivel bytes, nunca parse + reserialize

La reescritura opera sobre **los bytes crudos de cada línea**, no sobre un JSON parseado. Un
round-trip parse/serialize podría cambiar orden de claves, formato de números o escapes, y romper
el resume de Claude Code — que además hoy emite claves en orden estable.

En Unix los paths no contienen caracteres que JSON escape, así que la sustitución literal es
segura. La capa `util/jsonl.c` escanea líneas y valida que cada una siga siendo JSON bien formado
después de la sustitución (chequeo barato de balanceo, sin construir el árbol); si una línea falla,
se aborta la sesión entera con error — nunca se escribe a medias.

En Windows los paths sí necesitan cuidado (`\` es escape en JSON: `"C:\\Users\\..."`), por eso el
sustituidor toma la variante escapada además de la cruda. Se deja el gancho ahora, se implementa
en la fase de Windows.

---

## 6. Sync incremental y conflictos

### 6.1 Incremental en las dos puntas

**En la red:** `git_remote_fetch`/`git_remote_push` ya negocian packfiles — solo viajan los
objetos faltantes. Gratis por usar libgit2.

**En disco (el que importa con 390 MB):** stat-cache en `~/.local/share/casi/index`, el mismo
truco que el índice de git:

```
ruta local → (size, mtime, ino, chunk_count, oid[0..n-1], oid_normalizado)
```

- `size` y `mtime` iguales → se reusan los OIDs, cero lectura.
- `size` creció y los chunks completos previos siguen intactos → **se hashea solo la cola**.
- Cualquier otra cosa → re-chunk completo.

Sin esto, `casi status` releería 390 MB en cada invocación.

### 6.2 Conflictos: la regla de prefijo

Como el contenido es append-only, comparar la **lista de OIDs de chunks** resuelve todo, sin
timestamps ni relojes:

| Relación | Significado | Acción |
|---|---|---|
| listas idénticas | al día | nada |
| remota es prefijo de local | local va adelante | `push` la sube |
| local es prefijo de remota | remota va adelante | `pull` la baja |
| ninguna es prefijo de la otra | **divergencia real** | conflicto |

Solo el último chunk puede ser parcial, así que la comparación es exacta y O(nº de chunks).

**Política por defecto (decidida): preservar local, apartar remota.**

- El archivo local queda intacto.
- La versión remota se materializa en
  `~/.local/share/casi/conflicts/<sid>-<máquina>-<timestamp>.jsonl`.
- `casi pull` sigue con el resto de las sesiones, reporta el conflicto y termina con exit `3`.
- `casi pull --theirs <session-id>` invierte la elección (y aparta la local).

Cero pérdida de datos, cero prompts, apto para correrse desatendido al iniciar sesión.

### 6.3 Borrados

**La v1 no borra nada, en ninguna dirección.** Las sesiones se acumulan. `refs/casi/base` se
mantiene desde el día uno para que los tombstones de la fase 6 sean un 3-way correcto, no una
adivinanza.

---

## 7. Estructura del proyecto en C y build

### 7.1 Árbol

```
casi/
  CMakeLists.txt  CMakePresets.json  LICENSE  README.md  CONTRIBUTING.md
  cmake/            FindLibgit2.cmake, opciones, toolchains
  include/casi/     casi.h  provider.h  (API interna estable entre módulos)
  src/
    main.c          dispatch de argumentos
    cmd/            cmd_init.c cmd_push.c cmd_pull.c cmd_sync.c cmd_status.c
                    cmd_config.c cmd_doctor.c
    core/           scan.c chunk.c tree.c sync.c conflict.c index.c
    provider/       provider.c claude_code.c
    path/           roots.c normalize.c encoding.c
    util/           fs.c buf.c str.c jsonl.c config.c error.c log.c
  tests/
    unit/           test_chunk.c test_roots.c test_encoding.c test_jsonl.c
                    test_index.c test_sync.c
    integration/    two_machines.c  large_session.c  conflict.c  ssh_remote.c
    fixtures/       sesiones sintéticas + generador
    vendor/         utest.h  (single-header, MIT)
  tools/            gen_fixtures.c  scrub.c
  docs/             FORMAT.md  DESIGN.md
  .github/workflows/ci.yml
```

### 7.2 Build: CMake ≥ 3.20

CMake, no Meson ni Makefile plano, por tres razones concretas:

1. **libgit2 es CMake-first.** `FetchContent` con un tag fijado permite compilarlo desde fuente y
   linkearlo estático para los binarios prebuilt, sin fricción.
2. **Windows/MSVC es el camino trillado** en CMake. Meson funciona pero exige Python en la máquina
   de build; un Makefile plano directamente cierra la puerta a MSVC.
3. **CTest** cubre unit e integración con un solo `ctest --output-on-failure`.

Estrategia de dependencias, en orden:

1. `pkg_check_modules(LIBGIT2 libgit2>=1.7)` — el camino normal en Linux/macOS
   (`brew install libgit2`, `apt install libgit2-dev`).
2. `-DCASI_VENDOR_LIBGIT2=ON` → `FetchContent` del tag fijado, build estático. Es el modo de los
   releases prebuilt y del CI de Windows.

Presets en `CMakePresets.json`: `dev` (Debug + ASan/UBSan), `release` (RelWithDebInfo + LTO),
`static` (vendored, estático), `windows` (MSVC + vcpkg) — este último se agrega recién en la
fase 7.

Bandera de compilación: C11, `-Wall -Wextra -Werror` en CI, `-D_FILE_OFFSET_BITS=64`.

### 7.3 SSH: preferir el transporte exec de OpenSSH

libgit2 ofrece dos transportes SSH: libssh2 embebido, o ejecutar el `ssh` del sistema
(`core.sshcommand` / `$GIT_SSH`). **casi prefiere el exec de OpenSSH cuando está disponible**,
porque respeta `~/.ssh/config` y `known_hosts` — y toda la config SSH del usuario ya vive ahí
(alias `nemo-pc`, `mac`, claves `id_ed25519` registradas en GitHub y Bitbucket). Con libssh2 esos
alias no resuelven y habría que reimplementar el parser de `ssh_config`.

Fallback a libssh2 con `git_credential_ssh_key_from_agent` si el build no trae exec. `casi doctor`
reporta qué transporte está activo. **Fijar libgit2 ≥ 1.9.2**: las versiones previas tienen un
bug de ejecución arbitraria de comandos en el exec SSH.

> **Nota post-implementación:** en la práctica, la instalación más probable NO trae `exec`.
> Homebrew's libgit2 1.9.7 se compila con `-DUSE_SSH=ON` (libssh2) y Ubuntu/Debian están en
> libgit2 1.7 (sin transporte `exec`, que llegó en 1.8.0). El piso de versión se corrigió a
> **1.7** con una ventana de riesgo acotada a `[1.8.0, 1.9.2)` en vez de exigir 1.9.2 a secas —
> ver HANDOFF.md y `docs/DESIGN.md`.

---

## 8. Testing

**Unit** (CTest + `utest.h` vendorizado, sin dependencias de build):

- `chunk`: límites exactos, línea más larga que el chunk, archivo vacío, sin `\n` final, append
  puro (prefijo estable), truncado, byte cambiado en el medio.
- `roots`: prefijo más largo con roots solapados, root sin match, barra final, root que es prefijo
  de otro, `$HOME` implícito, ida y vuelta normalizar→desnormalizar.
- `encoding`: la tabla de §1.2 como golden test, más el caso de colisión (dos paths → misma
  carpeta) para verificar que se detecta y no se decodifica.
- `jsonl`: escáner de líneas, validación tras sustitución, línea corrupta.
- `index`: hit/miss del stat-cache, hasheo de solo la cola.
- `sync`: matriz completa de la tabla de §6.2 sobre listas de OIDs sintéticas.

**Integración** (drivers en C, sin red, `file://` como remote):

- `two_machines`: dos `$CASI_HOME` + `$CASI_CLAUDE_HOME` aislados con roots distintos → push de A,
  pull en B → asserts de que el archivo de B es byte-idéntico salvo los paths reescritos, de que
  cada `cwd` quedó traducido, y de que el nombre de carpeta se re-codificó al layout de B.
- `large_session`: JSONL sintético de 200 MB → push, append de 100 KB, push → assert de que el
  segundo push transfiere < 1 MB (contando objetos del pack). **Este es el test que valida la
  decisión de chunking.**
- `conflict`: divergencia forzada → assert de exit `3`, archivo local intacto, copia apartada
  presente, y que `--theirs` invierte correctamente.
- `idempotence`: push dos veces → el segundo no crea commit ni objetos.
- `roundtrip`: A → B → A, assert de que A vuelve a su estado byte-exacto (la normalización no
  pierde información).
- `ssh_remote`: job opcional de CI con `sshd` local, para validar el transporte real.

**Fixtures:** sintéticas y generadas por `tools/gen_fixtures.c`. Las sesiones reales tienen
contenido de clientes (bookit/nemogroup, Jira) y **no se commitean**; `tools/scrub.c` existe solo
para uso local del desarrollador.

**CI** (`.github/workflows/ci.yml`): matriz Linux (gcc, clang) × macOS (clang), cada uno con un
job normal y uno ASan+UBSan; job de valgrind en Linux; job de `-Werror`. Windows entra en la
fase 7.

---

## 9. Distribución (solo enunciado)

- **Homebrew:** tap propio `francodipietro/tap/casi`, fórmula con `depends_on "libgit2"`. Camino
  principal en macOS y viable en Linuxbrew.
- **Binarios prebuilt:** GitHub Releases desde CI — Linux `x86_64`/`aarch64` estáticos contra musl
  (libgit2 vendorizado, sin dependencias de runtime), macOS universal binary firmado ad-hoc.
  Checksums SHA-256 y attestation de build.
- **Linux nativo:** PKGBUILD para AUR; `.deb` vía `cpack` para Debian/Ubuntu. Repos oficiales solo
  si el proyecto gana tracción.
- **Desde fuente:** `cmake --preset release && cmake --build build && sudo cmake --install build`.
- **Licencia:** AGPL-3.0-only. `LICENSE` ya está en el repo. Falta el header SPDX
  (`SPDX-License-Identifier: AGPL-3.0-only`) en cada `.c`/`.h` y la sección de licencia del README.
  La cláusula de red de la AGPL es en la práctica inerte para una CLI local, pero es la elección
  explícita del proyecto y define los términos si alguien ofrece casi como servicio.

---

## 10. Windows: qué facilita y qué complica el porte

Decisiones tomadas **ahora**, sin esfuerzo extra, que abaratan el porte después:

| Decisión | Por qué ayuda en Windows |
|---|---|
| Repo bare, sin worktree ni checkout | Evita de raíz `core.autocrlf`, bit de modo Unix, symlinks y la semántica de checkout de libgit2 — los cuatro dolores clásicos |
| Todo I/O en binario explícito | Nunca aparece la traducción `\n` → `\r\n` del modo texto de CRT |
| Paths canónicos siempre con `/` | La forma canónica es idéntica en las tres plataformas; solo la capa `fs_` convierte a nativo |
| `<pid>` = SHA-256 hex | Sin caracteres ilegales (`:` `*` `?`), sin nombres reservados (`CON`, `NUL`), sin límite de 260 chars |
| Toda syscall detrás de `util/fs.c` | El porte toca un archivo, no cien |
| `casi_home()` en vez de `getenv("HOME")` | El fallback a `%USERPROFILE%`/`%APPDATA%` es un cambio de una función |
| Reemplazo atómico envuelto desde el día uno | `rename()` POSIX vs `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` — el sitio de llamada no cambia |
| UTF-8 en todo el core | La conversión a UTF-16 queda confinada al borde de syscall |
| Gancho de escapado en el sustituidor de paths | El caso `"C:\\Users\\..."` ya tiene lugar reservado |

Lo que **sí** costará trabajo real cuando llegue:

- **Sensibilidad a mayúsculas.** Linux distingue, macOS no (por defecto), Windows tampoco. El
  matcher de roots necesita modo de plegado configurable: comparación sensible para datos
  canónicos, insensible solo al sondear el filesystem local.
- **La codificación de directorio de Claude Code en Windows es desconocida.** `C:\Users\franco\dev\app`
  → ¿qué produce? Hay que sondearlo empíricamente; queda contenido dentro del provider.
- **Paths escapados en JSON.** `\` es escape en JSON, así que la sustitución de bytes necesita
  manejar la variante escapada además de la cruda (gancho ya previsto en §5.3).
- **Rutas largas.** Puede hacer falta el prefijo `\\?\` para superar el límite de 260 caracteres.
- **libgit2 + SSH en Windows.** El exec de OpenSSH depende del OpenSSH de Windows; el fallback a
  libssh2 pesa más ahí.

---

## 11. Fases de implementación

**Fase 0 — Scaffolding.** Repo, README, `CONTRIBUTING.md`, headers SPDX AGPL-3.0, CMake +
presets, CI Linux/macOS, `util/` (fs, buf, str, error, log), config sobre `git_config_*`, harness
de tests, `casi --version`. *Verificable:* compila en Linux y macOS, CI verde, `casi --version`
imprime.

**Fase 1 — MVP push/pull.** Provider Claude Code (`discover` + `read_item` + `materialize` +
`encode_project_dir`), chunker, constructor de árbol, `init`/`status`/`push`/`pull`/`sync`, regla
de prefijo, branch por máquina, remotes `file://` + SSH + HTTPS. Normalización mínima: `$HOME` y
un root único, más re-codificación del directorio de proyecto — **imprescindible ya en el MVP**,
porque las máquinas reales tienen `/home/fdipietro` vs `/home/nemo` vs `/Users/fdipietro` y sin
eso las sesiones no abren. Sin cifrado, sin borrados, sin stat-cache. *Verificable:* test
`two_machines` + push/pull real entre la Mac y `nemo-pc` contra un repo privado de GitHub.

**Fase 2 — Normalización completa de paths.** Tabla de named roots, prefijo más largo, roots
compartidos vía `casi.json`, detección de roots sin mapear con exit `5`, `casi doctor`, `memory/`
por proyecto, sidecar `subagents/`. *Verificable:* una sesión creada en el layout anidado de esta
laptop abre correctamente en el layout aplanado de `nemo-pc` — el caso que el script actual no
modela.

**Fase 3 — Robustez y performance.** Stat-cache con hasheo de solo la cola, apartado de conflictos
+ `casi conflicts` + `pull --theirs`, `--dry-run`, `--porcelain`, `gc`/repack, manejo de
interrupciones (escrituras atómicas, sin estado a medias). *Verificable:* test `large_session`
pasa, `casi status` sobre los 390 MB reales corre en menos de un segundo.

**Fase 4 — Cifrado opt-in.** libsodium, cifrado convergente por chunk (nonce derivado del
contenido, para que la dedup y la regla de prefijo sigan funcionando byte a byte), componentes de
path como HMAC-SHA256 para no filtrar nombres de proyectos, clave fuera del remote
(`crypto.keyfile` o llavero del SO), modo fijado en `casi.json` al hacer `init`. *Verificable:*
un clon del remote sin la clave no revela ni nombres de proyecto ni contenido; con la clave, el
roundtrip es byte-exacto.

**Fase 5 — Distribución y apertura.** Releases prebuilt desde CI, tap de Homebrew, PKGBUILD,
`.deb`, `docs/FORMAT.md` (el formato del árbol como contrato estable), repo público.

**Fase 6 — Borrados y `casi forget`.** Tombstones con 3-way contra `refs/casi/base`, `--prune`
explícito, `pull` sigue sin borrar nunca por defecto.

**Fase 7 — Windows.** Completar `util/fs.c`, plegado de mayúsculas en roots, sondeo de la
codificación de directorio de Claude Code en Windows, escapado de `\` en el sustituidor, preset
MSVC + vcpkg, job de CI en Windows.

**Fase 8 — Segundo provider.** Cursor o Codex CLI, como validación de que el seam de §2.3 aguanta.
Si obliga a tocar `core/`, la abstracción estaba mal y se corrige ahí.

---

## Verificación end-to-end

En cada fase, además de `ctest --output-on-failure`:

1. `casi init --remote <bare local>` en dos `$CASI_HOME` aislados con roots distintos.
2. Sesión sintética en la máquina A → `casi push`.
3. `casi pull` en B → abrir con `claude --resume <sid>` y confirmar que la sesión levanta con los
   paths de B.
4. Continuar en B → `casi push`; `casi pull` en A → confirmar que solo viajó el chunk de cola
   (`git -C repo.git count-objects -v` antes y después).
5. Divergencia forzada → confirmar exit `3`, archivo local intacto y copia apartada.
6. Contra el dataset real de 390 MB: `casi status` sub-segundo, primer push completo, segundo push
   incremental.

El criterio de aceptación de la fase 2 es reemplazar `claude-sync.sh` en las tres máquinas —
mismo resultado, sin exigir que estén en la misma red al mismo tiempo.
