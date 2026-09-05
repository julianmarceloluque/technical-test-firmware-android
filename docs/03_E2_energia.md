# E2 — Consistencia ante corte de energía

Referido a la implementación de E1: los puntos de escritura están marcados en
`firmware/lib/vcp/src/vcp_session.c` con comentarios `[E2: PUNTO DE ESCRITURA
NVM #n]`, así que el documento y el código hablan del mismo lugar.

---

## Estructura elegida: diario circular, nunca reescritura en el lugar

Antes de responder los puntos, la decisión de la que dependen todos:

> La memoria no volátil **no guarda "el estado de la sesión"**. Guarda una
> **secuencia de hechos ya ocurridos**, cada uno escrito una sola vez y jamás
> modificado. El estado de la sesión se **reconstruye** al arrancar leyendo el
> diario.

Un registro (32 bytes, alineado a la página de escritura):

```
 0    3   4     5      6         8        12        16      28  30  31
+-----+---+-----+------+---------+--------+----------+-------+---+---+
| seq |tipo|ver | sel  | precio  |  txid  | t_epoch  | extra |crc|sel|
+-----+---+-----+------+---------+--------+----------+-------+---+---+
  u32  u8   u8    u8      u16       u16       u32              u16 u8
                                                            └── se escriben
                                                                AL FINAL
```

Por qué diario y no un registro de estado que se sobrescribe: en flash, escribir
en el mismo lugar obliga a **borrar el sector entero** cada vez. Cuatro
escrituras por venta × 200 ventas = 800 borrados diarios sobre el mismo sector:
la memoria dura **125 días**. Con diario, el borrado ocurre solo cuando el
sector se llena, y eso cambia el número por tres órdenes de magnitud (ver E2.5).

---

## E2.2 — Qué se escribe, cuándo exactamente, y por qué ahí

| # | Registro | Momento exacto | Por qué no antes | Por qué no después |
|---|---|---|---|---|
| R1 | `INTENTO` (sel, precio, t) | Inmediatamente **antes** de `pago_iniciar()` | Antes del pedido no hay nada que registrar | Si se corta entre `pago_iniciar()` y la autorización (50–900 ms) el terminal puede haber cobrado y nosotros no tendríamos **ni el rastro de que hubo un intento**. Es el único registro que existe para poder conciliar contra el medio de pago |
| R2 | `COBRADO` (txid, importe, t) | En cuanto `pago_estado()` devuelve `PAGO_AUTORIZADO`, dentro de `vcp_session_poll()` | El txid no existe antes | Acá **la plata ya se movió**. Cada milisegundo entre la autorización y el registro es una ventana en la que un corte deja dinero cobrado sin ningún registro. Es el registro más caro de perder |
| R3 | `INFORMADO` (txid, t) | En el lazo principal, **justo después** de transmitir el primer `VEND_APPROVE` | Antes de transmitir no sabemos si vamos a transmitir (la guardia de plazo puede suprimirlo) | Es lo que separa "la máquina supo que podía entregar" de "nunca se enteró". Sin este registro, R2 sin R4 sería siempre ambiguo; con él, la ambigüedad se reduce a un solo caso |
| R4 | `CIERRE` (txid, resultado, t) | Al recibir `VEND_SUCCESS`/`VEND_FAILURE`, o al vencer el plazo de confirmación de 2 000 ms | — | Cierra el episodio. Si no se escribe, al arrancar la venta figura abierta y se dispara la conciliación |

**Restricción dura que ordena todo esto:** *ninguna* de estas escrituras puede
ocurrir dentro del camino ETX → respuesta. Una página de flash tarda 1–5 ms y
un borrado 20–200 ms; el plazo entero son 5 ms. Por eso R3 se escribe **después**
de transmitir, y todas las escrituras viven en el lazo principal, nunca en el
manejo de la trama.

Eso deja una ventana residual de ~1 ms entre transmitir el `VEND_APPROVE` y
registrar R3. Un corte exacto ahí clasifica el episodio como "no informado"
cuando la máquina sí recibió el permiso. Es ~0,05 % del tiempo del episodio y
falla del lado del cliente (le devolvemos de más), que es el lado en el que
elijo equivocarme.

---

## E2.3 — Qué hace al arrancar si encuentra una sesión sin cerrar

Al bootear se lee el diario hacia atrás hasta el último `CIERRE`. Lo que haya
después es una sesión abierta:

| Lo que se registró | Qué sé | Qué hago |
|---|---|---|
| Nada | No había venta en vuelo | Arrancar normal |
| `INTENTO` solo | Hubo un pedido; **no sé si se cobró** | Consultar al medio de pago la última transacción por importe y ventana de tiempo. Si reporta autorización → tratarlo como el caso siguiente. Si no reporta nada → `CIERRE(resultado=SIN_COBRO)` y encolar un aviso de auditoría. Si el terminal no soporta la consulta → encolar como **"intento sin resolver"** para revisión humana |
| `INTENTO` + `COBRADO`, sin `INFORMADO` | Cobré y **la máquina nunca supo** que podía entregar | Certeza: **no hubo entrega**. `CIERRE(resultado=COBRADO_SIN_ENTREGA)` y encolar **devolución** |
| `INTENTO` + `COBRADO` + `INFORMADO`, sin `CIERRE` | Cobré, la máquina fue autorizada, **no sé si entregó** | Es el caso de E2.4 |
| Todo + `CIERRE` | Episodio completo | Nada |

En los tres casos con sesión abierta, además: la sesión **no** se reabre en
memoria. Al arrancar el controlador queda `INACTIVA` y listo para vender. La
venta vieja se resuelve por la vía contable, no reintentando el protocolo: la
expendedora también se reinició y no está esperando nuestra respuesta.

---

## E2.4 — El caso en el que no se puede saber

**`COBRADO` + `INFORMADO` sin `CIERRE`.** Emitimos el `VEND_APPROVE`, la
máquina tuvo permiso para entregar, y el corte ocurrió antes de que llegara la
confirmación. El producto pudo haber caído o no, y **ninguna de las dos puntas
lo sabe**: la expendedora tampoco guarda ese instante.

**Decisión: se resuelve a favor del cliente.** `CIERRE` con resultado
`ENTREGA_DESCONOCIDA` y se encola una **devolución** hacia el backend, marcada
como *presunta* para que el operador pueda revisarla.

Justificación:

- El error tiene costos **asimétricos**. Devolver de más cuesta el precio de un
  producto (~$18 en el ejemplo del ejercicio). No devolver cuesta un cliente que
  pagó y no recibió nada: reclamo, reputación, y en algunos marcos regulatorios
  una contracargo con penalidad. No son comparables.
- La ambigüedad la introdujimos nosotros (nuestro equipo se cortó), no el
  cliente.
- Es un evento **raro**: requiere un corte en una ventana de segundos dentro de
  una venta. Si dejara de serlo, el propio contador lo delata y el problema pasa
  a ser el corte, no la política de devolución.

**Consecuencia que hay que aceptar:** queda abierto un abuso posible — alguien
que corte la alimentación a propósito justo después de la entrega para forzar
devoluciones. Se mitiga sin cambiar la política: se marca la devolución como
presunta, se cuenta por máquina, y si una máquina supera un umbral (por ejemplo
5 en 30 días) se levanta una alerta para inspección física. Cambiar la política
por defecto para tapar el abuso castigaría a los clientes honestos, que son
casi todos.

---

## E2.5 — Escrituras por venta y vida útil

| Camino | Registros |
|---|---|
| Venta completa | 4 (R1, R2, R3, R4) |
| Pago rechazado | 2 (R1, R4) |
| Rechazo por rango o sesión en curso | **0** (se valida antes de tocar el medio de pago) |

Cuenta para 200 ventas diarias, tomando el peor caso (todas completas):

```
200 ventas × 4 registros            =    800 registros/día
800 × 32 bytes                      = 25,6 KB/día
sector de 4 KB / 32 B               =    128 registros por sector
800 / 128                           =   6,25 sectores llenados por día
```

Con **4 sectores rotando** (16 KB de flash), cada sector se borra
6,25 / 4 = **1,56 veces por día**:

```
100 000 ciclos / 1,56 por día  =  64 000 días  ≈  175 años
```

Aun con **un solo sector**: 100 000 / 6,25 = 16 000 días ≈ **43 años**.

La conclusión importante no es el número: es que **la vida útil de la memoria no
es el límite del equipo**. Y el contraste con el diseño ingenuo (reescribir un
registro fijo, un borrado por escritura → 800 borrados diarios → **125 días**)
muestra que lo que define la vida útil no es *cuántas veces escribís* sino
*con qué granularidad*.

Guardas operativas: el diario también sirve de cola de telemetría (E2.6), así
que se dimensiona por ahí (7 días de autonomía ≈ 5 600 registros ≈ 180 KB, que
entra cómodo en la flash SPI externa que estos controladores ya llevan). Y se
lleva un contador de borrados por sector, reportado por telemetría, para que el
desgaste sea observable en vez de teórico.

---

## E2.6 — Reporte al backend cuando el enlace estuvo caído

**El diario es la cola.** No hay una segunda estructura "cola de envío" en RAM:
esa es exactamente la que se pierde en un reinicio. (En el log del ejercicio E3
se ve el síntoma: `TELEM queue_depth=1180` antes del reset y `queue_depth=0`
después. Mil ciento ochenta eventos evaporados.)

Mecánica:

1. Cada registro lleva `seq` monótono, que sobrevive a los reinicios porque se
   deriva del último `seq` leído del diario al bootear.
2. En NVM hay un puntero `confirmado_hasta`. Se actualiza **de forma perezosa**:
   solo cuando avanzó al menos 32 registros, o pasaron 10 minutos, o el backend
   confirmó un cierre de venta con devolución. Actualizarlo en cada ACK
   convertiría el ahorro de escrituras en nada.
3. Al recuperar el enlace se envía desde `confirmado_hasta + 1` **en orden**, en
   lotes, y se espera confirmación por lote.
4. La clave de idempotencia es `(id_maquina, seq)`. Si el enlace se cae después
   de que el backend recibió pero antes de que llegara la confirmación, el
   reenvío es un duplicado exacto y el backend lo descarta. **El costo de
   reenviar de más es cero; el de no reenviar es perder una devolución.**
5. Prioridad: los eventos con consecuencia contable (`COBRADO_SIN_ENTREGA`,
   `ENTREGA_DESCONOCIDA`) se envían **primero**, fuera del orden estricto. Que
   un log de temperatura llegue tarde no le importa a nadie; que una devolución
   espere seis horas detrás de mil logs, sí.
6. Si el diario se llena antes de recuperar el enlace, se descartan los
   registros **más viejos de menor prioridad** (logs, telemetría) y nunca los
   contables. Y se reporta el descarte como evento propio: perder datos en
   silencio es peor que perderlos.

---

## E2.7 — Corte **durante** la escritura del registro

En flash el borrado deja `0xFF` y la escritura solo puede bajar bits a `0`. Se
usa a favor:

1. Un registro sin escribir se lee como `seq == 0xFFFFFFFF`. Es la marca de
   "fin del diario", sin necesidad de un puntero aparte.
2. El registro se escribe **en dos operaciones**: primero los 30 bytes de
   contenido (con `seq` y todo), y **después**, como última operación, los
   2 bytes de `crc16` sobre esos 30 bytes.
3. Al arrancar se recorre el diario y se valida el CRC de cada registro:

| Lo que se lee | Interpretación |
|---|---|
| `seq = 0xFFFFFFFF` | posición libre → acá termina el diario |
| `seq` válido y CRC correcto | registro **completo y consistente** |
| `seq` válido y CRC incorrecto | **escritura interrumpida** → se ignora, y se ignora todo lo que venga después |

Un corte durante la primera operación deja un registro con CRC inválido → se
descarta. Un corte entre las dos operaciones deja el CRC en `0xFFFF` → tampoco
coincide → se descarta. Un corte durante la segunda deja un CRC parcial → no
coincide → se descarta. **En los tres casos el registro se descarta**, que es
lo correcto: si no llegó a completarse, el hecho que describe no se puede dar
por firme.

La consecuencia es que un registro a medio escribir equivale a "ese hecho no
ocurrió", y el arranque cae en la fila anterior de la tabla de E2.3 — que
siempre resuelve a favor del cliente. La política de recuperación no cambia por
un registro roto.

Detalle que hay que respetar: el registro (32 B) tiene que entrar en una sola
página de programación de la flash y estar alineado a ella, para que la
operación 1 sea atómica a nivel de página. Si no, hay que agregar un byte
centinela al final y validar `centinela + CRC`.

---

## E2.8 — Si el cobro nunca resuelve

**Ya está implementado** (`vcp_session_poll()`, escenario 6 de
`demo_venta.exe`).

- **Plazo: 3 000 ms** desde `pago_iniciar()`. El enunciado dice que el cobro
  tarda 50–900 ms; 3 000 ms es más del triple del peor caso normal, así que un
  vencimiento significa "algo se rompió", no "tardó un poco más".
- **Mientras tanto**, a la expendedora se le contesta:
  - a `STATUS_REQ` → `STATUS_RESP` con estado `0x01 OCUPADO`;
  - a una retransmisión del mismo `VEND_REQUEST` → `VEND_PENDING`;
  - a un `VEND_REQUEST` de otro producto → `VEND_DENY 0x04 sesión en curso`.
  Lo importante: **se sigue contestando**. Si nos calláramos, a los tres
  silencios el master nos declararía fuera de servicio y la máquina dejaría de
  operar con medios de pago (Anexo A.2).
- **Al vencer**: `VEND_DENY 0x05 error interno`, no `0x03 pago rechazado`. No
  sabemos si se cobró; decir "el medio de pago rechazó" sería cerrar la
  ambigüedad del lado equivocado y además taparía el problema en las
  estadísticas. Se cierra la sesión (si no, la máquina queda inutilizable) y se
  encola el episodio para conciliación, exactamente como el caso
  `INTENTO` solo de E2.3.
- Además se cuenta en `n_timeout_pago`. Un solo vencimiento es una anomalía;
  varios por día en la misma máquina son un lector que hay que cambiar.

---

## E2.9 — Reintentar un cobro sin cobrar dos veces

**Con la interfaz provista, no se puede garantizar. Y por eso no reintento.**

`pago_iniciar(uint16_t centavos)` no recibe ninguna referencia de operación. Sin
un identificador que nosotros generemos y que el medio de pago respete, un
segundo `pago_iniciar()` es, para el terminal, **una segunda venta**. No hay
forma de que distinga el reintento del pedido nuevo.

Por eso el diseño **nunca reintenta un cobro automáticamente**. Ante un
vencimiento, cierra con `0x05` y deja el episodio para conciliación. Se pierde
una venta; no se cobra dos veces. La venta perdida la recupera el cliente
apretando el botón otra vez; el doble cobro no lo recupera nadie sin una
devolución manual.

**Lo que quedaría expuesto si igual hubiera que reintentar**, en orden de
preferencia:

1. **Consultar antes de reintentar.** Si el terminal expone "última transacción"
   (importe, hora, resultado), se consulta y solo se reintenta si reporta que la
   anterior no fue autorizada. Reduce la exposición a la ventana entre la
   consulta y el reintento (milisegundos), pero no la elimina.
2. **Cambiar la interfaz**, que es la solución de fondo:
   `pago_iniciar(centavos, ref_operacion)` con `ref_operacion` generada por
   nosotros y persistida en R1 **antes** del primer intento. El terminal
   devuelve la autorización original ante una `ref` repetida. Es idempotencia
   estándar de medios de pago y es lo que propondría al equipo que mantiene esa
   capa.
3. **Sin ninguna de las dos**: queda expuesto el doble cobro, y hay que decirlo
   así. La mitigación es de detección, no de prevención: registrar cada intento
   en el diario con importe y marca de tiempo, y hacer que el backend levante
   una alerta ante dos autorizaciones de la misma máquina por el mismo importe
   dentro de una ventana de 60 segundos. No evita el problema, lo hace visible
   en horas en vez de en el resumen de tarjeta del cliente.
