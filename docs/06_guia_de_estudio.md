# Guía de estudio para la defensa

> No es parte de la entrega evaluada. Es la hoja de ruta para llegar a la
> videollamada pudiendo explicar y **modificar** el código en vivo (regla R4).

---

## 1. Las cinco frases que hay que poder decir sin pensar

1. **"Hay dos relojes: 5 ms hacia la expendedora y hasta 900 ms hacia el medio
   de pago. Todo el diseño sale de ahí."**
2. **"El receptor no interpreta comandos y la aplicación no parsea bytes."**
   Por eso el receptor se prueba con vectores y la aplicación con tramas.
3. **"`vcp_uart_write()` se llama desde un solo lugar, y ese lugar es el manejo
   de una trama recibida."** Por eso E1.11 se cumple por construcción.
4. **"La respuesta se arma sobre una variable local en la misma llamada; no
   existe ningún buffer de respuesta pendiente."** Por eso es imposible
   contestar una trama anterior (E1.14).
5. **"Al re-sincronizar puedo encontrar una trama vieja, y por eso cada trama
   trae su `t_etx_ms`: si pasaron más de 5 ms, no contesto."**

---

## 2. Números para tener a mano

| Cosa | Valor | De dónde sale |
|---|---|---|
| Dirección propia | `0x0A` | Anexo A.1 |
| Payload máximo | 64 bytes | Anexo A.1 |
| Trama máxima | **70 bytes** | 1+1+1+1+64+1+1 |
| Trama mínima | 6 bytes | sin payload |
| CRC | poly `0x07`, init `0x00`, sobre ADDR+LEN+CMD+PAYLOAD | Anexo A.1 |
| Plazo de respuesta | 5 ms desde el ETX | Anexo A.2 |
| Timeout entre bytes | 5 ms | Anexo A.2 |
| Tiempo de un byte a 9600 8N1 | **1,0417 ms** (10 bits / 9600) | cuenta |
| Reintentos del master | cada 100 ms, 3 y fuera de servicio | Anexo A.2 |
| Cobro | 50 a 900 ms | Anexo A.5 |
| Plazo propio del cobro | 3 000 ms | decisión mía (3× el peor caso) |
| Plazo de confirmación | 2 000 ms | sale del log de E3 |
| Selección válida | 1..80 | Anexo A.4 |
| Precio válido | 50..20 000 centavos | Anexo A.4 |

**Huella medida** (gcc, x86-32; en Cortex‑M da menos):

| | |
|---|---|
| `sizeof(vcp_rx_t)` | **364 bytes** |
| `sizeof(vcp_app_t)` | 104 bytes (incluye la sesión de venta, 60) |
| `sizeof(vcp_frame_t)` | 72 bytes |
| `sizeof(vcp_decision_t)` | 152 bytes (vive en la pila) |
| RAM estática del protocolo | **468 bytes** (`rx` + `app`) |
| `.text` de `lib/vcp/` con `-Os` | ~6,9 KB |

De los 364 bytes de `vcp_rx_t`, **280 son los cuatro buffers**: `crudo[70]`,
`dt_crudo[70]`, `rep[70]`, `rep_dt[70]`. Si preguntan por RAM, esa es la
respuesta y la palanca: los dos `dt_*` (140 bytes) son el precio de poder hacer
cumplir el plazo de 5 ms sobre tramas recuperadas.

**Resultados del vector del Anexo B**: 6 tramas · 4 errores (2 CRC, 1 LEN,
1 framing) · 2 recuperadas · 2 respuestas emitidas · 1 omitida por plazo ·
24 bytes fuera de trama · 38 reprocesados. Con `--resync descarte`: 3 tramas.

**Pruebas**: 179 verificaciones, 0 fallas.

---

## 3. Preguntas probables y cómo responderlas

**"¿Por qué cambiaste la interfaz sugerida a un callback?"**
Porque con reproceso un byte puede generar dos eventos: el error del candidato
y la trama válida que estaba escondida adentro. Con `vcp_event_t` de retorno
hay que tirar uno. Descarté la cola (hay que dimensionarla para el peor caso y
el llamador se puede olvidar de drenarla) y la recursión (pila).

**"¿Por qué el CRC no incluye el STX?"**
Porque lo dice el Anexo A.1 y lo confirma el vector: `0A 00 10` da `0xF7`, que
es lo que trae la trama del offset 3. Si incluyera el STX, ninguna trama nuestra
sería aceptada. Hay un test que compara las dos variantes justamente para dejar
constancia de que la diferencia es silenciosa.

**"¿Cómo garantizás que no se desborda el buffer?"**
Tres capas: (1) `LEN > 64` mata el candidato en el byte del LEN, sin esperar
nada más; (2) el buffer se dimensiona con `VCP_TRAMA_MAX_BYTES = 4 + 64 + 2`;
(3) `crudo_push()` verifica el índice antes de cada escritura y cuenta un error
interno si fallara. La tercera capa nunca se dispara con la máquina de estados
actual; está para el día que alguien la modifique.

**"¿Cómo probás el plazo de 5 ms en una PC?"**
No se mide, se **simula**: el banco le pasa al receptor la marca de tiempo de
cada byte a 1,0417 ms. Con eso el mismo código que corre en el micro evalúa el
plazo igual. Y lo que sí se puede verificar de verdad es que en el camino ETX →
`vcp_uart_write()` no hay ninguna espera: son un `switch`, un `memcpy` de hasta
64 bytes y un CRC sobre lo mismo.

**"¿Por qué el `VEND_APPROVE` se repite y el `VEND_DENY` no?"**
Porque en el `APPROVE` ya se movió plata. Si nuestra respuesta se perdió en el
ruido, el master no sabe que hay una venta aprobada y nunca va a entregar. En el
`DENY` no se movió nada: si se pierde, el peor caso es que el master reintente
la venta. Latchear un `DENY` bloquearía el próximo pedido legítimo.

**"El vector tiene un `VEND_APPROVE` dirigido a vos. ¿Qué hacés?"**
Nada. `VEND_APPROVE` es controlador → máquina: es algo que emito yo. Recibirlo
no es una orden. El enunciado lo anticipa cuando avisa que la captura es de
laboratorio. Lo registro en un contador y no transmito.

**"¿Y si el pago autoriza justo mientras estás transmitiendo?"**
No pasa nada, porque `vcp_session_poll()` no transmite: solo deja el resultado
listo. La transmisión ocurre después, en la respuesta a la próxima
interrogación. Es la razón por la que la sesión no tiene acceso a la UART.

**"¿Qué pasa si `LEN` es 0?"**
El estado `CMD` salta directo a `CRC`, sin pasar por `PAYLOAD`. Es el caso de
`STATUS_REQ`, `ACK` y `VEND_PENDING`, o sea la mayoría del tráfico.

**"¿Por qué validás el ETX antes que el CRC?"**
Porque el ETX es estructural. Si no está donde tenía que estar, los límites de
la trama están mal y el CRC se calculó sobre otro conjunto de bytes: su
resultado no significa nada. Informar "CRC" mandaría a buscar ruido cuando el
problema es de encuadre.

**"¿Tu re-sincronización no puede quedar en un lazo infinito?"**
No: cada fallo mueve el inicio del candidato al menos una posición hacia
adelante, y el inicio nunca pasa el último byte recibido. La cota es O(N²) con
N ≤ 70, unos 2 450 pasos en el peor caso absoluto. Igual hay una red de
seguridad (`RX_MAX_PASOS_POR_BYTE`) que corta y cuenta, porque en firmware todo
lazo tiene que tener una cota demostrable.

**"¿Dónde falla tu diseño?"**
Con un stream continuo adversarial sin ninguna pausa de 5 ms, el inicio del
candidato avanza al mismo ritmo que el stream y no alcanza un STX posterior. No
hay desborde: la ventana queda corrida y se recupera con la primera pausa. Está
documentado, tiene un test, y la alternativa que lo resolvería (candidatos en
paralelo) cuesta ~550 bytes de RAM.

**"¿Por qué restás con `uint32_t` en vez de comparar `now > antes + T`?"**
Porque el contador de milisegundos da la vuelta a los 49,7 días y esa segunda
forma falla exactamente ahí. Hay un test que hace pasar el contador por
`0xFFFFFFFF`.

---

## 4. Preparación para "modificá el código en vivo"

### Cambios de una línea (saber dónde, no memorizar)

| Piden | Archivo | Qué |
|---|---|---|
| Otra dirección de controlador | `vcp_cfg.h` | `VCP_ADDR_SELF` |
| Plazo de 3 ms en vez de 5 | `vcp_cfg.h` | `VCP_PLAZO_RESPUESTA_MS` |
| Otro rango de precios | `vcp_cfg.h` | `VCP_PRECIO_MIN/MAX` |
| CRC con tabla | `vcp_crc.c` | solo ese archivo |
| Que el `DENY` también se latchee | `vcp_session.c` | `vcp_session_tomar_resultado()`, rama `else` |
| Responder también al broadcast | `vcp_app.c` | paso 1 |
| Otro plazo de cobro | `vcp_cfg.h` | `VCP_PAGO_TIMEOUT_MS` |
| Un contador nuevo | `vcp_rx.h` / `vcp_app.h` | agregar al struct de stats e incrementarlo |

### El ejercicio más probable: agregar un comando nuevo

Supongamos `0x50 PRICE_UPDATE`, máquina → controlador, payload 3 bytes
(selección u8 + precio nuevo u16 BE), y hay que contestar `ACK`. Son **cinco
lugares**, siempre los mismos, y conviene poder recitarlos:

1. `vcp_frame.h` → agregar `VCP_CMD_PRICE_UPDATE = 0x50` al enum.
2. `vcp_frame.c` → `vcp_cmd_nombre()`: un `case` para el log.
3. `vcp_frame.c` → `vcp_cmd_es_del_master()`: agregarlo a los `true`
   (**si esto falta, la trama se descarta como "sentido inverso"**).
4. `vcp_frame.c` → `vcp_len_esperada_ok()`: `return (len == 3u);`
   (**si esto falta, se descarta como LEN inesperado**).
5. `vcp_app.c` → un `case` en el `switch` del paso 4 que lea el payload y ponga
   `dec->resp_cmd = VCP_CMD_ACK; dec->resp_len = 0u;`.

Y un test en `test_app.c` copiando el bloque de `VEND_SUCCESS`.

Los pasos 3 y 4 son los que se olvidan y los que hacen que "la trama llegue pero
no pase nada". Vale la pena decirlo en voz alta al hacerlo: muestra que el
diseño tiene filtros a propósito.

### Un pedido de diseño, no de código

Si preguntan *"¿cómo agregarías la cancelación de una venta en curso?"*:
comando nuevo máquina → controlador `VEND_CANCEL(txid)`; en `vcp_session.c`, si
el estado es `COBRANDO` no se puede cancelar sin poder anular el cobro (la
interfaz del medio de pago no lo permite → respondo `VEND_DENY 0x05`); si es
`ESPERANDO_CONFIRMA` es equivalente a un `VEND_FAILURE` y va por el mismo
camino, con registro para devolución. El punto de la respuesta es que **la
limitación es de la interfaz del medio de pago**, no del protocolo.

---

## 5. Cómo demostrarlo en pantalla, en 3 minutos

```powershell
cd firmware

# 1) las pruebas pasan
.\build.ps1 -Test

# 2) el vector del Anexo B con la politica por defecto: 6 tramas
.\build\vcp1.exe ..\Anexos\stream_vcp1.txt

# 3) la MISMA entrada con la otra politica: 3 tramas
#    (aca se muestra por que se eligio reproceso)
.\build\vcp1.exe --resync descarte ..\Anexos\stream_vcp1.txt

# 4) la venta completa, con el cobro de 250 ms y el plazo de 5 ms
.\build\demo_venta.exe 1

# 5) el cobro que nunca resuelve
.\build\demo_venta.exe 6
```

El momento a señalar en (2) es la **TRAMA #5**: se detecta, se muestra el `ACK`
que correspondía, y dice `NO TRANSMITIDA` con el retardo de 8 ms. Ahí está,
junta, la mitad del ejercicio.

---

## 6. Repaso de 20 minutos, la noche anterior

1. `docs/00_ROADMAP.md`, secciones 3 a 8 (los diagramas). — 8 min
2. `docs/02_E1_analisis_vector.md`, el recorrido del vector. — 5 min
3. `firmware/lib/vcp/src/vcp_rx.c`, solo la función `rx_resincronizar()` y su
   comentario de cabecera. — 4 min
4. `firmware/lib/vcp/src/vcp_app.c`, los siete pasos numerados de
   `vcp_app_on_frame()`. — 3 min

Si solo hay tiempo para una cosa: los **siete pasos de `vcp_app_on_frame()`**.
Es donde están, en orden y comentadas, casi todas las decisiones que el
enunciado pide justificar.
