# El vector del Anexo B, byte por byte

> Material de estudio, no parte del entregable evaluado. Sirve para poder
> defender el resultado del programa sin depender de que "el programa lo dice".
> **Todo lo de acá se puede reproducir a mano**: los CRC están calculados y
> verificados uno por uno.

El vector son 76 bytes:

```
offset  0: AA 55 7F 02 0A 00 10 F7 03 02 0B 00 10 9C 03 02
offset 16: 0A 06 40 41 42 02 03 43 44 61 03 02 0A 03 20 05
offset 32: 03 E8 E3 03 02 0A 08 40 54 52 02 0A 02 21 00 2A
offset 48: 0F 03 02 0A 41 40 58 58 58 02 02 0A 02 30 00 2A
offset 64: C6 03 02 00 02 40 42 43 15 03 00 FF
```

**Resultado**: 6 tramas válidas, 4 errores, 5 bytes de ruido puro
(3 al principio + 2 al final). Dos de las 6 tramas solo aparecen si la política
de re-sincronización es la de reproceso.

---

## Recorrido

### `[0..2]` — `AA 55 7F` · ruido

Ninguno es `0x02`, así que el receptor los descarta uno por uno estando en
`ESPERANDO_STX`. Se cuentan como *bytes fuera de trama* (evento propio, para
poder distinguir "cableado ruidoso" de "tramas rotas": son dos fallas con dos
causas distintas).

### `[3..8]` — TRAMA 1 · `STATUS_REQ` para nosotros ✅

```
02   0A     00     10     F7    03
STX  ADDR   LEN    CMD    CRC   ETX
     =0x0A  =0     =0x10
     nuestra       STATUS_REQ
```

CRC sobre `0A 00 10`:

| paso | entrada | CRC acumulado |
|---|---|---|
| inicial | — | `0x00` |
| `^0x0A`, 8 corrimientos | `0x0A` | `0x36` |
| `^0x00`, 8 corrimientos | `0x00` | `0x82` |
| `^0x10`, 8 corrimientos | `0x10` | **`0xF7`** ✔ |

**Respuesta:** `STATUS_RESP`, porque no hay resultado de venta pendiente.

```
TX: 02 0A 04 11 00 00 00 EB 38 03
             │  │  │  └─┴── temperatura 0x00EB = 235 décimas = 23,5 °C
             │  │  └─────── reservado
             │  └────────── estado 0x00 = LISTO
             └───────────── STATUS_RESP, LEN=4
```

### `[9..14]` — TRAMA 2 · `STATUS_REQ` para **otro** dispositivo ✅

`ADDR = 0x0B`. CRC sobre `0B 00 10` = `0x9C` ✔.

Se parsea entera (para no perder el encuadre) y **no se responde**. Es el
requisito E1.13: si contestáramos, chocaríamos con la respuesta del dispositivo
`0x0B` y se corromperían las dos tramas.

### `[15..26]` — TRAMA 3 · `LOG_EVENT` con `0x02` y `0x03` **dentro** ✅

```
02  0A  06  40  41 42 02 03 43 44  61  03
            │   │  │  │  │  │  │
            │   A  B  ↑  ↑  C  D
            │         STX ETX  ← bytes de datos, NO delimitadores
            LOG_EVENT
```

CRC sobre `0A 06 40 41 42 02 03 43 44` = `0x61` ✔.

**Esta es la trama trampa.** Un receptor que hiciera "si veo `0x02`, reinicio"
la rompería. El nuestro está en `RX_PAYLOAD` y no mira el valor del byte: solo
cuenta hasta `LEN`.

**Respuesta:** `ACK` → `02 0A 00 01 80 03`

### `[27..35]` — ERROR 1 · CRC ❌

```
02  0A  03  20  05  03 E8  E3  03
        │   │   │   └──┬┘   └── CRC recibido
        │   │   │      precio = 0x03E8 = 1000 centavos
        │   │   selección = 5
        │   VEND_REQUEST
        LEN=3
```

El CRC calculado sobre `0A 03 20 05 03 E8` da **`0x1C`**, no `0xE3`.

Es una `VEND_REQUEST` que *habría sido válida* (selección 5 está en 1..80,
precio 1000 está en 50..20000). Justamente por eso está en el vector: **una
venta corrupta no puede arrancar un cobro.** Se descarta entera y no se
responde; el master reintentará a los 100 ms.

Re-sincronización: se re-examina desde el offset 28. No hay ningún `0x02` en
`[28..35]`, así que los 8 bytes se descartan y se sigue.

### `[36..49]` — ERROR 2 · CRC ❌ … con una trama adentro

```
02  0A  08  40  54 52 02 0A 02 21 00 2A   0F  03
        │   │   T  R  └──────────┬──────┘  └── "CRC" 
        │   LOG_EVENT            │
        LEN=8              8 bytes de "payload"
```

CRC sobre `0A 08 40 54 52 02 0A 02 21 00 2A` = **`0x40`**, no `0x0F`. Falla.

Pero mirá dónde cae el `0x02` del offset 42:

```
offset:  42 43 44 45 46 47 48 49
         02 0A 02 21 00 2A 0F 03
        STX ADDR LEN CMD  ─┬─ CRC ETX
            0x0A  2  0x21   txid = 0x002A = 42
                  VEND_APPROVE
```

CRC sobre `0A 02 21 00 2A` = **`0x0F`** ✔ — coincide exactamente con el byte
del offset 48. **Adentro del payload falso hay una trama perfectamente
válida.** El candidato externo era el señuelo.

### `[42..49]` — TRAMA 4 · `VEND_APPROVE` recuperada ✅ (y a tiempo)

Se encuentra al re-examinar desde el offset 37. Su ETX (offset 49) coincide con
el byte que hizo fallar al candidato externo, así que el **retardo es 0 ms**:
llegó a tiempo.

**Y aun así no se responde.** `VEND_APPROVE` es un comando
controlador → máquina: es algo que emitimos *nosotros*. Recibirlo no es una
orden. El enunciado lo anticipa: *"puede contener tramas que en operación
normal no deberían circular en ese sentido"*. Criterio NE‑05: se registra
(contador `comandos no admitidos`) y no se transmite nada.

> **Dato de paso, útil para NE‑14:** esta trama es controlador → máquina y
> lleva `ADDR = 0x0A`, o sea *nuestra* dirección. Es la evidencia de que la
> convención del bus es "la dirección del slave viaja en los dos sentidos", que
> es exactamente lo que hace nuestro emisor.

### `[50..52]` — ERROR 3 · LEN fuera de rango ❌

```
02  0A  41  ...
        └── LEN = 0x41 = 65 > 64
```

Se corta **en el byte del LEN**, sin esperar CRC ni ETX. Es el requisito E1.3:
seguir juntando bytes con un LEN imposible es justamente lo que desborda un
buffer.

Re-sincronización desde el offset 51 → `0A 41` se descartan, y después
`[53..56] = 40 58 58 58` también (no son `0x02`).

### `[57..72]` — ERROR 4 · framing ❌ … con **dos** tramas adentro

El `0x02` del offset 57 es basura, pero arranca un candidato:

```
offset:  57  58  59  60  61 62 63 64 65 66 67 68 69 70  71  72
         02  02  0A  02  30 00 2A C6 03 02 00 02 40 42  43  15
        STX ADDR LEN CMD └──────── 10 bytes de payload ──┘ CRC ETX?
             0x02 =10 0x02                                     └── 0x15 ≠ 0x03
```

El candidato se traga **la trama buena y la siguiente**, y muere en el ETX
(offset 72) porque ahí hay `0x15`. Con la política de descarte, acá se pierden
dos tramas y el receptor no vuelve a encuadrar hasta el final del vector.

### `[58..65]` — TRAMA 5 · `VEND_SUCCESS` recuperada ✅ (pero tarde)

Re-examen desde el offset 58, que **también** es `0x02`:

```
02  0A  02  30  00 2A  C6  03
       LEN=2 VEND_SUCCESS  txid = 0x002A = 42
```

CRC sobre `0A 02 30 00 2A` = **`0xC6`** ✔.

Corresponde responder `ACK`. **Pero no se transmite**, y este es el punto más
interesante del ejercicio:

```
su ETX llegó en el offset 65   → t = 65 × 1,0417 ms = 67,7 ms
se detecta en el offset 72     → t = 72 × 1,0417 ms = 75,0 ms
                                 retardo = 8 ms  >  5 ms de plazo
```

El Anexo A.2 es explícito: *"el plazo es duro: si no llegás, no contestes. Una
respuesta vieja contestando un comando nuevo es peor que no contestar"*. El
programa muestra el `ACK` que correspondía, marca `NO TRANSMITIDA` y explica
por qué. Con `--sin-plazo` se ve el otro escenario.

Con `--byte-ms 0` (sin simulación de tiempo) el retardo es 0 y sí se
transmitiría: sirve para separar "¿la lógica está bien?" de "¿llegó a tiempo?".

### `[66..73]` — TRAMA 6 · `LOG_EVENT` broadcast ✅

```
02  00  02  40  42 43  15  03
   ADDR=0x00 = broadcast   texto "BC"
```

CRC sobre `00 02 40 42 43` = `0x15` ✔.

**Solo se encuentra porque el reproceso restauró el encuadre.** Y su ETX
(offset 73) llega como byte nuevo, así que se detecta **en tiempo real**,
retardo 0. Es la mejor evidencia del valor de la política: el reproceso no solo
rescata tramas viejas, sobre todo **devuelve la sincronía** para las que vienen
después.

Broadcast ⇒ se procesa y no se responde (Anexo A.4).

### `[74..75]` — `00 FF` · ruido de cola

---

## Cuadro final

| # | offset | trama | dirigida a | ¿se responde? | qué |
|---|---|---|---|---|---|
| 1 | 3..8 | `STATUS_REQ` | 0x0A propia | **sí** | `STATUS_RESP` |
| 2 | 9..14 | `STATUS_REQ` | 0x0B ajena | no | filtrado por dirección |
| 3 | 15..26 | `LOG_EVENT` "AB␂␃CD" | 0x0A propia | **sí** | `ACK` |
| 4 | 42..49 | `VEND_APPROVE` txid=42 | 0x0A propia | no | sentido inverso (NE‑05) |
| 5 | 58..65 | `VEND_SUCCESS` txid=42 | 0x0A propia | no | correspondía `ACK`, fuera de plazo |
| 6 | 66..73 | `LOG_EVENT` "BC" | 0x00 broadcast | no | broadcast |

| # | offset | error | detalle |
|---|---|---|---|
| 1 | 27..35 | CRC | recibido `0xE3`, correcto `0x1C` |
| 2 | 36..49 | CRC | recibido `0x0F`, correcto `0x40` |
| 3 | 50..52 | LEN | `0x41` = 65 > 64 |
| 4 | 57..72 | framing | en la posición del ETX había `0x15` |

**Contadores:** 76 bytes recibidos · 24 fuera de trama · 38 reprocesados ·
6 tramas · 2 recuperadas · 2 respuestas emitidas · 1 omitida por plazo.

---

## Cómo verificar un CRC a mano

Por si en la defensa hace falta:

```
crc = 0x00
por cada byte b de (ADDR, LEN, CMD, PAYLOAD):     ← ¡sin STX ni ETX!
    crc = crc XOR b
    repetir 8 veces:
        si crc tiene el bit 0x80 en 1:  crc = ((crc << 1) & 0xFF) XOR 0x07
        si no:                          crc =  (crc << 1) & 0xFF
```

Ejemplo con el byte `0x0A` desde `crc = 0x00`:

```
0x0A → 0x14 → 0x28 → 0x50 → 0xA0 → (0x40^0x07)=0x47 → 0x8E → (0x1C^0x07)=0x1B → 0x36
```
