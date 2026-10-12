# Handoff de usabilidad — primera prueba real de casi

Fecha: 2026-09-20. Este documento acompaña a `HANDOFF.md`, pero tiene un
propósito distinto: registra una prueba de primera adopción con el dueño del
proyecto y los bloqueos de producto que aparecieron. Está escrito para que otro
asistente de código pueda retomar el trabajo sin convertir observaciones reales
en supuestos de arquitectura.

No sustituye la evidencia técnica de `HANDOFF.md`, `docs/DESIGN.md` o los
tests. Tampoco declara que los problemas reportados sean defectos del algoritmo
de sincronización: varios son fallas de onboarding, visibilidad y experiencia
que hacen que las garantías técnicas no alcancen para usar el producto.

## Decisión del dueño del proyecto

Franco considera bloqueante el flujo actual para adoptar casi él mismo. Sus
observaciones, expresadas durante la primera configuración real, son:

- Copiar manualmente la clave de cifrado entre equipos es un *deal breaker*.
  En su forma actual, "así no puede funcionar".
- Tener que encontrar o agregar remotes a mano no es una experiencia aceptable.
- La mayoría de las sesiones aparecen como divergentes.
- Casi le pide tomar decisiones de conflicto sin información suficiente para
  entender qué cambió, de qué equipo vino o cuál copia conviene conservar.

La prioridad inmediata no es agregar otra capacidad de sync. Es rediseñar y
validar el camino de adopción hasta que una persona pueda confiar en él sin
entender refs de Git, claves de archivos ni UUIDs de sesiones.

## Estado comprobado de la prueba

### Máquina macOS

- Casi fue instalado por Homebrew (`v0.1.1`). El canal de instalación funcionó.
- Se creó un remoto privado separado para sesiones:
  `git@github.com:francodipietro/casi-storage.git`.
- La primera invocación pegó por accidente un enlace en formato Markdown:

  ```sh
  casi init --remote [https://github.com/francodipietro/casi-storage](https://github.com/francodipietro/casi-storage)
  ```

  `casi init` aceptó la cadena literal y anunció éxito. No intentó conectarse;
  por eso el error de URL no apareció en ese momento. Luego se corrigió con la
  URL SSH pura.
- Se activó cifrado antes de que exista evidencia de un `push`:

  ```sh
  casi init \
    --remote git@github.com:francodipietro/casi-storage.git \
    --encrypt
  ```

  La clave se creó en `~/.local/share/casi/crypto.key` con permisos privados.
  No incluir su contenido en ningún documento, commit, issue o remoto.
- `casi doctor` devolvió:

  ```text
  backend: libgit2 1.9.7 [https, ssh: exec]
  remote: git@github.com:francodipietro/casi-storage.git
  remote: reachable
  roots: no shared configuration yet
  ```

  Esto verifica lectura/autenticación SSH y es esperable antes del primer
  `push`: todavía no existe `refs/heads/casi/config`.
- Se creó correctamente un backup local de `~/.claude`:

  ```text
  /Users/fdipietro/claude-bak/casi-claude-backups/Francos-MacBook-Air/
  snapshots/20260920T012347Z-52529
  ```

  El backup es incremental por hard links y no borra snapshots anteriores. Está
  en el mismo disco de la Mac, por lo que protege contra una mala operación de
  sync pero no contra pérdida del disco.
- Al cierre de este registro no hay salida aportada de `casi push --dry-run`,
  `casi push` ni de una prueba en Ubuntu. No asumir que alguna sesión llegó al
  remoto.

### Estado del repositorio de código

- `main` está en `f5a80fc` / release `v0.1.1`.
- La rama de prueba `feat/claude-backup-script` contiene `849d069`
  (`feat(backup): add Claude snapshot script`). Agrega
  `scripts/backup-claude.sh` y la guía de backup/restauración en `README.md`.
- Ese script todavía **no** está en el paquete Homebrew de `v0.1.1`; un usuario
  de Homebrew necesita un checkout del repositorio para usarlo. Es otra
  discontinuidad de onboarding a resolver si el script pasa a ser parte del
  flujo recomendado.

## Problemas de usabilidad observados

### 1. Configurar un remoto es una tarea de infraestructura, no onboarding

El diseño actual es deliberadamente remoto-agnóstico: `casi init --remote <url>`
acepta cualquier transporte Git. Esa flexibilidad técnica trasladó al usuario
todo el trabajo de elegir o crear un remoto, encontrar su URL, elegir SSH o
HTTPS y resolver autenticación.

El caso real demostró dos consecuencias:

- `init` sin `--encrypt` sólo guarda la URL: no hace fetch ni prueba la
  autenticación. Una URL inválida, incluido un enlace Markdown pegado por
  accidente, recibe un mensaje de éxito.
- La confirmación posterior requiere conocer y ejecutar `casi doctor`, un
  comando oculto de la ayuda principal. El usuario no recibe un siguiente paso
  guiado ni una explicación de que aún no se verificó acceso de lectura o
  escritura.

La queja "los remotes no aparecen y hay que agregarlos a mano" debe tomarse
literalmente como requisito de producto, no como falta de documentación. Casi
no descubre remotos existentes, no crea uno, no lista repositorios privados ni
ofrece un selector.

La autenticación HTTPS también es poco apta para primera adopción: el callback
de credenciales no pide ni almacena usuario, token o passphrase. El camino que
funcionó fue SSH ya configurado fuera de casi. Para una instalación Homebrew
con backend `libssh2`, incluso los alias de `~/.ssh/config` pueden no funcionar;
la release observada aquí usó `exec`, pero no se debe prometer eso para todos
los builds.

**Dirección a decidir, no implementar a ciegas:**

- Un onboarding GitHub explícito (por ejemplo, detectar `gh auth status`,
  listar/crear un repositorio privado `casi-storage` y confirmar la cuenta), o
- un asistente genérico de remotos que valide URL, lectura y escritura antes de
  declarar terminada la inicialización, o
- ambas rutas, con GitHub como camino simple y Git genérico como avanzado.

La UX debe mostrar una prueba de conectividad real y explicar el siguiente
paso. No debe anunciar "store ready" como si eso significara que el remoto está
listo si todavía no hubo una conexión.

### 2. La entrega manual de la clave de cifrado invalida el producto para este usuario

El modelo actual es técnicamente deliberado: `casi init --encrypt` genera una
clave local aleatoria de 32 bytes, modo `0600`, y jamás la escribe en Git. Para
un segundo equipo, el usuario debe copiar esa clave por un canal externo y
ejecutar `casi init --encrypt --keyfile <ruta>`.

Esto está cubierto por tests y protege contra dejar una clave por accidente en
el remoto. Pero es inaceptable como flujo de producto para Franco. Exigirle
saber qué archivo copiar, cómo transportarlo, dónde guardarlo, con qué permisos
y cómo recuperarlo significa que el cifrado no es adoptable.

No resolverlo rebajando silenciosamente la seguridad. Antes de tocar código,
definir un modelo de amenaza y elegir una experiencia coherente. Opciones a
evaluar:

1. Emparejamiento entre equipos: un dispositivo ya autorizado comparte la
   clave cifrada con una clave efímera del nuevo dispositivo (QR, código corto o
   canal local/SSH controlado por el usuario).
2. Clave de recuperación con passphrase: el remoto privado guarda una copia de
   la clave maestra envuelta con una KDF robusta; el usuario ingresa una frase
   de recuperación al sumar un equipo. Hay que diseñar límites de intentos,
   parámetros de KDF y recuperación de cuenta.
3. Integración con credenciales del proveedor elegido: por ejemplo, un secreto
   administrado por la cuenta GitHub. Esto simplifica UX pero cambia la frontera
   de confianza y no sirve para todo remoto Git.
4. Un modo inicialmente no cifrado, explícito y bien explicado para un remoto
   privado, con migración segura posterior. No satisface por sí solo el deseo
   de cifrado transparente, pero es preferible a fingir que la clave manual es
   una experiencia normal.

El próximo asistente debe proponer alternativas con trade-offs y pedir una
decisión de Franco. No elegir una sin autorización: cambia confidencialidad,
recuperación y dependencia de proveedores.

### 3. Conflictos masivos sin diagnóstico accionable

Franco reportó que la gran mayoría de sus sesiones aparecen como divergentes.
No se adjuntó la salida de `status`/`pull`, por lo que **no está reproducido**
ni se conoce aún si son divergencias reales, una migración desde la herramienta
anterior, una diferencia de normalización, sesiones reanudadas en paralelo o
un bug. No afirmar una causa sin capturar datos seguros y reproducibles.

Aunque cada divergencia fuera legítima, la UX actual no permite tomar una
decisión informada:

- `casi status` sólo muestra un conteo agregado de conflictos. Además usa la
  última información fetchada; no actualiza el remoto por sí mismo.
- `casi pull` detecta la divergencia, conserva la copia local y estaciona la
  remota. Su informe útil se limita esencialmente a un UUID de sesión y una ruta
  de archivo bajo `conflicts/`.
- `casi conflicts` sólo lista rutas de copias estacionadas. No muestra proyecto,
  máquinas de origen, timestamps, relación con la base común, tamaño,
  antigüedad, último mensaje, ni un diff/resumen.
- `casi pull --theirs <session-id>` puede elegir la copia remota, pero exige que
  el usuario decida a partir de ese UUID. Antes guarda la local, lo cual es una
  buena garantía de seguridad, pero no resuelve la falta de criterio.

Esto transforma una política técnicamente segura de "no sobrescribir" en una
cola de conflictos que nadie puede resolver con confianza.

**Primero investigar, después diseñar:**

1. Capturar una salida de `casi status --porcelain`, `casi pull --dry-run` y,
   si fuera seguro, IDs y metadatos mínimos de una muestra de divergencias. No
   subir transcripciones reales al repositorio.
2. Clasificar para cada caso: sesión ausente, local adelante, remota adelante,
   prefijo común, divergencia real, fallo de normalización o estado heredado.
3. Confirmar si la cantidad reportada por el usuario se produce antes o después
   del primer fetch/pull y desde qué máquinas/fuentes vienen las ramas.

**Criterio mínimo para una nueva UX de conflictos:** una lista legible con
proyecto, nombre o CWD seguro de mostrar localmente, id corto, equipo de origen,
fecha/tamaño/última actividad de ambos lados y explicación de la relación. Debe
existir una vista previa local de los dos finales o un diff seguro, y acciones
que dejen claro qué se conservará y dónde quedará la otra copia. La operación
por UUID puede permanecer como modo avanzado, no como interfaz principal.

### 4. Roots, configuración compartida y estados intermedios son invisibles

`casi doctor` informó correctamente `roots: no shared configuration yet`.
Para quien conoce la implementación significa "todavía no hubo primer push";
para un usuario nuevo parece que falta una configuración obligatoria.

Los named roots son necesarios para layouts distintos entre macOS y Ubuntu,
pero hoy se declaran manualmente como `casi config root.<nombre>.path <ruta>`.
La raíz implícita `~` cubre casos simples, aunque ese hecho no aparece como una
decisión clara durante onboarding. El primer push publica la lista compartida
de nombres de roots; después cada equipo debe mapearlos localmente.

El producto necesita detectar y explicar el caso simple, proponer roots a partir
de las sesiones encontradas y diferenciar visualmente:

- no configurado todavía;
- configurado y remoto vacío;
- remoto accesible pero sin permisos de escritura;
- roots requeridos que no tienen mapping en esta máquina;
- listo para un primer push seguro.

### 5. Instalación y documentación no completan la tarea

Homebrew instaló la CLI correctamente. Sin embargo:

- El release macOS ofrece un `tar.gz` universal, no un `.dmg`. Para una CLI,
  Homebrew es el canal natural; el `.dmg` no es el problema central.
- El backup recomendado se agregó después de `v0.1.1`, sólo a una rama de
  prueba. Un usuario recién instalado no lo recibe junto con la herramienta.
- El README aún dice "Phase 4 is complete" y deja Phase 5 sin estado, aunque
  `v0.1.1` y el tap Homebrew existen. `HANDOFF.md` también conserva texto de
  antes del release final. La documentación no describe un flujo completo,
  probado y conciso de "instalé, conecté mi cuenta, veo qué se sincronizará,
  hago un primer sync seguro y agrego otro equipo".

Actualizar la documentación es necesario, pero no reemplaza corregir el flujo:
una guía más larga no vuelve aceptables una clave manual o decisiones de
conflicto opacas.

## Hechos técnicos relevantes para el próximo asistente

| Área | Implementación actual | Consecuencia UX |
|---|---|---|
| `init` plano | Crea/abre el repo bare y guarda `origin`; no hace fetch. | Una URL o auth inválida se descubre tarde. |
| `init --encrypt` | Hace fetch, requiere remoto, genera/carga la clave local y fija modo crypto. | Es el único `init` que prueba lectura, pero sumar otro equipo exige copiar clave. |
| Credenciales | SSH: claves convencionales/agent o backend `exec`; HTTPS no solicita token/contraseña. | El usuario debe entender SSH o preparar secretos fuera de casi. |
| Store remoto | Un ref por máquina y `refs/heads/casi/config` para roots/excludes. | Escala técnicamente, pero el usuario no ve un modelo mental ni un estado de preparación. |
| Conflicto | Regla de prefijo de chunks; no se sobrescribe en silencio; remoto se estaciona. | Seguro, pero no explicable ni resoluble desde la CLI actual. |
| `status` | Resume números y usa el último fetch. | No sirve como tablero de decisión ni garantiza que la imagen remota esté actualizada. |
| Backup | `rsync` snapshots con hard links en la rama de prueba. | Protege el primer ensayo, pero no está distribuido con Homebrew todavía. |

Archivos de entrada recomendados:

- `src/cmd/cmd_init.c`: secuencia de init y condición especial de `--encrypt`.
- `src/core/repo.c`: callbacks de credenciales SSH/HTTPS y fetch/push.
- `src/cmd/cmd_status.c`: resumen agregado y estado basado en último fetch.
- `src/cmd/cmd_pull.c`: detección, parking y selección manual de conflictos.
- `src/cmd/cmd_conflicts.c`: lista de rutas sin metadatos.
- `src/core/crypto.c`, `src/util/paths.c`, `docs/DESIGN.md`: lifecycle de clave
  y límites de seguridad.
- `tests/integration/encryption.sh`, `conflict.sh`, `two_machines.sh`: garantías
  ya cubiertas que no se deben romper al mejorar UX.

## Prioridad propuesta para retomar

1. **No usar las sesiones reales como banco de pruebas de una migración masiva.**
   Mantener el backup; usar una copia aislada de `~/.claude` y un remoto de
   prueba para reproducir las divergencias con telemetría local/redactada.
2. **Definir la experiencia de clave cifrada con Franco.** Es una decisión de
   producto y seguridad, no una mejora menor de CLI.
3. **Investigar las divergencias reales y construir observabilidad antes de
   ofrecer resolución.** La primera entrega debería poder explicar cada estado
   sin modificar sesiones.
4. **Rediseñar `init` como onboarding verificable.** Debe validar remoto y
   mostrar claramente autenticación, permisos y qué falta.
5. **Sólo entonces automatizar/provisionar remotos y roots.** El mecanismo
   técnico depende de las decisiones previas de proveedor y cifrado.
6. **Cerrar distribución/documentación.** Incluir el backup si sigue siendo
   recomendado, actualizar estado de fases y publicar un quickstart validado.

## Criterios de aceptación desde la perspectiva del dueño

No considerar la experiencia lista porque el sync no pierda datos. Antes de
pedir a Franco que vuelva a usar casi con sus sesiones reales, el flujo debe
permitirle responder "sí" a todo esto:

- Puedo instalarlo, conectarlo a un remoto correcto y verificar lectura/escritura
  sin saber cómo funcionan Git, SSH, URLs o refs.
- Si activo cifrado, agregar mi segunda máquina no requiere copiar ni administrar
  manualmente un archivo de clave; entiendo cómo recuperar acceso si pierdo un
  equipo.
- Antes del primer envío veo qué sesiones se enviarán, qué queda excluido y por
  qué.
- Si hay una divergencia, entiendo qué dos versiones existen, de cuándo son, en
  qué proyecto se usaron y qué hará cada opción, sin decidir por UUID.
- Una elección equivocada no destruye una copia y sé dónde encontrar la otra.
- El camino normal entre macOS y Ubuntu está documentado, distribuido y fue
  probado de punta a punta con una instalación release, no sólo con binarios de
  desarrollo y repositorios temporales.

Hasta entonces, las garantías de atomicidad, parking de conflictos, chunking,
tests y cifrado siguen siendo valiosas como base; no equivalen a una herramienta
adoptable para este usuario.
