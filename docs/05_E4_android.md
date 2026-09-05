# E4 — Plan de incorporación de Android

App interna para el técnico de campo: se conecta al controlador por Bluetooth,
lee estado, muestra los últimos eventos y fuerza una prueba de entrega.

No tengo experiencia en Android. Lo que sigue es cómo me organizaría, no lo que
ya sé.

---

## E4.1 — Cómo llegaría a una primera versión funcional

**La premisa que ordena el plan: el riesgo no está en Android, está en el
enlace.** Aprender a dibujar una pantalla en Android es trabajo acotado y con
muchísimo material. Que un teléfono cualquiera se conecte de forma confiable
por BLE a un periférico propio, en un pasillo de supermercado, al lado de un
compresor, es lo que puede hundir el proyecto. Así que ataco eso primero,
aunque sea la parte que menos se parece a una app.

| Etapa | Qué | Criterio de terminado |
|---|---|---|
| **0. Definir el protocolo sobre BLE** (antes de escribir la app) | Un servicio GATT con dos características: *comando* (write) y *respuesta* (notify). El payload es **una trama VCP-1**, con el mismo formato y el mismo CRC que ya está implementado y probado. | Documento de una carilla acordado con quien mantiene el firmware |
| **1. Aguja técnica, descartable** | App fea de una pantalla y un botón: escanear, conectar, mandar `STATUS_REQ`, mostrar el `STATUS_RESP` en crudo. Sin arquitectura, sin tests, sin diseño. | El hexadecimal correcto en pantalla, en **tres teléfonos distintos** |
| **2. Confiabilidad del enlace** | Reconexión, timeouts, qué pasa al alejarse, al bloquear la pantalla, al entrar una llamada. | 50 conexiones seguidas sin intervención manual |
| **3. Las tres funciones** | Estado, últimos eventos, prueba de entrega. En ese orden: leer es más fácil que leer mucho, y mucho más fácil que escribir algo que mueve un motor. | Un técnico real la usa en una máquina real |
| **4. Recién acá, la app "de verdad"** | Estructura, manejo de estados, errores presentados como algo que un técnico puede accionar, empaquetado y distribución interna. | — |

**Lo que dejo explícitamente para después:** diseño visual, soporte de tablets,
modo oscuro, i18n, y cualquier función que no haya pedido un técnico. También
dejo para después la autenticación fuerte de la prueba de entrega —pero **no**
la autenticación mínima: la versión 1 ya tiene que impedir que cualquiera con
un celular haga caer un producto (ver riesgo R3).

**Cómo aprendo mientras tanto:** el 80 % del tiempo sobre el problema real, no
sobre tutoriales. La etapa 1 es a propósito descartable: sirve para chocarse con
lo que no se sabe, no para conservar código.

---

## E4.2 — Qué de mi experiencia es transferible y qué no

**Transferible, y es la parte difícil del proyecto:**

- Diseñar y depurar un protocolo sobre un medio poco confiable. El BLE de un
  teléfono se parece más a la línea ruidosa del Anexo A.2 que a una API HTTP:
  se pierden paquetes, hay reintentos, hay estados que sobreviven a la
  desconexión. Todo el razonamiento de E1 aplica.
- Pensar en máquinas de estados explícitas, con los caminos de error como
  estados de primera clase y no como `catch`.
- Instrumentar para diagnosticar: sin un log del lado del teléfono correlacionado
  con el del controlador, el primer bug intermitente cuesta una semana (E3).
- La mitad del firmware: el lado del controlador de esta app lo escribo yo, y
  ahí ya reuso `vcp_build()` y `vcp_rx` tal cual.
- Saber qué **no** hacer: no bloquear en el hilo equivocado, no asumir que un
  mensaje enviado llegó, no confiar en el orden.

**No transferible, y lo asumo:**

- El ciclo de vida de Android (actividades, procesos que el sistema mata,
  restricciones en segundo plano). Es la fuente clásica de bugs de quien viene
  de embebido, porque el sistema operativo te saca el control por debajo.
- El modelo de permisos, que cambió varias veces y donde BLE arrastra permisos
  de ubicación por motivos históricos.
- Kotlin, corrutinas y el ecosistema de UI. Es aprendible en semanas; no es el
  riesgo.
- La **fragmentación de fabricantes**: el stack BLE de un Samsung no se comporta
  igual que el de un Xiaomi. Eso no se deduce, se descubre probando.

---

## E4.3 — Los riesgos que más me preocupan, y cómo los bajo temprano

| | Riesgo | Por qué me preocupa | Cómo lo reduzco en la primera semana |
|---|---|---|---|
| **R1** | **El BLE no es confiable de forma pareja entre teléfonos.** | Es el riesgo que puede hacer fracasar el proyecto entero después de dos meses de trabajo, y no se descubre programando: se descubre probando en hardware variado. Si el módulo elegido no anda bien con cierto stack, la decisión (cambiar módulo) es cara y hay que tomarla temprano. | La etapa 1 es exactamente esto y va **primera**. Probar en al menos tres teléfonos de fabricantes distintos, incluido el modelo más viejo que usen los técnicos. Medir tiempo de conexión y tasa de fallo. Si el resultado es malo, replantear el transporte (BLE clásico SPP, o directamente USB-C) **antes** de construir nada encima. |
| **R2** | **Depurar dos sistemas a la vez.** Cuando algo no funciona, la primera pregunta es "¿el teléfono no mandó, o el controlador no contestó?", y sin evidencia se pierden días. | Es el escenario de E3 en versión chica y se repite en cada bug. | Desde el primer commit: log persistido en las dos puntas, con un identificador de sesión compartido y marcas de tiempo. Y un modo del firmware que vuelque por la UART todo lo que entra por BLE, para poder mirar las dos puntas con un cable. |
| **R3** | **"Forzar una prueba de entrega" es una función que mueve plata y producto.** Un bug —o un celular ajeno— hace caer mercadería. | Es la única función de la app con consecuencia física. El resto solo lee. | En la versión 1: la prueba de entrega exige que el controlador esté en un modo mantenimiento (que se activa físicamente, con la puerta abierta), la operación se registra en el diario con quién la pidió, y queda fuera del reporte de ventas. La app no es la barrera de seguridad; **la barrera está en el firmware**, que es lo que controlo. |

Un cuarto, menor pero real: **distribución interna**. Una app que no es de la
tienda necesita un canal de instalación y actualización para los técnicos. Es
trabajo de plataforma que suele aparecer tarde y frenar la entrega; lo pongo en
la etapa 4 pero lo pregunto en la semana 1.

---

## E4.4 — Qué necesitaría de ustedes

1. **Hardware desde el día uno**: un controlador con el módulo BLE que van a
   usar en producción y, si es posible, una expendedora de banco. Sin eso la
   etapa 1 no existe.
2. **Dos o tres teléfonos representativos de los que usan los técnicos**,
   incluido el más viejo. No los míos: los de ellos.
3. **Media hora con un técnico de campo**, antes de diseñar nada. Qué mira
   primero cuando llega a una máquina, qué decide con eso, cuánto tarda hoy.
   Eso define qué va en la primera pantalla mejor que cualquier reunión de
   producto.
4. **Quién decide sobre el firmware del controlador**, porque la mitad de este
   proyecto es firmware y el protocolo BLE hay que acordarlo con esa persona.
5. **Reglas de la casa para Android**: si ya hay una app interna, con qué está
   hecha, cómo se distribuye y qué política de versiones mínimas soportan. No
   quiero inventar una convención paralela.
6. **Alguien a quien preguntar Android** —aunque sea media hora por semana, de
   otra empresa o externo. Acelera muchísimo el tramo en que uno no sabe ni qué
   está buscando en Google.
7. **Acordar el criterio de "primera versión"** por escrito. Sin eso, una app
   interna crece sin límite y no se entrega nunca.
