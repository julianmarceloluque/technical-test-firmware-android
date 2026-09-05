# E3 — Diagnóstico del incidente en campo

## Lo que el log cuenta antes de hipotetizar

```
13:42:07.114  VEND_REQ  sel=24 price=1800
13:42:07.240  PAY  auth_ok txid=8821          ← 126 ms: cobro normal
13:42:07.244  TX  VEND_APPROVE txid=8821      ← 4 ms: dentro del plazo, todo bien
13:42:07.310  RX  ---- (sin respuesta)        ← 66 ms después no llega nada
13:42:09.310  TIMEOUT vend_confirm txid=8821  ← 2,07 s esperando confirmación
13:42:09.311  TELEM queue_depth=1180          ← 1 ms después
13:42:09.480  BOOT  reset_cause=WDT           ← 169 ms después: watchdog
13:42:11.902  BOOT  ok fw=2.4.1               ← 2,42 s de arranque
13:42:12.007  TELEM queue_depth=0             ← la cola desapareció
```

**Hasta `TX VEND_APPROVE` el controlador hizo todo bien.** El cobro salió, la
respuesta salió a tiempo. Lo que falla es lo que viene después.

Y hay un dato que ordena todo lo demás: entre `.310` (cobro hecho, sin
confirmar) y `.902` (controlador de vuelta) pasaron **2,6 segundos sin
controlador**. El master reintenta cada 100 ms y a los 3 silencios declara al
medio de pago fuera de servicio (Anexo A.2): a los 300 ms ya nos dio por
muertos. **La máquina nunca entregó porque su medio de pago desapareció con la
plata ya cobrada.** Ese es el mecanismo del "cobro sin entrega".

---

## E3.3 — La pista fuerte

**`TELEM queue_depth=1180` un milisegundo antes del reset, y `queue_depth=0`
después.**

Dos cosas en un solo par de líneas:

1. **La cola vive en RAM.** Un reinicio la borra entera. Los 1 180 eventos
   pendientes —incluida esta venta cobrada y sin confirmar— **nunca llegaron al
   backend**. Por eso el problema tardó cinco días en detectarse: no estaba
   invisible por falta de instrumentación, estaba invisible porque *la
   evidencia se borraba sola en el mismo evento que había que investigar*.
2. **1 180 es un número enorme y no es casual que aparezca justo antes del
   WDT.** Que el reset caiga a 169 ms de haber reportado esa profundidad
   sugiere que **algo cuyo costo crece con el tamaño de la cola** se comió la
   ventana del watchdog: un recorrido O(n), una compactación, un `memmove` del
   arreglo entero, una serialización de la cola completa, o directamente
   agotamiento de heap. El disparador del reset no es la venta: es la cola.

Pista secundaria, más chica pero útil: **el arranque tarda 2,42 s**. Aunque se
arregle la causa del reset, un arranque tan largo convierte cualquier reinicio
en un cobro sin entrega. Es un segundo frente independiente.

Y una pista **negativa** que descarta cosas: `reset_cause=WDT`. Si fuera un
problema de alimentación esperaríamos `BOR`/`POR`, no watchdog.

---

## E3.1 y E3.2 — Hipótesis

Ordenadas por probabilidad × costo de descartarlas (primero las probables y
baratas).

### H1 — Regresión de fw 2.4.1: un camino cuyo costo crece con la cola dispara el watchdog

*La más probable, y la más barata de confirmar: es leer un diff.*

| | |
|---|---|
| **A favor** | Los reinicios "no aparecían antes" y el despliegue fue hace 6 días, los reclamos empezaron hace 5. `reset_cause=WDT` (no alimentación). El WDT cae 169 ms después de reportar `queue_depth=1180`. |
| **En contra** | La actualización fue a las 60 máquinas y solo 8 fallan. **Por sí sola no explica el recorte**: necesita un cofactor que haga crecer la cola solo en esas 8 (ver H2). |
| **Qué pediría / mediría** | (a) `git diff 2.4.0..2.4.1` acotado a la cola de telemetría y al lazo que patea el watchdog. (b) Reproducción en banco: precargar la cola a 1 200 eventos y medir el peor tiempo de vuelta del lazo principal. (c) Distribución de `queue_depth` en el instante previo a cada reset, en las 8. |
| **Qué esperaría** | Si es esto: un umbral nítido de `queue_depth` a partir del cual el tiempo de vuelta supera la ventana del WDT, y **cero resets** con la cola vacía. Si no es esto: los resets aparecerían repartidos en cualquier `queue_depth`. |

### H2 — El enlace celular está degradado en esas 8 máquinas (el cofactor que falta)

| | |
|---|---|
| **A favor** | La cola solo crece si no se puede vaciar. 1 180 eventos son horas de acumulación. La franja 12–15 h coincide con el pico de tráfico celular y con el pico de ventas (la cola crece por las dos puntas). Explica por qué solo 8 de 60 con el mismo firmware. |
| **En contra** | Las 8 son del mismo modelo de expendedora, lo cual apunta a algo de la máquina y no de la antena. Y hay 14 máquinas del mismo modelo sin reclamos, así que el modelo tampoco alcanza: puede ser pura coincidencia de una muestra chica. |
| **Qué pediría** | RSSI/RSRP, tasa de reintentos y minutos sin ACK del backend por máquina y por hora, de las 60. Y la **ubicación física** de las 22 del modelo sospechoso (subsuelo, interior de edificio, operador celular). |
| **Qué esperaría** | Si es esto: las 8 aparecen en la cola de peor señal y con ventanas sin ACK de horas, justo en 12–15 h; las 14 del mismo modelo sin reclamos tienen señal normal. Si no: la señal de las 8 es indistinguible de las otras 52 y hay que buscar otra explicación al crecimiento de la cola (por ejemplo, que esas 8 *generen* más eventos). |

### H3 — El estado de la venta no sobrevive al reinicio, así que no hay devolución automática

*No causa el cobro sin entrega, pero explica su costo y su invisibilidad.*

| | |
|---|---|
| **A favor** | `queue_depth=0` tras el boot. El `TIMEOUT vend_confirm` quedó en una cola volátil. Nadie generó una devolución: los reclamos llegan **por el cliente**, no por el sistema. |
| **En contra** | Ninguna evidencia disponible la contradice; lo único que falta es confirmar si el fw 2.4.1 persiste algo en NVM (puede que persista y el problema sea solo de la cola). |
| **Qué pediría** | Volcado de la NVM de una máquina afectada y el mapa de qué escribe 2.4.1. Cruce de las transacciones del procesador de tarjetas contra las ventas reportadas al backend, en las 8, de los últimos 5 días. |
| **Qué esperaría** | Si es esto: el cruce muestra **más cobros que ventas reportadas**, y la diferencia coincide en cantidad y horario con los reclamos. Ese cruce, además, encuentra a los clientes que **no** reclamaron, que son la mayoría. |

### H4 — Alimentación o temperatura: pico de consumo del compresor entre 12 y 15 h

| | |
|---|---|
| **A favor** | La franja horaria encaja perfecto con el mediodía (calor, compresor al máximo, más gente). Un modelo de expendedora concreto puede tener una fuente al límite. |
| **En contra** | **`reset_cause=WDT`**. Un brownout se reporta como `BOR`/`POR`. Y los reinicios "no aparecían antes" del despliegue: la fuente no cambió hace 6 días. |
| **Qué pediría** | Registro de tensión de alimentación y temperatura interna del controlador muestreados a 1 Hz durante 24 h en dos máquinas afectadas y una sana del mismo modelo. Y el histograma de `reset_cause` de la flota. |
| **Qué esperaría** | Si es esto: caídas de tensión correlacionadas con los resets y presencia de `BOR` entre las causas. Si no (lo más probable): tensión estable y **100 % de los resets con causa WDT**, lo que la descarta casi por completo. |

### H5 — Ruido en el bus VCP-1 en ese modelo de expendedora

| | |
|---|---|
| **A favor** | `RX ---- (sin respuesta)`: la máquina dejó de hablarnos. Un bus ruidoso da tramas perdidas y timeouts. Las 8 comparten modelo, o sea cableado, longitud de bus y masa. |
| **En contra** | El silencio arranca **después** de nuestro TX y dura 2 s: eso es un controlador ausente, no una trama corrupta suelta. Y el ruido de bus no explica un `reset_cause=WDT` ni el `queue_depth=1180`. Tampoco explica que empiece hace 5 días. |
| **Qué pediría** | Los contadores del receptor (`err_crc`, `err_len`, `err_framing`, `err_timeout`, tramas recuperadas por re-sincronización) por máquina y por hora — exactamente los que expone el diseño de E1. Y una captura con analizador lógico en una de las 8. |
| **Qué esperaría** | Si es esto: `err_crc`/`err_framing` órdenes de magnitud más altos en las 8 que en las 52, en cualquier horario. Si no: contadores comparables, y entonces el ruido es a lo sumo un agravante. |

### H6 — Los reclamos no son todos el mismo fenómeno

| | |
|---|---|
| **A favor** | 2 % es mucho. En cualquier flota hay un fondo de productos trabados en el espiral, que también se viven como "pagué y no salió". |
| **En contra** | El fondo de reclamos por atasco no se concentra en 8 máquinas, ni aparece de golpe a los 5 días de un despliegue. |
| **Qué pediría** | Separar los reclamos por máquina, por selección y por hora. Y aclarar **sobre qué base está calculado el 2 %**: si es sobre las ventas de las 60, en las 8 el fenómeno es del orden del 15 %, que es una escala completamente distinta y cambia la urgencia. |
| **Qué esperaría** | Si hay dos poblaciones: un grupo repartido en toda la flota y en todo el día (atascos) y otro concentrado en las 8 entre 12 y 15 h (el nuestro). Vale la pena separarlas antes de medir si un arreglo funcionó. |

**Hipótesis de trabajo** (H1 + H2 + H3 encadenadas): el enlace degradado de esas
8 hace crecer una cola que 2.4.1 recorre con un costo proporcional a su tamaño;
al superar cierta profundidad se dispara el watchdog; el reinicio deja al
controlador 2,6 s fuera del bus con el cobro ya hecho, la expendedora lo da por
fuera de servicio y no entrega; y como la cola era volátil, el episodio no llega
nunca al backend.

---

## E3.4 — Las próximas 2 horas: contención

**Contención** es cortar el daño sin saber la causa. **Solución** es que no
vuelva a pasar. Lo de las próximas 2 horas es todo contención.

| Orden | Acción | Por qué primero |
|---|---|---|
| 1 | **Cruzar transacciones del procesador de tarjetas contra ventas reportadas**, en las 8, de los 5 días. Devolver **de oficio** todo lo que aparezca cobrado sin venta. | Es lo único que atiende al cliente que ya perdió la plata y no reclamó. No depende de entender la causa. Y produce el número real del problema. |
| 2 | **Rollback a 2.4.0 en las 8 máquinas.** | Los reinicios no existían antes del despliegue: es la reversión de mayor probabilidad de éxito y es reversible. Sobre 8 equipos es de bajo riesgo. |
| 3 | Si el rollback no está disponible en 2 horas: **deshabilitar el pago con tarjeta en las 8** (siguen vendiendo en efectivo). | Sacrifica facturación para llevar los cobros sin entrega a **cero**. Es la decisión incómoda que hay que estar dispuesto a tomar: seguir cobrando mientras se investiga es lo caro. |
| 4 | **Alerta en toda la flota** sobre `queue_depth` y `reset_cause`, con umbral en, digamos, 300 eventos. | Las 8 son las que ya explotaron. La alerta dice cuáles son las próximas, antes de que generen reclamos. |
| 5 | Congelar el despliegue de 2.4.1 a cualquier máquina que falte, y no promover 2.4.2. | Elemental mientras haya una regresión sin explicar. |

Y lo que **no** haría en esas 2 horas: publicar una causa raíz. Con lo que hay
alcanza para contener, no para concluir.

**Solución** (después, con causa confirmada): cola persistida en NVM con las
operaciones acotadas en tiempo (E2.6); diario de sesión para reconstruir la
venta al arrancar y emitir la devolución sola (E2.3, E2.4); reducir el tiempo de
arranque para que un reinicio no exceda la ventana de 3 reintentos del master
(300 ms) — y si no se puede, que el bus quede en un estado que no haga fallar la
entrega; y una prueba de regresión que corra el lazo con la cola llena.

---

## E3.5 — Qué instrumentaría para que la próxima vez lleve una hora

El principio: **si el síntoma borra su propia evidencia, no hay diagnóstico
posible.** Todo lo de abajo apunta a que el estado sobreviva al reinicio y sea
observable sin ir a la máquina.

1. **Caja negra en NVM**: al detectar reset por WDT, persistir `reset_cause`, el
   PC/LR del fallo, los últimos N eventos y una foto de los contadores. Hoy
   sabemos `WDT` y nada más.
2. **Peor tiempo de vuelta del lazo principal**, con máximo histórico y el
   identificador de la etapa donde se midió, reportado en cada latido. **Es el
   número que hubiera delatado esto en una hora**: habría mostrado el tiempo de
   vuelta creciendo con `queue_depth` días antes del primer reclamo.
3. **La cola de telemetría en NVM** con `profundidad`, `máximo histórico`,
   `descartes` y `minutos desde el último ACK del backend`. Los descartes
   silenciosos son deuda invisible.
4. **Alarma de "reinicio con venta abierta"** como evento de severidad alta, no
   como línea de log: es literalmente el evento "cobré y no sé si entregué".
5. **Contadores del receptor VCP-1** —`err_crc`, `err_len`, `err_framing`,
   `err_timeout`, tramas recuperadas, respuestas omitidas por plazo— por hora.
   Ya existen en el diseño de E1; falta reportarlos. Separan "la línea está
   sucia" de "el firmware está roto", que es la primera bifurcación de cualquier
   diagnóstico en un bus.
6. **Un identificador de venta (`txid`) que viaje en todas las líneas de log**,
   de punta a punta, para poder reconstruir un episodio sin adivinar por marca
   de tiempo.
7. **Tiempo de arranque medido y reportado.** 2,42 s es un dato que nadie estaba
   mirando y es la mitad del mecanismo del daño.
8. **Comparación automática por cohorte de firmware**: tasa de resets por
   máquina-día de 2.4.1 contra 2.4.0. Un despliegue que duplica los reinicios
   tiene que avisar solo, el mismo día, sin que nadie lo pregunte.

---

## E3.6 — Qué le comunicaría al cliente hoy

> Confirmamos el problema y ya lo acotamos a 8 máquinas: en ciertas condiciones
> el controlador se reinicia justo después de cobrar y la máquina no llega a
> entregar. Está relacionado con la actualización de la semana pasada y hoy
> mismo la revertimos en esas 8; el resto de la flota queda bajo monitoreo con
> una alerta nueva.
>
> No esperamos a los reclamos: estamos cruzando las transacciones de tarjeta
> contra las ventas registradas de los últimos 5 días y devolviendo de oficio
> todo cobro sin entrega, incluidos los que nadie reclamó.
>
> Mañana les enviamos el detalle de las devoluciones y el plan de corrección
> definitivo, que incluye que estos episodios queden registrados y se devuelvan
> automáticamente aunque el equipo se reinicie o se quede sin conexión.
