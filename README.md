# Eden Bridge PS5

Eden Bridge recibe la IP, el puerto y el apodo de una sala de Eden Rooms y los guarda en la configuración de ProsperoEden. La versión C experimental se compila con un SDK de payloads para PS5. El ELF de esta versión se compiló para PS5 y el guardado se verificó con configuraciones temporales en Linux.

- [Versión experimental v0.3.4-experimental](https://github.com/hectorsequera65-ops/EdenBridge-PS5/releases/tag/v0.3.4-experimental)
- [Versiones anteriores](https://github.com/hectorsequera65-ops/EdenBridge-PS5/releases)
- [ELF original v0.1.0-original](https://github.com/hectorsequera65-ops/EdenBridge-PS5/releases/tag/v0.1.0-original)

## Flujo de Eden Rooms

1. En una PS5 de pruebas con un cargador ELF compatible, iniciá el payload y anotá la IP mostrada en la notificación. El servicio escucha en IPv4 en el puerto TCP **24879**. Usalo en una red privada de confianza.
2. Cerrá ProsperoEden antes de enviar. Seleccioná una sala en [Eden Rooms](https://eden-rooms.pages.dev/), ingresá la IP de la PS5 y tu apodo, y pulsá **Enviar a PS5** una vez.
3. La misma pestaña abre el puente y vuelve automáticamente a Eden Rooms cuando el puente confirma que guardó la sala. Si falla el guardado, el puente muestra el motivo y permite **Reintentar**. Después del guardado, abrí ProsperoEden para usar los nuevos datos.

Eden Rooms registra una intención aleatoria en el almacenamiento de sesión de la pestaña antes de navegar al puente. El puente genera un código aleatorio ligado a la intención y a la IP, al puerto y al apodo de la sala. Devuelve el código a la URL fija de Eden Rooms, en el fragmento. Eden Rooms verifica la intención pendiente de esa pestaña antes de devolver el código al puente seleccionado. El puente acepta el guardado únicamente mediante un POST de su propio origen, con el código y los datos exactos. La autorización para el primer guardado vence a los cinco minutos. El flujo funciona cuando el navegador omite `Referer` y la pestaña carece de `window.opener`.

Para ingresar los datos directamente desde la red local, abrí `http://IP-DE-LA-PS5:24879/`.

La confirmación exige una solicitud del mismo origen y un token aleatorio por formulario asociado a la IP y al puerto de la sala. Después del primer guardado, los reintentos deben incluir también el mismo apodo. El navegador reintenta una vez los fallos transitorios con los mismos datos. El puente conserva los resultados de los 64 formularios más recientes para que un reintento del mismo formulario tras perder la respuesta no repita la escritura, el respaldo ni la notificación. El servidor comprueba `Host` contra la dirección local de la conexión. Los POST JSON de otro origen se limitan a `https://eden-rooms-ps5.pages.dev`. La validación de origen protege el flujo de navegador. El tráfico HTTP del puente requiere una LAN de confianza. Los equipos con acceso directo a esa red pueden enviar solicitudes HTTP sin las restricciones del navegador.

| Ruta | Comportamiento |
| --- | --- |
| `GET /v1/health` | Estado del servicio. |
| `GET /v1/version` | Identifica la versión activa y la plataforma (`ps5` o `linux`). |
| `GET /v1/config-check` | Comprueba si el archivo coincide con una estructura de escritura admitida. |
| `GET /v1/transfer` | Valida la sala y devuelve un código aleatorio al sitio fijo de Eden Rooms. |
| `GET /v1/receive` | Prepara el guardado en la misma pestaña con el código devuelto por Eden Rooms. |
| `GET /v1/prepare` | Valida la sala y presenta el formulario de guardado. Admite un apodo inicial opcional. |
| `POST /v1/preview` con JSON | Valida y devuelve `saved: false`. Acepta `host`, `port` y opcionalmente `nick` o `nickname`. |
| `POST /v1/connection` con `application/json` | Mantiene la vista previa del prototipo Python. |
| `POST /v1/connection` con `text/plain` | Devuelve `409` para indicar que se requiere confirmación. El frontend antiguo mostraría un guardado exitoso si recibiera `ok: true`. |
| `POST /v1/preview` con formulario | Guarda tras validar la confirmación, IPv4, puerto de 1 a 65535 y apodo de 1 a 24 caracteres ASCII entre letras, números, guion y guion bajo. Devuelve `saved: true` tras confirmar la persistencia. |

## Archivos de ProsperoEden

El payload usa `/data/prosperoeden/config/prosperoeden.json`. Reconoce `multiplayer.host`, `multiplayer.port` y un único `nickname` en `multiplayer` o en el objeto que lo contiene. En la estructura verificada con una copia de una PS5 con firmware 6.02, `profiles.current` selecciona el bloque activo dentro de `profile_settings`. Los campos `profiles.first` y `profiles.users` se conservan. También se admite el formato anterior con `activeProfile` y el mapa `profiles`. En ambos formatos se actualizan los tres campos del bloque general y del perfil activo.

El servidor lee encabezados y cuerpo por fragmentos hasta completar los bytes indicados por `Content-Length`, con un límite total de cinco segundos. Si falla la lectura, la respuesta y el registro indican la etapa y el código. Un plazo agotado devuelve HTTP 408. Las solicitudes ambiguas o incompletas se rechazan antes del guardado.

Si falla la validación o la escritura, la respuesta indica la etapa, el destino lógico y el código `errno` cuando corresponde. El registro del payload incluye el mensaje del sistema. Al consultar `/v1/config-check` se comprueban la lectura y la estructura del JSON sin realizar escrituras. Las pruebas de escritura de esta versión se ejecutaron sobre copias locales.

El puente rechaza configuraciones ambiguas, archivos no regulares y enlaces simbólicos en la ruta del archivo. Cambia solo los valores de los campos reconocidos del bloque general y del perfil activo. Conserva el resto del JSON byte por byte. Guarda el contenido anterior en `prosperoeden.json.edenrooms.bak` con permisos `0600`, sincroniza el respaldo y reemplaza la configuración mediante un temporal del mismo directorio y un renombrado atómico. El respaldo corresponde al estado inmediatamente anterior al último guardado. La edición concurrente desde ProsperoEden queda fuera del flujo admitido.

## Compilación y pruebas

El SDK utilizado es el [fork hectorsequera65-ops/sdk en la revisión 52bed4e](https://github.com/hectorsequera65-ops/sdk/tree/52bed4e91afd678f5d03c881a91be0a75de18a06). GitHub Actions usa LLVM 19 o 18, construye esa revisión y publica el ELF y su SHA-256 como artefactos del job.

```bash
git clone https://github.com/hectorsequera65-ops/sdk.git
git -C sdk checkout 52bed4e91afd678f5d03c881a91be0a75de18a06
make -C sdk LLVM_CONFIG=llvm-config-19 DESTDIR="$PWD/ps5-payload-sdk" install

# Desde el repositorio EdenBridge-PS5
make ps5 PS5_PAYLOAD_SDK=/ruta/al/ps5-payload-sdk
make test
```

El resultado es `build/EdenBridge-PS5.elf`. Las pruebas ejecutan la reconstrucción de Python y el servidor C de Linux en loopback con un JSON temporal. Para probar el servidor C manualmente, ejecutá `make local` y luego `build/eden-bridge-local /ruta/a/una-copia-de-prosperoeden.json 24879`.

## Análisis del ELF original

`readelf -h -d -Ws` identifica un ELF x86-64 PIE sin tabla de símbolos estática, con entrada en `0x24d4` e importaciones dinámicas como `bind`, `recv`, `sceKernelSendNotificationRequest`, `fsync` y `rename`. `objdump -d` muestra el puerto `0x612f` (24879) y la escucha IPv4 con dirección cero. El código prepara una solicitud de notificación de `0xc30` bytes. Las comparaciones alrededor de `0x1ceb`, `0x1e4b` y `0x2380` identifican `multiplayer`, `nickname`, `host` y `port`. Las cadenas contienen el formulario de confirmación, el origen permitido y las rutas del JSON, el respaldo `.edenrooms.bak` y el temporal `.edenrooms.tmp`.

Las cuatro copias locales de `EDEN_BRIDGE.md` tienen el mismo SHA-256 y describen el prototipo HTTP en loopback. La reconstrucción Python conserva ese protocolo de prueba. La versión C usa también el flujo LAN de confirmación identificado en el ELF y en el frontend de Eden Rooms.

```text
SHA-256 del ELF original  3fecc13c40cd62fb44fec300830508e3a3511993556e6ee96a65e40dea4e03cb
Tamaño                    115952 bytes
```
