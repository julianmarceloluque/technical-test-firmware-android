# E1 — Decisiones de diseño

**Entregable escrito de E1.** El enunciado pide máximo una carilla y media; el
material de apoyo (traza del vector byte por byte, guía de estudio) está en
documentos aparte y no forma parte de esta carilla y media.

---

## 1. Decisiones de diseño

| # | Decisión | Por qué |
|---|---|---|
| D1 | **Todo el estado en estructuras de contexto** (`vcp_rx_t`, `vcp_app_t`), cero `malloc`, cero globales mutables en `lib/vcp/`. | E1.2. Efecto lateral útil: se pueden instanciar dos receptores en el mismo binario, que es lo que hacen los tests para comparar las dos políticas de re-sincronización sobre el mismo stream. |
| D2 | **CRC acumulado byte a byte** (`vcp_crc8_byte`) en vez de recorrer la trama al final. | El camino ETX → respuesta queda en dos comparaciones. Ningún trabajo evitable dentro de los 5 ms. |
| D3 | **El receptor no interpreta comandos; la aplicación no parsea bytes.** | El receptor se prueba con vectores de bytes y la aplicación con tramas ya armadas: cada capa se puede romper y arreglar sin tocar la otra. |
| D4 | **`vcp_frame_t` lleva `t_etx_ms`** (agregado a la interfaz sugerida). | Sin la marca de tiempo de la trama, el plazo de 5 ms es una promesa del README. Con ella es una condición que el código evalúa (`vcp_app.c`, paso 6). |
| D5 | **El receptor reporta por callback** en vez de retornar un `vcp_event_t`. | Con re-sincronización por reproceso, **un byte de entrada puede generar dos eventos**: un error y, además, una trama válida escondida adentro del candidato fallido. La firma original obligaría a descartar uno. Descarté la cola de eventos (hay que dimensionarla para el peor caso y se puede olvidar drenar) y la recursión (la pila no se negocia en firmware). |
| D6 | **La sesión de venta no tiene acceso a la UART.** | E1.11. Puede enterarse del cobro en cualquier instante, pero solo deja el resultado listo; lo informa `vcp_app.c` cuando el master pregunta. Si el módulo pudiera transmitir, tarde o temprano alguien agregaría un "y avisale". |
| D7 | **La respuesta se arma sobre una variable local, en la misma llamada.** | E1.14. No existe ningún buffer de "respuesta pendiente" que alguien pueda transmitir después: es estructuralmente imposible contestar una trama anterior. |
| D8 | **`vcp_uart_write()` se llama desde un único lugar del proyecto.** | Hace auditable el cumplimiento de E1.11: un `grep` alcanza para verificarlo. |
| D9 | **Todos los plazos con resta sin signo** (`(uint32_t)(now - antes) > T`). | Es correcto cuando el contador de milisegundos da la vuelta a los 49,7 días. La forma `now > antes + T` falla justo ahí y el bug aparece a los 50 días de funcionamiento. Hay un test específico. |
| D10 | **Toda la salida por consola en ASCII sin tildes.** | La consola de Windows y la de Linux no comparten codificación por defecto; los comentarios y los documentos sí llevan acentos. |

---

## 2. Política de recuperación ante trama inválida

### Qué elegí

**Re-proceso** (`VCP_RESYNC_REPROCESO`, por defecto): cuando un candidato
muere, sus bytes vuelven a pasar por la máquina de estados **empezando uno
después del `0x02` que lo originó**.

La otra opción —descartar el candidato entero y seguir con los bytes nuevos—
también está implementada (`--resync descarte`) para poder mostrar la
diferencia sobre el mismo vector.

### Por qué esa y no la otra

1. **El `0x02` que arrancó el candidato pudo no ser un STX.** El protocolo no
   tiene escapado: cualquier byte de payload puede valer `0x02`. Un candidato
   fallido es, muy a menudo, un STX falso que se tragó una trama de verdad.
2. **El costo real de descartar no es perder *esa* trama: es perder el
   encuadre.** Un candidato falso consume bytes hasta donde diga su LEN
   inventado, y eso puede tapar varias tramas siguientes. Con el master
   declarando al controlador fuera de servicio a los 3 silencios (Anexo A.2),
   quedar desencuadrado dos o tres tramas seguidas es exactamente el camino a
   "medio de pago fuera de servicio".
3. **El costo del reproceso está acotado y es analizable.** Peor caso
   O(N²) con N ≤ 70 → ~2 450 pasos de máquina de estados, y solo cuando hay
   corrupción; el camino normal sigue siendo un paso por byte. Hay una red de
   seguridad (`RX_MAX_PASOS_POR_BYTE`) que corta y cuenta si alguna vez se
   superara.
4. **No inventa información.** Solo vuelve a mirar bytes que ya llegaron, con
   el mismo CRC y las mismas validaciones. Una trama recuperada pasó por
   exactamente los mismos controles que cualquier otra.

### El efecto colateral, que es parte de la decisión

Una trama recuperada se descubre **después** de su propio ETX: hay que esperar
a que el candidato falso muera. Si ese retardo pasa los 5 ms, la trama está
detectada pero **ya no se puede contestar** (Anexo A.2: *"el plazo es duro: si
no llegás, no contestes"*). Por eso el reproceso viene acompañado de la guardia
de plazo de `vcp_app.c`: sin ella, el reproceso introduciría justo el problema
que el enunciado marca como peor que no contestar.

En el banco de pruebas eso se ve explícito: la trama del offset 58 se detecta,
se muestra qué se le habría contestado, y se informa por qué no se transmite.

### Limitación conocida

Con un stream **continuo** y adversarial (por ejemplo miles de `0x02` seguidos,
sin ni un hueco de 5 ms), el inicio del candidato avanza al mismo ritmo que el
stream y nunca alcanza un STX verdadero posterior. No hay desborde ni
corrupción: la ventana queda corrida. Se recupera sola en cuanto hay una pausa,
que en un bus half-duplex donde el master espera respuesta siempre existe. Hay
un test que documenta el caso y la recuperación.

La alternativa que lo resolvería es **seguir varios candidatos en paralelo**
(uno por cada `0x02` en vuelo). Detectaría cada trama exactamente en su ETX,
sin retardo, y por lo tanto contestaría más. La descarté por costo: ~550 bytes
de RAM adicionales, y una complejidad que hay que mantener para ganar, como
mucho, un reintento de master cada tanto.

---

## 3. Qué casos del vector del Anexo B la ejercitan

Resultado con las dos políticas (salidas completas en `salida/`):

| | tramas válidas | recuperadas | respuestas emitidas |
|---|---|---|---|
| `reproceso` (por defecto) | **6** | 2 | 2 |
| `descarte` | 3 | 0 | 2 |

| offset | qué es | qué ejercita |
|---|---|---|
| 0–2 | `AA 55 7F` | ruido entre tramas; se reporta diferenciado, no se confunde con trama rota |
| 3 | `STATUS_REQ` a `0x0A` | camino normal → se responde `STATUS_RESP` |
| 9 | `STATUS_REQ` a `0x0B` | **filtrado por dirección** (E1.13): se parsea, no se contesta |
| 15 | `LOG_EVENT` con `02 03` **dentro del payload** | protocolo sin escapado: un parser que resincronice al ver `0x02` se rompe acá → se responde `ACK` |
| 27 | `VEND_REQUEST` con CRC `0xE3` (correcto: `0x1C`) | **CRC**: una venta corrupta no arranca un cobro |
| 36 | `LOG_EVENT` LEN=8 con CRC roto… | …que **contiene** en el offset 42 un `VEND_APPROVE` válido |
| **42** | `VEND_APPROVE txid=42` a `0x0A` | **recuperada por reproceso, y a tiempo** (retardo 0 ms). Es un comando de *sentido inverso*: se registra y **no** se responde (NE‑05). Es la trama que el enunciado anticipa con *"puede contener tramas que en operación normal no deberían circular en ese sentido"* |
| 50 | `LEN = 0x41 = 65` | **E1.3**: se corta en el byte del LEN, sin esperar CRC ni ETX |
| 57 | `0x02` duplicado → candidato falso con LEN=10 | **framing**: se traga la trama buena *y* la siguiente |
| **58** | `VEND_SUCCESS txid=42` | **recuperada por reproceso, pero 8 ms tarde** → se muestra el `ACK` que correspondía y **no se transmite** (E1.14) |
| 66 | `LOG_EVENT` broadcast (`ADDR=0x00`) | **solo se encuentra si el reproceso restauró el encuadre**; se procesa y no se responde |
| 74–75 | `00 FF` | ruido de cola |

Lo que el vector **no** trae: ningún `VEND_REQUEST` válido y ningún timeout
entre bytes. Los dos caminos se cubren en `app/main_demo_venta.c` (7 escenarios)
y en `tests/`.

---

## 4. Situaciones que la especificación no define

Están todas las que detecté, resueltas y no resueltas.

| ID | Situación | Criterio adoptado |
|---|---|---|
| NE‑01 | Llega `VEND_REQUEST` con sesión abierta **y además** fuera de rango: ¿qué motivo? | `0x04 sesión en curso`. El estado del sistema pesa más que la validación del contenido: le dice al master que espere, no que corrija. |
| NE‑02 | El master **retransmite** el mismo `VEND_REQUEST` (perdió el `PENDING`). | Mismo `sel` + mismo `precio` + sesión cobrando ⇒ retransmisión: se repite `VEND_PENDING` y **no** se inicia un segundo cobro. Tratarlo como pedido nuevo abortaría una venta ya cobrada. Riesgo asumido: dos pedidos legítimos idénticos simultáneos se toman como uno (imposible en una expendedora: hay un solo mecanismo de entrega). |
| NE‑03 | ¿Cuánto vive un "resultado pendiente"? | **`APPROVE`: latcheado** — se repite con el mismo `txid` en cada `STATUS_REQ` hasta que llegue `VEND_SUCCESS/FAILURE` o venzan 2 000 ms. **`DENY`: una sola vez.** Asimetría deliberada: en el `DENY` no se movió plata, perder el mensaje cuesta un reintento; en el `APPROVE` sí, y perderlo significa cobrar sin entregar. |
| NE‑04 | Selección **y** precio inválidos a la vez. | Se informa `0x02 selección inválida`: la selección identifica el producto, sin producto válido el precio no significa nada. |
| NE‑05 | Llega un comando que **emitimos nosotros** (`ACK`, `STATUS_RESP`, `VEND_APPROVE`, `VEND_DENY`, `VEND_PENDING`) dirigido a `0x0A`. | Se registra y **no se responde** (mismo trato que un comando desconocido: la tabla A.4 dice "nada"). Ocurre en el offset 42 del vector. |
| NE‑06 | `VEND_SUCCESS` / `VEND_FAILURE` con `txid` que no es el nuestro, o sin sesión abierta. | Se cuenta, se registra, y **se responde `ACK` igual**. El `ACK` acusa recepción a nivel protocolo, no aprueba nada. Callarse haría que el master reintente 3 veces y nos declare fuera de servicio: mucho peor que un `txid` huérfano. Ocurre en el offset 58. |
| NE‑07 | Trama para otra dirección (o broadcast) que **es** un `VEND_REQUEST`. | "Procesar" significa parsear y contar, nada más: **no toca la sesión**. Un pedido dirigido a `0x0B` es una venta de otro dispositivo; arrancar un cobro por ella sería cobrar por una operación ajena. |
| NE‑08 | El Anexo A.3 define el campo `estado` de `STATUS_RESP` pero **no sus valores**, ni qué poner en `reservado`. | Inventados y documentados: `0x00 LISTO`, `0x01 OCUPADO`, `0x02 FUERA_DE_SERVICIO` (0 = todo bien, creciente = peor). `reservado = 0x00`: mandar basura ataría un uso futuro del campo. |
| NE‑09 | Trama con CRC perfecto pero `LEN` incoherente con el comando (p. ej. `VEND_REQUEST` con `LEN=2`). | **No se responde.** El CRC dice "los bytes llegaron como salieron", no "los bytes tienen sentido". Sin este chequeo leeríamos `payload[2]` de una trama de 2 bytes. |
| NE‑10 | Qué hacer después de una trama inválida. | Sección 2 de este documento. |
| NE‑11 | Trama con ETX ausente **y** CRC malo: ¿qué error se informa? | `FRAMING`. Sin ETX los límites están mal, así que el CRC se calculó sobre otros bytes y no significa nada; informar "CRC" mandaría a buscar ruido cuando el problema es de encuadre. |
| NE‑12 | Una trama válida detectada **fuera de plazo**: ¿se ignora del todo? | No: **el estado interno sí se actualiza** (el cobro arranca, la sesión avanza) y lo único que se suprime es la emisión. Ignorarla nos dejaría desincronizados con una expendedora que sí la mandó. |
| NE‑13 | Política ante **timeout entre bytes**: ¿también se reprocesa? | **No, se descarta todo.** Si venció el timeout, cualquier trama rescatable tendría su ETX ≥ 5 ms en el pasado, o sea que igual no se podría contestar. Trabajo para un resultado inutilizable. |
| NE‑14 | ¿Qué dirección lleva la **respuesta**? El Anexo A.1 dice que `ADDR` es "la dirección del destinatario", pero **la dirección de la expendedora no está definida en ningún lado**. | Se responde con la **propia** (`0x0A`), que es la convención de los buses master/slave (Modbus, MDB): la dirección del slave viaja en los dos sentidos y el master sabe a quién interrogó. El vector lo respalda: el `VEND_APPROVE` del offset 42 —un comando controlador→máquina— lleva `ADDR = 0x0A`. |
| NE‑15 | El byte `causa` de `VEND_FAILURE` no tiene tabla de valores. | Se registra y se propaga al backend sin interpretarlo. **No resuelto**: haría falta la tabla para decidir si una causa amerita devolución automática. |
| NE‑16 | No se define cuántas veces repetir un `VEND_APPROVE` sin confirmar antes de darlo por perdido. | Elegí plazo (2 000 ms) en vez de contador de repeticiones, porque el valor sale del propio log del ejercicio E3. **Discutible**: un contador sería más predecible si el master cambiara su período de sondeo. |
| NE‑17 | Compatibilidad futura: ¿qué pasa si el master manda un `STATUS_REQ` con payload porque una versión nueva agregó campos? | Hoy se rechaza (NE‑09). **No resuelto**: una política "ignorar bytes de más al final" sería más tolerante a versiones, pero relaja una validación que hoy nos protege. Lo dejo anotado como decisión a tomar con la especificación de la versión 2 a la vista. |

---

## 5. Qué modifiqué de la interfaz sugerida

Está listado con su justificación en `firmware/lib/vcp/include/vcp/vcp.h`.
Resumen: separé el header único en uno por responsabilidad, cambié el retorno
del receptor por un callback (D5), agregué `t_etx_ms` a la trama (D4), agregué
el evento `VCP_EV_BYTE_FUERA_TRAMA` (E1.5) y `vcp_rx_set_resync()` para poder
comparar políticas. **No toqué** el formato de trama, el CRC, los códigos de
comando ni las firmas del entorno provisto: eso es contrato con la otra punta.
El `Anexos/vcp.h` original quedó intacto.
