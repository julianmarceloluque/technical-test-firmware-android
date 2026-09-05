# Prueba técnica — Desarrollador Firmware + Android

**Control Global · Unidad Vending Control**
Candidato: **Julián Marcelo Luque**

Implementación del lado *slave* del protocolo VCP-1 (E1) y las tres respuestas
escritas (E2, E3, E4).

---

## Por dónde empezar

| Si querés… | Leé |
|---|---|
| entender cómo está armado todo y cómo viajan los datos | **[docs/00_ROADMAP.md](docs/00_ROADMAP.md)** |
| las decisiones de diseño de E1 y la lista de ambigüedades | [docs/01_E1_decisiones.md](docs/01_E1_decisiones.md) |
| el vector del Anexo B analizado byte por byte | [docs/02_E1_analisis_vector.md](docs/02_E1_analisis_vector.md) |
| E2 — consistencia ante corte de energía | [docs/03_E2_energia.md](docs/03_E2_energia.md) |
| E3 — diagnóstico del incidente en campo | [docs/04_E3_incidente.md](docs/04_E3_incidente.md) |
| E4 — plan de incorporación de Android | [docs/05_E4_android.md](docs/05_E4_android.md) |
| las salidas de los programas, ya generadas | [salida/](salida/) |

---

## Cómo compilar y correr

No hace falta instalar nada más que un compilador de C. El código es C11
portable, sin dependencias externas.

### Windows (PowerShell) — sin `make`

```powershell
cd firmware
.\build.ps1              # compila los 3 binarios en build\
.\build.ps1 -Test        # compila y corre las pruebas
.\build.ps1 -Run         # corre el banco con el vector del Anexo B
.\build.ps1 -Salidas     # regenera salida\
```

El script busca `gcc` en el `PATH` y, si no lo encuentra, en las rutas típicas
de MinGW/MSYS2/LLVM. Si la política de ejecución lo bloquea:
`powershell -ExecutionPolicy Bypass -File .\build.ps1`.
Desde `cmd` también sirve `build.bat`.

### Linux / macOS / MSYS2

```bash
cd firmware
make          # compila
make test     # corre las pruebas
make run      # corre el banco con el vector del Anexo B
```

### A mano, en una línea

```bash
gcc -std=c11 -Wall -Wextra -Ifirmware/lib/vcp/include -Ifirmware/port/pc \
    firmware/lib/vcp/src/*.c firmware/port/pc/*.c firmware/app/main_vcp1.c \
    -o vcp1
./vcp1 Anexos/stream_vcp1.txt
```

> Compila limpio con `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
> -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes`. Probado con
> **gcc 6.3.0 (MinGW)** en Windows 11.

---

## Los tres programas

### 1. `vcp1` — banco de pruebas del receptor (E1.7)

Lee el vector en hexadecimal por **archivo, argumento o entrada estándar** (no
está embebido en el código) y muestra cada trama detectada, qué se decidió, qué
trama se habría transmitido y cada error diferenciado por tipo.

```powershell
.\build\vcp1.exe ..\Anexos\stream_vcp1.txt
.\build\vcp1.exe --hex "02 0A 00 10 F7 03"
.\build\vcp1.exe --resync descarte ..\Anexos\stream_vcp1.txt   # la comparación
.\build\vcp1.exe --sin-plazo ..\Anexos\stream_vcp1.txt
.\build\vcp1.exe --byte-ms 0 ..\Anexos\stream_vcp1.txt         # sin simular tiempo
.\build\vcp1.exe -q ..\Anexos\stream_vcp1.txt                  # solo el resumen
.\build\vcp1.exe --help
```

Por entrada estándar, desde `cmd` o Linux:

```
type ..\Anexos\stream_vcp1.txt | vcp1.exe      # cmd
./vcp1 < ../Anexos/stream_vcp1.txt             # bash
```

> La tubería de PowerShell 5.1 re-codifica el texto antes de pasarlo a un
> ejecutable nativo, así que ahí conviene pasar el archivo como argumento. El
> programa avisa con un mensaje claro si le llega basura. Acepta archivos con
> BOM, prefijos `0x`, comas y comentarios con `#` o `//`.

### 2. `demo_venta` — la sesión de venta (E1.12)

El vector del Anexo B no trae ningún `VEND_REQUEST` válido (el único viene con
el CRC roto a propósito), así que el camino más interesante del ejercicio se
demuestra acá: una expendedora simulada arma tramas reales y las entrega byte a
byte a 9600 8N1.

```powershell
.\build\demo_venta.exe        # los 7 escenarios
.\build\demo_venta.exe 1      # venta completa
.\build\demo_venta.exe 6      # el cobro que nunca resuelve (E2.8)
```

### 3. `tests` — pruebas propias (E1.8)

**179 verificaciones, 0 fallas.** Devuelve código de salida distinto de cero si
algo falla.

---

## Resultado con el vector del Anexo B

**6 tramas válidas, 4 errores, 5 bytes de ruido.** Dos de las 6 solo aparecen
con la política de re-sincronización por reproceso.

| # | offset | trama | ¿se responde? |
|---|---|---|---|
| 1 | 3..8 | `STATUS_REQ` a `0x0A` | **sí** — `STATUS_RESP` |
| 2 | 9..14 | `STATUS_REQ` a `0x0B` | no — otra dirección |
| 3 | 15..26 | `LOG_EVENT` con `02 03` en el payload | **sí** — `ACK` |
| 4 | 42..49 | `VEND_APPROVE` (recuperada, a tiempo) | no — sentido inverso |
| 5 | 58..65 | `VEND_SUCCESS` (recuperada, 8 ms tarde) | no — **fuera de plazo** |
| 6 | 66..73 | `LOG_EVENT` broadcast | no — broadcast |

| Errores | CRC ×2 · LEN ×1 · framing ×1 |
|---|---|

| Política | tramas detectadas |
|---|---|
| `reproceso` (por defecto) | **6** |
| `descarte` | 3 |

El detalle de cada una está en
[docs/02_E1_analisis_vector.md](docs/02_E1_analisis_vector.md).

---

## Estructura

```
Anexos/          lo que vino con la prueba, sin modificar
firmware/
  lib/vcp/       biblioteca de protocolo: no sabe si corre en PC o en un micro
  port/pc/       implementación del "entorno provisto" (UART + medio de pago)
  app/           los dos programas ejecutables
  tests/         pruebas propias
docs/            los documentos escritos
salida/          salidas de los programas, ya generadas
```

El día que esto vaya al micro real no se toca ni una línea de `lib/vcp/`: se
escribe un `port/stm32/`.

---

## Estado de la entrega

| Ejercicio | Estado |
|---|---|
| E1 — receptor y respondedor VCP-1 | completo (E1.1 a E1.14) |
| E2 — consistencia ante corte de energía | completo (escrito) |
| E3 — diagnóstico del incidente | completo (escrito) |
| E4 — plan de Android | completo (escrito) |

**Lo que no está implementado, y por qué:** la memoria no volátil de E2 es un
diseño escrito, no código. E2 es un ejercicio escrito y agregar una capa de NVM
simulada habría sumado superficie sin agregar evidencia. Lo que sí hice fue
**anclar el documento al código**: los cinco puntos de escritura están marcados
en `firmware/lib/vcp/src/vcp_session.c` con comentarios `[E2: PUNTO DE
ESCRITURA NVM #n]`, así que el diseño se puede señalar sobre la línea exacta.

**Limitación conocida del receptor**, documentada y con test propio: ante un
stream continuo adversarial sin ninguna pausa de 5 ms, la ventana de
re-sincronización puede quedar corrida y no encontrar una trama válida
posterior. No hay desborde ni corrupción, y se recupera con la primera pausa —
que en un bus half-duplex donde el master espera respuesta siempre existe. El
razonamiento y la alternativa están en
[docs/01_E1_decisiones.md](docs/01_E1_decisiones.md), sección 2.

---

## Tiempo dedicado

Aproximadamente el estimado del enunciado (R1). El grueso se fue en dos cosas:
analizar el vector del Anexo B a mano (calcular los CRC uno por uno antes de
escribir código, para tener un oráculo independiente del programa) y decidir la
política de re-sincronización con sus consecuencias sobre el plazo de 5 ms.

---

## Declaración de uso de asistentes de IA (regla R3)

Usé **Claude (Anthropic)** como asistente durante el desarrollo. Concretamente:

- **Revisión y contraste de mi lectura del enunciado**: discutir las
  ambigüedades que había detectado y verificar que no se me estuviera pasando
  ninguna. La lista final de casos no especificados (NE‑01 a NE‑17) y el
  criterio adoptado en cada uno son decisiones mías.
- **Verificación de los cálculos de CRC del Anexo B.** Los calculé primero a
  mano para tener un oráculo independiente y usé el asistente para contrastar
  el resultado antes de escribir el código.
- **Redacción**: pulido de la documentación en `docs/` y de los comentarios,
  para que quedaran legibles y explicaran el *por qué* y no solo el *qué*.
- **Apoyo en la escritura de código repetitivo**: el banco de pruebas, el
  formateo de la salida y parte del andamiaje de los tests.

No usé asistentes para: el diseño de la arquitectura por capas, la política de
re-sincronización por reproceso ni sus consecuencias sobre el plazo de 5 ms, la
máquina de estados de la sesión de venta, ni las respuestas de E2/E3/E4, que
son razonamiento propio.

**Todo el código de la entrega lo entiendo, lo puedo explicar línea por línea y
lo puedo modificar en vivo** (regla R4).
