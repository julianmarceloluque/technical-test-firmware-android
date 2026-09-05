# Roadmap y flujo de comunicaciones

> Este es el documento para leer **primero**. Explica cómo está armado el
> proyecto, en qué orden conviene leer el código y cómo viajan los datos desde
> que un byte entra por la UART hasta que sale una respuesta.

---

## 1. El problema en una pantalla

Hay **dos buses** y el controlador tiene un rol distinto en cada uno:

```
        ┌──────────────────┐                      ┌─────────────────────┐
        │   EXPENDEDORA    │  ← bus serie VCP-1 → │    CONTROLADOR      │
        │    (MASTER)      │    9600 8N1          │      (SLAVE)        │
        │                  │    half-duplex       │       0x0A          │
        │  pregunta cada   │    multipunto        │                     │
        │     ~100 ms      │                      │  responde en < 5 ms │
        └──────────────────┘                      └──────────┬──────────┘
                 │                                            │
                 │  en la MISMA línea cuelgan:                │ acá el
                 ├── validador de billetes (0x0B?)            │ controlador
                 ├── monedero                                 │ es MASTER
                 └── otros medios de pago                     ▼
                     (todos escuchan todo)         ┌─────────────────────┐
                                                   │   MEDIO DE PAGO     │
                                                   │  (SLAVE)            │
                                                   │  tarda 50 a 900 ms  │
                                                   └─────────────────────┘
```

**La tensión central del ejercicio:** hay que contestarle a la expendedora en
**5 ms**, pero el medio de pago tarda **hasta 900 ms**. Son tres órdenes de
magnitud de diferencia. De ahí sale todo lo demás:

- no puede haber ninguna llamada bloqueante en el camino;
- la venta **no** se resuelve dentro de una respuesta: se resuelve a lo largo
  de varias interrogaciones del master;
- el estado de la venta tiene que sobrevivir entre tramas.

---

## 2. Mapa de carpetas

```
technical-test-firmware-android/
├── README.md                   ← índice de la entrega + cómo compilar
├── PT_Firmware_Vending_Enunciado.md
├── Anexos/                     ← lo que vino con la prueba, SIN tocar
│   ├── vcp.h                     (interfaz sugerida, se conserva original)
│   └── stream_vcp1.txt           (vector del Anexo B)
│
├── firmware/
│   ├── Makefile / build.ps1 / build.bat
│   │
│   ├── lib/vcp/                ← LA BIBLIOTECA DE PROTOCOLO
│   │   │                         no sabe que existe una PC ni un micro
│   │   ├── include/vcp/
│   │   │   ├── vcp.h             header paraguas + qué cambié del sugerido
│   │   │   ├── vcp_cfg.h         todas las constantes del protocolo
│   │   │   ├── vcp_crc.h         CRC-8
│   │   │   ├── vcp_frame.h       tipo de trama + construcción para emitir
│   │   │   ├── vcp_rx.h          receptor incremental (máquina de estados)
│   │   │   ├── vcp_session.h     máquina de estados de la venta
│   │   │   ├── vcp_app.h         tabla de respuestas del Anexo A.4
│   │   │   └── vcp_port.h        firmas del "entorno provisto"
│   │   └── src/
│   │       ├── vcp_crc.c
│   │       ├── vcp_frame.c
│   │       ├── vcp_rx.c          ← el corazón del ejercicio
│   │       ├── vcp_session.c
│   │       └── vcp_app.c
│   │
│   ├── port/pc/                ← IMPLEMENTACIÓN DEL ENTORNO PARA PC
│   │   ├── port_pc.h             controles del simulador
│   │   ├── port_uart_pc.c        vcp_uart_write() imprime en hexadecimal
│   │   └── port_pago_pc.c        medio de pago simulado y guionable
│   │
│   ├── app/                    ← PROGRAMAS EJECUTABLES
│   │   ├── main_vcp1.c           banco de pruebas del stream (E1.7)
│   │   └── main_demo_venta.c     demostración de la venta (E1.12)
│   │
│   ├── tests/                  ← PRUEBAS PROPIAS (E1.8)
│   │   ├── test_util.h / test_main.c   mini framework
│   │   ├── test_crc.c
│   │   ├── test_frame.c
│   │   ├── test_rx.c
│   │   └── test_app.c
│   │
│   └── build/                  ← binarios (generado)
│
├── docs/                       ← ESTE DOCUMENTO Y LOS ESCRITOS
│   ├── 00_ROADMAP.md             (estás acá)
│   ├── 01_E1_decisiones.md       entregable escrito de E1
│   ├── 02_E1_analisis_vector.md  el vector del Anexo B, byte por byte
│   ├── 03_E2_energia.md          ejercicio E2
│   ├── 04_E3_incidente.md        ejercicio E3
│   ├── 05_E4_android.md          ejercicio E4
│   └── 06_guia_de_estudio.md     recorrido para preparar la defensa
│
└── salida/                     ← salidas de los programas, ya generadas
```

### Por qué esta separación y no un solo archivo

| Capa | Depende de | Se puede probar sola | Cambia si… |
|---|---|---|---|
| `lib/vcp` | solo de `stdint.h`, `string.h` y de las **firmas** de `vcp_port.h` | sí, con datos en memoria | cambia el protocolo |
| `port/pc` | del sistema operativo | — | cambiamos de plataforma |
| `app` | de las dos anteriores | — | cambia lo que queremos mostrar |
| `tests` | de `lib` + `port` | — | agregamos casos |

El día que esto se lleve al micro real, **no se toca ni una línea de
`lib/vcp/`**: se escribe un `port/stm32/` con la UART y el lector de tarjetas
de verdad. Esa es la única razón por la que vale la pena separar.

---

## 3. Las cinco capas del firmware

```
   ┌──────────────────────────────────────────────────────────────────┐
   │  ISR de UART              (port)                                 │
   │  un byte + su marca de tiempo → FIFO                             │
   └───────────────────────────┬──────────────────────────────────────┘
                               │  vcp_rx_byte(rx, byte, t_byte, cb, ctx)
   ┌───────────────────────────▼──────────────────────────────────────┐
   │  vcp_rx.c   RECEPTOR                                             │
   │  ¿esto es una trama? ¿está sana? ¿desde dónde re-sincronizo?      │
   │  NO sabe qué significan los comandos.                            │
   └───────────────────────────┬──────────────────────────────────────┘
                               │  evento VCP_EV_FRAME (o de error)
   ┌───────────────────────────▼──────────────────────────────────────┐
   │  vcp_app.c   APLICACIÓN                                          │
   │  ¿es para mí? ¿ese comando me lo puede mandar el master?          │
   │  ¿el LEN cierra? ¿qué contesto? ¿llegué a tiempo?                │
   └───────────┬───────────────────────────────────┬──────────────────┘
               │                                   │
   ┌───────────▼──────────────┐      ┌─────────────▼──────────────────┐
   │  vcp_session.c  VENTA    │      │  vcp_frame.c  EMISIÓN          │
   │  máquina de estados que  │      │  vcp_build() arma la trama     │
   │  sobrevive entre tramas  │      │  y le pone el CRC              │
   │  NUNCA transmite         │      └─────────────┬──────────────────┘
   └───────────┬──────────────┘                    │
               │ pago_iniciar() / pago_estado()    │ vcp_uart_write()
   ┌───────────▼──────────────┐      ┌─────────────▼──────────────────┐
   │  port: medio de pago     │      │  port: UART                    │
   └──────────────────────────┘      └────────────────────────────────┘
```

**Regla que estructura todo:** la flecha hacia `vcp_uart_write()` sale
**únicamente** de `vcp_app_on_frame()`, y a esa función solo se la llama desde
el evento `VCP_EV_FRAME`. No hay ninguna otra ruta hacia la línea. Por eso el
requisito E1.11 ("nunca transmite por iniciativa propia") se cumple **por
construcción** y no por disciplina.

---

## 4. El lazo principal

Todo el firmware es este lazo. Son ocho líneas y no hay ni una espera:

```c
while (1) {
    uint32_t now = reloj_ms();

    /* 1) todo lo que haya llegado por la UART */
    while (uart_fifo_pop(&byte, &t_byte)) {
        vcp_rx_byte(&rx, byte, t_byte, on_evento, &app);   /* puede responder */
    }

    /* 2) que el timeout entre bytes pueda vencer si dejaron de llegar */
    vcp_rx_tick(&rx, now, on_evento, &app);

    /* 3) el lado master: cómo viene el cobro, vencimiento de plazos */
    vcp_app_poll(&app, now);                                /* NO transmite  */
}
```

Fijate en el detalle de `(1)`: la FIFO entrega el byte **y su marca de tiempo**.
La toma la interrupción en el momento de la recepción, no el lazo principal.
Gracias a eso, el plazo de 5 ms se mide desde que el byte entró de verdad, y si
el lazo principal se demoró, el propio código lo detecta y calla la respuesta
en vez de mandarla tarde.

---

## 5. Flujo de comunicaciones, caso por caso

### 5.1 Interrogación periódica (el 95 % del tráfico)

```
 MÁQUINA                                CONTROLADOR
    │                                        │
    │──── STATUS_REQ ───────────────────────▶│  02 0A 00 10 F7 03
    │                                        │  ├─ rx: STX..ETX ok, CRC ok
    │                                        │  ├─ app: addr 0x0A = mía
    │                                        │  ├─ app: ¿resultado de venta
    │                                        │  │        pendiente? no
    │                                        │  └─ app: STATUS_RESP
    │◀─── STATUS_RESP ───────────────────────│  02 0A 04 11 00 00 00 EB 38 03
    │        (dentro de 5 ms)                │     estado, reservado, temp
    │                                        │
    ⋮ cada ~100 ms                           ⋮
```

### 5.2 Venta completa (el caso que da forma al diseño)

```
 MÁQUINA                          CONTROLADOR                 MEDIO DE PAGO
    │                                 │                             │
    │─ VEND_REQUEST sel=24 $18.00 ───▶│                             │
    │                                 ├─ valida rangos              │
    │                                 ├─ pago_iniciar(1800) ───────▶│  (arranca,
    │                                 │                             │   no bloquea)
    │◀──────────── VEND_PENDING ──────│  "recibido, todavía no sé"  │
    │                                 │                             │
    │─ STATUS_REQ ───────────────────▶│                             │
    │◀─ STATUS_RESP (OCUPADO) ────────│  ← poll: pago_estado() ────▶│ EN_CURSO
    │                                 │                             │
    │─ STATUS_REQ ───────────────────▶│                             │
    │◀─ STATUS_RESP (OCUPADO) ────────│  ← poll: pago_estado() ────▶│ EN_CURSO
    │                                 │                             │
    │                                 │  ← poll: pago_estado() ────▶│ AUTORIZADO
    │                                 │     guarda txid, NO transmite│  txid=8720
    │                                 │                             │
    │─ STATUS_REQ ───────────────────▶│                             │
    │◀─ VEND_APPROVE txid=8720 ───────│  ← acá viaja el resultado   │
    │                                 │                             │
    │  [la máquina entrega el producto]                             │
    │                                 │                             │
    │─ VEND_SUCCESS txid=8720 ───────▶│                             │
    │◀──────────── ACK ───────────────│  sesión cerrada             │
```

Los tres puntos que hay que poder explicar de este diagrama:

1. **`VEND_PENDING` no es un "ok", es un "todavía no sé".** Es la única
   respuesta físicamente posible en 5 ms cuando el cobro tarda 250.
2. **El resultado del cobro no dispara ninguna transmisión.** Queda guardado y
   viaja en la respuesta a la siguiente pregunta del master (E1.11).
3. **El `VEND_APPROVE` queda "latcheado"** hasta que llegue la confirmación: si
   el master vuelve a preguntar, se le repite el mismo `txid`. Ver decisión
   NE-03 en `01_E1_decisiones.md`.

### 5.3 Trama para otro dispositivo del bus

```
 MÁQUINA                                CONTROLADOR (0x0A)      MONEDERO (0x0B)
    │                                        │                       │
    │──── STATUS_REQ a 0x0B ────────────────▶│───────────────────────▶│
    │                                        │  rx: la parsea entera  │
    │                                        │  app: addr ≠ 0x0A      │
    │                                        │  → cuenta y CALLA      │
    │◀──────────────────────────────────────────── STATUS_RESP ───────│
```

Se procesa igual (para no perder el encuadre del stream) pero **no se
transmite**: si contestáramos, chocaríamos con la respuesta del monedero y se
corromperían las dos.

### 5.4 Trama corrupta

```
 MÁQUINA                                CONTROLADOR
    │                                        │
    │──── VEND_REQUEST (con ruido) ─────────▶│  rx: CRC no coincide
    │                                        │  → VCP_EV_ERR_CRC, se descarta
    │       (silencio)                       │  → re-sincroniza
    │                                        │
    │ ...100 ms...                           │
    │──── VEND_REQUEST (de nuevo) ──────────▶│  rx: ahora sí
    │◀──── VEND_PENDING ─────────────────────│
```

Callarse es la respuesta correcta: cuesta un reintento del master. Contestar en
base a una trama corrupta podría iniciar un cobro que nadie pidió.

---

## 6. La máquina de estados del receptor

```
                       byte ≠ 0x02
                     ┌───────────┐
                     │           │  (se cuenta como byte espurio)
                     ▼           │
              ┌─────────────────────┐
   ┌─────────▶│   ESPERANDO_STX     │
   │          └──────────┬──────────┘
   │                     │ byte == 0x02  → arranca un CANDIDATO
   │                     ▼
   │          ┌─────────────────────┐
   │          │        ADDR         │  crc = crc8(addr)
   │          └──────────┬──────────┘
   │                     ▼
   │          ┌─────────────────────┐   LEN > 64
   │          │        LEN          │──────────────▶ ERR_LEN
   │          └──────────┬──────────┘
   │                     ▼
   │          ┌─────────────────────┐
   │          │        CMD          │
   │          └──────┬───────┬──────┘
   │            LEN>0│       │LEN==0
   │                 ▼       │
   │          ┌────────────┐ │
   │          │  PAYLOAD   │ │  ← acá NO se mira el valor del byte:
   │          │  (LEN veces)│ │     0x02 y 0x03 son datos normales
   │          └──────┬─────┘ │
   │                 └───┬───┘
   │                     ▼
   │          ┌─────────────────────┐
   │          │        CRC          │  se guarda, todavía no se compara
   │          └──────────┬──────────┘
   │                     ▼
   │          ┌─────────────────────┐   byte ≠ 0x03
   │          │        ETX          │──────────────▶ ERR_FRAMING
   │          └──────────┬──────────┘
   │                     │ byte == 0x03
   │                     ▼
   │              ¿crc_rx == crc_calc?
   │                 │           │
   │              no │           │ sí
   │                 ▼           ▼
   │             ERR_CRC     VCP_EV_FRAME  ─────▶ vcp_app_on_frame()
   │                 │           │
   └─────────────────┴───────────┘
       (error → re-sincronización;  éxito → siguiente byte)

   Y en paralelo, desde vcp_rx_tick():
       cualquier estado ≠ ESPERANDO_STX  +  >5 ms sin bytes  →  ERR_TIMEOUT
```

Detalle que conviene tener a mano: **primero se valida el ETX y después el
CRC**. Si el ETX no está donde debía, los límites de la trama están mal, así
que el CRC se calculó sobre el conjunto equivocado de bytes y su resultado no
significa nada. Reportar "CRC malo" ahí sería engañoso.

---

## 7. La re-sincronización, que es la decisión de diseño central

Cuando un candidato falla hay que decidir **desde dónde** seguir buscando.

```
   stream:   ... 02 0A 08 40 54 52 02 0A 02 21 00 2A 0F 03 ...
             offset: 36                42
                     └──────────────────────────────────────┘
                       candidato que falla por CRC
                                        └────────────────────┘
                                          trama VÁLIDA adentro

   política DESCARTE  : tira los 14 bytes, sigue con lo que venga → PIERDE la trama
   política REPROCESO : vuelve a mirar desde el offset 37         → LA ENCUENTRA
```

Con el vector del Anexo B la diferencia es medible y está en `salida/`:

| | tramas detectadas |
|---|---|
| `--resync reproceso` (por defecto) | **6** |
| `--resync descarte` | 3 |

El razonamiento completo, el costo y las limitaciones están en
`01_E1_decisiones.md`. Lo importante acá: **el reproceso también tiene un
efecto colateral** — una trama recuperada se descubre *después* de su propio
ETX. Si ese retardo pasa los 5 ms, la trama está detectada pero ya no se puede
contestar. Por eso `vcp_frame_t` lleva `t_etx_ms` y por eso hay una guardia de
plazo en `vcp_app.c`.

---

## 8. La máquina de estados de la venta

```
                    ┌──────────────────┐
        ┌──────────▶│    INACTIVA      │◀────────────┐
        │           └────────┬─────────┘             │
        │                    │ VEND_REQUEST válido   │
        │                    │ → pago_iniciar()      │
        │                    │ → responde PENDING    │
        │                    ▼                       │
        │           ┌──────────────────┐             │
        │           │    COBRANDO      │             │
        │           └────────┬─────────┘             │
        │                    │                       │
        │     pago_estado(): │ AUTORIZADO → txid     │
        │                    │ RECHAZADO  → deny 0x03│
        │      >3000 ms      │ timeout    → deny 0x05│
        │                    ▼                       │
        │           ┌──────────────────────┐         │
        │           │ RESULTADO_PENDIENTE  │         │
        │           └────────┬─────────────┘         │
        │                    │ llega STATUS_REQ      │
        │        ┌───────────┴──────────┐            │
        │  DENY  │                      │ APPROVE    │
        └────────┘                      ▼            │
        (se informa            ┌──────────────────┐  │
         una sola vez)         │ESPERANDO_CONFIRMA│  │
                               └────────┬─────────┘  │
                                        │            │
              VEND_SUCCESS / VEND_FAILURE con el txid│
              o vencimiento a los 2000 ms ───────────┘
```

Mientras el estado es `ESPERANDO_CONFIRMA`, cada `STATUS_REQ` vuelve a recibir
el **mismo** `VEND_APPROVE` con el **mismo** `txid`. No es un error: es lo que
protege el caso en que nuestra respuesta se perdió en el ruido y el master
nunca se enteró de que hay una venta aprobada… con la plata ya cobrada.

---

## 9. Orden sugerido para leer el código

Si es la primera vez, este orden hace que cada archivo se apoye en el anterior:

| # | Archivo | Por qué acá | Tiempo |
|---|---|---|---|
| 1 | `lib/vcp/include/vcp/vcp_cfg.h` | los números del protocolo, sin lógica | 5 min |
| 2 | `lib/vcp/src/vcp_crc.c` | 40 líneas, se entiende entero | 10 min |
| 3 | `lib/vcp/src/vcp_frame.c` | cómo se arma una trama; la trampa del CRC | 15 min |
| 4 | `lib/vcp/include/vcp/vcp_rx.h` | **leer los comentarios de cabecera enteros** | 20 min |
| 5 | `lib/vcp/src/vcp_rx.c` | el corazón: máquina de estados + reproceso | 45 min |
| 6 | `lib/vcp/src/vcp_session.c` | la venta, que sobrevive entre tramas | 25 min |
| 7 | `lib/vcp/src/vcp_app.c` | la tabla A.4 y las 7 decisiones en orden | 25 min |
| 8 | `app/main_vcp1.c` | cómo se arma el lazo principal | 15 min |
| 9 | `tests/test_rx.c` | cada prueba nombra el caso que cubre | 20 min |

Para preparar la defensa hay una guía aparte: `06_guia_de_estudio.md`.

---

## 10. Cómo correr todo

```powershell
# Windows (PowerShell) - no hace falta make
cd firmware
.\build.ps1 -Test        # compila y corre las 179 verificaciones
.\build.ps1 -Run         # corre el banco con el vector del Anexo B
```

```bash
# Linux / macOS / MSYS2
cd firmware
make test
make run
```

Y para ver los tres escenarios que importan:

```powershell
.\build\vcp1.exe ..\Anexos\stream_vcp1.txt                    # política por defecto
.\build\vcp1.exe --resync descarte ..\Anexos\stream_vcp1.txt  # la comparación
.\build\demo_venta.exe                                        # los 7 escenarios de venta
```
