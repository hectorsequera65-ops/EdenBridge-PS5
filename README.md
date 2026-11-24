# Eden Bridge PS5

Eden Bridge recibe por HTTP los datos de una sala de Eden Rooms para ProsperoEden. Este repositorio conserva el ELF original y una reconstrucción parcial del protocolo en Python 3.

## ELF original para PS5

El binario publica un servicio HTTP en el puerto TCP **24879**. En el desensamblado, la llamada a `bind` recibe una dirección IPv4 con la IP en cero, que corresponde a escuchar en todas las interfaces IPv4 disponibles. El binario contiene una pantalla de confirmación y mensajes de validación y de guardado de IP, puerto y apodo. También contiene las rutas del archivo `/data/prosperoeden/config/prosperoeden.json`, su respaldo `.edenrooms.bak` y su archivo temporal `.edenrooms.tmp`. El análisis estático identifica estos elementos. La ejecución y el comportamiento de escritura en consola quedan sin verificar.

Las cuatro copias locales de `EDEN_BRIDGE.md` tienen contenido idéntico. Documentan un prototipo con destino de loopback, `POST /v1/connection`, cuerpo JSON con `host` y `port`, y comprobación de origen. La escucha en LAN y los mensajes de escritura encontrados en el ELF amplían lo descrito en ese documento.

### Instalación y uso

1. Descarga [EdenBridge-PS5.elf](https://github.com/hectorsequera65-ops/EdenBridge-PS5/releases/download/v0.1.0-original/EdenBridge-PS5.elf) y verifica su SHA-256.
2. En una PS5 que ya disponga de un cargador de ELF compatible, usa su opción habitual para cargar el archivo. El binario es un ELF x86-64 PIE con bibliotecas `.sprx` de PS5. La compatibilidad por firmware y cargador está pendiente de comprobar.
3. Si el inicio muestra un aviso con la IP y el puerto de la consola, comprueba `http://<IP-DE-LA-PS5>:24879/v1/health` desde la misma red. Mantén el ELF activo durante el uso y limita el acceso a una red privada de confianza.
4. Antes de probar el guardado de una sala, cierra ProsperoEden y guarda una copia de su configuración. Tras confirmar la sala, abre ProsperoEden y comprueba el destino.

```text
SHA-256  3fecc13c40cd62fb44fec300830508e3a3511993556e6ee96a65e40dea4e03cb
Tamaño   115952 bytes
```

## Reconstrucción parcial en Python

`bridge.py` es una implementación nueva, basada en las rutas y mensajes del ELF y en el contrato de `EDEN_BRIDGE.md`. Corre en un equipo con Python 3 y escucha en `127.0.0.1`. Expone `GET /v1/health`, una vista previa JSON en `GET /v1/prepare` y acepta JSON en `POST /v1/connection` y `POST /v1/preview`. Valida el origen de los POST, la dirección IPv4 y el puerto. Todas las respuestas exitosas incluyen `writeEnabled: false`.

El alcance de esta reconstrucción es el protocolo de prueba local. La interfaz HTML, el apodo, la edición de perfiles y la integración nativa con PS5 siguen pendientes. El ELF publicado procede del archivo original y tiene símbolos de depuración eliminados.

```bash
git clone https://github.com/hectorsequera65-ops/EdenBridge-PS5.git
cd EdenBridge-PS5
python3 bridge.py --origin https://example.test
```

En otra terminal:

```bash
curl -fsS http://127.0.0.1:24879/v1/health
curl -fsS http://127.0.0.1:24879/v1/connection \
  -H 'Origin: https://example.test' \
  -H 'Content-Type: application/json' \
  --data '{"host":"192.0.2.1","port":24872}'
```

GitHub Actions compila los archivos Python a bytecode y ejecuta las pruebas HTTP en loopback.
