Traducción:

# DuckStation VR - Emulador de PlayStation 1, también conocido como PSX, con soporte OpenXR VR

[Características](#características) | [Soporte VR](#soporte-vr) | [Descarga y Ejecución](#descarga-y-ejecución) | [Compilación](#compilación) | [Avisos Legales](#avisos-legales)

> **Este es un fork VR de [DuckStation](https://github.com/stenzek/duckstation).** Agrega renderizado estereoscópico 3D vía OpenXR usando geometría real del PS1 capturada del GTE (Geometry Transformation Engine). Basado en el trabajo de captura de vértices de [scurest/duckstation-3D-Screenshot](https://github.com/scurest/duckstation-3D-Screenshot).

**Versiones upstream:** https://github.com/stenzek/duckstation/releases/tag/latest

**Wiki:** https://www.duckstation.org/wiki/

DuckStation es un simulador/emulador de la consola Sony PlayStation(TM), enfocado en jugabilidad, velocidad y mantenimiento a largo plazo. El objetivo es ser lo más preciso posible, manteniendo un rendimiento adecuado para dispositivos de bajo rendimiento. Las opciones de "hack" no son recomendadas, la configuración por defecto debería soportar todos los juegos jugables, con solo algunas de las mejoras teniendo problemas de compatibilidad.

Una imagen ROM del "BIOS" es necesaria para iniciar el emulador y jugar. Puedes usar una imagen de cualquier versión de hardware o región, aunque regiones de juegos y regiones de BIOS que no coincidan pueden resultar en problemas de compatibilidad. La imagen ROM no se incluye con el emulador por razones legales; debes obtenerla de tu propia consola usando Caetla u otros medios.

## Características

DuckStation cuenta con una interfaz completamente funcional construida con Qt, así como una interfaz de pantalla completa/TV basada en Dear ImGui.

<p align="center">
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main-qt.png" alt="Captura de la Ventana Principal" />
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/bigduck.png" alt="Captura de la Interfaz de Pantalla Completa" />
</p>

Otras características incluyen:

 - Recompilador de CPU/JIT (x86-64, armv7/AArch32 y AArch64).
 - Renderizado por hardware (D3D11, D3D12, OpenGL, Vulkan, Metal) y renderizado por software.
 - Escalado, filtrado de texturas y color verdadero (24 bits) en los renderizadores de hardware.
 - PGXP para precisión de geometría, corrección de texturas y emulación de buffer de profundidad.
 - Filtro de downsampling adaptativo.
 - Cadenas de shaders de post-procesamiento (GLSL y Reshade FX experimental).
 - "Inicio rápido" para saltar la pantalla de arranque/intro del BIOS.
 - Soporte de estados guardados.
 - Soporte para Windows, Linux y macOS.
 - Soporta imágenes bin/cue, archivos bin/img crudos, MAME CHD, ECM de pista única, MDS/MDF y formatos PBP sin encriptar.
 - Inicio directo de ejecutables homebrew.
 - Carga directa de archivos Portable Sound Format (psf).
 - Controles digitales y analógicos.
 - Soporte de lightgun Namco GunCon (simulado con el mouse).
 - Soporte de NeGcon.
 - Interfaz Qt y "Big Picture".
 - Actualizaciones automáticas desde los canales oficiales.
 - Verificación automática de contenido - los títulos/juegos son proporcionados por redump.org.
 - Cambio automático opcional de tarjetas de memoria para cada juego.
 - Soporta cargar trucos de listas existentes.
 - Editor de tarjetas de memoria e importador de guardados.
 - Overclock emulado de CPU.
 - Depuración integrada y remota.
 - Controles multitap (hasta 8 dispositivos).
 - RetroAchievements.
 - Carga/aplicación automática de parches PPF.
 - **Renderizado estereoscópico VR vía OpenXR** con head tracking, inyección de cámara y soporte de control Quest Touch (ver [Soporte VR](#soporte-vr)).

## Soporte VR

Este fork agrega renderizado estereoscópico 3D para headsets VR vía OpenXR. En lugar de proyectar el juego en una pantalla virtual, captura geometría 3D real del GTE del PS1 antes de la proyección y la re-renderiza en estéreo desde la perspectiva del headset.

### Cómo funciona

El Geometry Transformation Engine (GTE) del PS1 transforma vértices 3D en coordenadas 2D de pantalla. Este fork intercepta los datos de los vértices antes de la proyección, preservando las posiciones 3D completas. Un sistema de inyección de cámara rota la salida del GTE para que el culling del lado de la CPU del juego funcione con la dirección de visión del VR, permitiendo que la escena completa sea renderizada desde cualquier orientación del headset.

### Características

 - Renderizado estéreo con matrices de vista y proyección por ojo vía swapchains Vulkan de OpenXR.
 - Inyección de cámara para dirección de vista con head tracking (modo primera persona).
 - Captura de texturas PS1 de VRAM con remapeo de 256 slots y uploads Vulkan en lote.
 - Quad layer OpenXR para contenido 2D (menús, pantallas de carga, BIOS).
 - Entrada del control Quest Touch mapeada como gamepad PS1 (A/B/X/Y, triggers, grips, sticks).
 - Tres modos de navegación: Tank, Camera Yaw y Hybrid con snap turn.
 - Escala del mundo, altura de ojos, distancia de pantalla y velocidad de rotación configurables.

### Requisitos

 - Headset Meta Quest (Quest 1/2/3/Pro) con Quest Link o Air Link, o cualquier headset compatible con OpenXR vía SteamVR.
 - Windows 10/11 x64 con GPU compatible con Vulkan.
 - Runtime OpenXR activo (Oculus o SteamVR).
 - PGXP debe estar habilitado para el rastreo de vértices 3D.

### Inicio rápido

1. Compila desde el código fuente (ver [Compilación](#compilando-vr)).
2. Inicia DuckStation y carga un juego.
3. Ve a **Settings > Advanced Settings > Tweaks** y habilita **Enable VR Mode**.
4. Opcionalmente habilita **VR First-Person Mode** para inyección de cámara.
5. Ponte el headset — el juego debería aparecer en 3D estéreo.

### Mapeo de controles (Quest Touch)

| Botón Quest | Entrada PS1 | Botón Quest | Entrada PS1 |
|---|---|---|---|
| A | Cross | Trigger Izquierdo | L2 |
| B | Circle | Trigger Derecho | R2 |
| X | Square | Grip Izquierdo | L1 |
| Y | Triangle | Grip Derecho | R1 |
| Menu | Start | Clic Stick Izquierdo | L3 |
| Stick Izquierdo | D-Pad / Analógico | Clic Stick Derecho | R3 |
| Stick Derecho | Rotación Yaw VR | Ambos Clics de Stick | Ciclar Modo Nav |

### Limitaciones conocidas

 - Los elementos HUD 2D (barras de vida, textos) no son visibles durante el gameplay 3D.
 - Los modos de semi-transparencia del PS1 (aditivo, sustractivo) no están completamente implementados.
 - La iluminación de escena vía overlays transparentes sin textura está ausente.
 - Los fondos pre-renderizados (Resident Evil, Final Fantasy) permanecen en 2D.
 - Solo probado en Quest 1 vía Quest Link; otros headsets pueden necesitar ajustes.

Consulta los [issues abiertos](https://github.com/xaskasdf/duckstation/issues) para la lista completa de mejoras planeadas.

## Requisitos del Sistema
 - Un CPU rápido. Pero necesita ser x86_64, AArch32/armv7 o AArch64/ARMv8, de lo contrario la recompilación será lenta.
 - Para los renderizadores de hardware, se necesita una GPU compatible con OpenGL 3.1/OpenGL ES 3.1/Direct3D 11 Feature Level 10.0 (o Vulkan 1.0) o superior. Básicamente, cualquier computadora fabricada en los últimos 10 años debería funcionar.
 - Control de juego compatible con SDL, XInput o DInput (por ejemplo, XB360/XBOne/XBSeries). Usuarios de DualShock 3 en Windows necesitarán instalar los drivers oficiales de DualShock 3 incluidos como parte de PlayStation Now.
 - **Para VR:** Un headset compatible con OpenXR (Meta Quest, Valve Index, etc.), una GPU compatible con Vulkan y un runtime OpenXR activo (Oculus o SteamVR) en Windows 10/11 x64.

## Descarga y Ejecución
Ejecutables de DuckStation para Windows x64/ARM64, Linux x86_64 (en formato AppImage) y para macOS están disponibles vía GitHub en la pestaña Releases y son compilados automáticamente con cada commit/push.

### Windows

DuckStation **requiere** Windows 10/11, específicamente la versión 1809 o más reciente. Si todavía estás usando Windows 7/8/8.1, DuckStation **no funcionará** en tu sistema operativo. Usar estos sistemas operativos hoy en día debería considerarse un riesgo de seguridad, recomendaría actualizar a algo que reciba soporte del fabricante.
Si necesitas usar un sistema operativo más antiguo, [v0.1-5624](https://github.com/stenzek/duckstation/releases/tag/v0.1-5624) es la última versión que funcionará. Pero no esperes recibir asistencia, esas compilaciones ya no tienen soporte.

Para descargar:
 - Accede a https://github.com/stenzek/duckstation/releases/tag/latest y descarga la compilación de Windows x64. Este es un archivo ZIP que contiene el ejecutable precompilado.
 - Alternativamente, link de descarga directa: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-release.zip
 - Extrae el archivo ZIP **en una carpeta**. El archivo ZIP no tiene un subdirectorio raíz, así que si no extraes en un subdirectorio, va a dejar varios archivos en tu directorio de descargas.

Una vez descargado y extraído, puedes iniciar el emulador con `duckstation-qt-x64-ReleaseLTCG.exe`. Sigue el Asistente de Configuración para comenzar.

**Si recibes un error sobre la falta de `vcruntime140_1.dll`, necesitarás actualizar tu runtime de Visual C++.** Puedes hacerlo desde esta página: https://support.microsoft.com/en-au/help/2977003/the-latest-supported-visual-c-downloads. Específicamente, necesitas el runtime x64, que se puede descargar en https://aka.ms/vs/17/release/vc_redist.x64.exe.

### Linux

Las únicas versiones soportadas de DuckStation para Linux son el AppImage en la página de lanzamientos. Si instalaste DuckStation de otra fuente o distribución (por ejemplo, EmuDeck), debes contactar al responsable para soporte, nosotros no tenemos control sobre eso.

#### AppImage

Los AppImages requieren una distribución equivalente a Ubuntu 22.04 o más reciente para ejecutarse.

 - Accede a https://github.com/stenzek/duckstation/releases/tag/latest y descarga `duckstation-x64.AppImage`.
 - Ejecuta `chmod a+x` en el AppImage descargado -- después de este paso, el AppImage puede ejecutarse como un ejecutable típico.

### macOS

Se proporcionan compilaciones universales de macOS para x86_64 (Intel) y ARM64 (Apple Silicon).

macOS Ventura (13.3) es necesario, ya que también es el requisito mínimo para Qt.

Para descargar:
 - Accede a https://github.com/stenzek/duckstation/releases/tag/latest y descarga `duckstation-mac-release.zip`.
 - Extrae el archivo ZIP haciendo doble clic en él.
 - Abre `DuckStation.app`, opcionalmente moviéndolo a la ubicación deseada primero.
 - Dependiendo de la configuración de GateKeeper, puede que necesites hacer clic derecho -> Abrir la primera vez que lo ejecutes, ya que los certificados de firma de código están fuera de discusión para un proyecto que no genera ingresos.

### Android

Necesitarás un dispositivo con armv7 (32 bits ARM), AArch64 (64 bits ARM) o x86_64 (64 bits x86). 64 bits es preferible, los requisitos son más altos para 32 bits, probablemente querrás al menos un CPU de 1.5 GHz.

La distribución por Google Play es el mecanismo de distribución recomendado y resultará en tamaños de descarga menores: https://play.google.com/store/apps/details?id=com.github.stenzek.duckstation

**No se proporciona soporte para la aplicación Android**, es gratuita y tus expectativas deberían estar alineadas con eso. Por favor, **no** me envíes correos sobre problemas relacionados, serán ignorados.

Si necesitas usar un APK, los links de descarga están listados en https://www.duckstation.org/android/

Para usar:
1. Instala y ejecuta la aplicación por primera vez.
2. Agrega directorios de juegos tocando el botón de agregar y seleccionando un directorio. Puedes agregar directorios adicionales después seleccionando "Editar Directorios de Juegos" en el menú.
3. Toca un juego para comenzar. Cuando inicies un juego por primera vez, te pedirá importar una imagen de BIOS.

Si tienes un control externo, necesitarás mapear los botones y analógicos en la configuración.

### Protección LibCrypt y archivos SBI

Algunos juegos de la región PAL usan la protección LibCrypt, que requiere información adicional de subcanal de CD para funcionar correctamente. El mal funcionamiento de libcrypt generalmente se manifiesta como cuelgues, pero a veces puede afectar la jugabilidad, dependiendo de cómo el juego lo implementó.

Para estos juegos, asegúrate de que la imagen del CD y su archivo SBI (.sbi) correspondiente tengan el mismo nombre y estén en la misma carpeta. DuckStation cargará automáticamente el archivo SBI cuando lo encuentre junto a la imagen del CD.

Por ejemplo, si tu imagen de disco se llamara `Spyro3.cue`, colocarías el archivo SBI en la misma carpeta y lo nombrarías `Spyro3.sbi`.

## Compilación

### Compilando VR

El fork VR actualmente es solo para Windows (OpenXR + Vulkan). El soporte VR para Linux/macOS aún no está disponible.

Requisitos:
 - Visual Studio 2022 o más reciente con la carga de trabajo "Desktop development with C++" instalada.

1. Clona este fork: `git clone https://github.com/xaskasdf/duckstation.git && cd duckstation && git checkout feature/vr`.
2. Descarga el paquete de dependencias de https://github.com/stenzek/duckstation-ext-qt-minimal/releases/download/latest/deps-x64.7z y extráelo en `dep\msvc`.
3. Abre `duckstation.sln` en Visual Studio.
4. Compila el proyecto `duckstation-qt` en la configuración Release x64.
5. El binario se encuentra en `bin/x64/duckstation-qt-x64-Release-MSVC.exe`.
6. Conecta tu headset VR, asegúrate de que el runtime OpenXR esté activo y habilita VR en **Settings > Advanced Settings > Tweaks**.

Los headers de OpenXR y el loader dinámico están incluidos en `dep/openxr/`. No se necesita ninguna instalación adicional de SDK para compilar.

### Windows
Requisitos:
 - Visual Studio 2022

1. Clona el repositorio: `git clone https://github.com/stenzek/duckstation.git`.
2. Descarga el paquete de dependencias de https://github.com/stenzek/duckstation-ext-qt-minimal/releases/download/latest/deps-x64.7z y extráelo en `dep\msvc`.
3. Abre la solución de Visual Studio `duckstation.sln` en la raíz o "Open Folder" para la compilación con CMake.
4. Compila la solución.
5. Los binarios se encuentran en `bin/x64`.
6. Ejecuta `duckstation-qt-x64-Release.exe` o la configuración que hayas usado.

### Linux
Requisitos (nombres de paquetes Debian/Ubuntu):
 - CMake (`cmake`)
 - SDL2 (al menos versión 2.28.2) (`libsdl2-dev` `libxrandr-dev`)
 - pkgconfig (`pkg-config`)
 - Qt 6 (al menos versión 6.5.1) (`qt6-base-dev` `qt6-base-private-dev` `qt6-base-dev-tools` `qt6-tools-dev` `libqt6svg6`)
 - git (`git`) (Nota: necesario para clonar el repositorio y durante la compilación)
 - Cuando Wayland esté habilitado (por defecto): (`libwayland-dev` `libwayland-egl-backend-dev` `extra-cmake-modules` `qt6-wayland`)
 - libcurl (`libcurl4-openssl-dev`)
 - Opcional para compilación más rápida: Ninja (`ninja-build`)

1. Clona el repositorio: `git clone https://github.com/stenzek/duckstation.git -b dev`.
2. Crea un directorio de compilación, ya sea dentro o fuera del directorio fuente.
3. Ejecuta CMake para configurar el sistema de compilación. Suponiendo que el directorio de compilación sea `build-release`, ejecuta `cmake -Bbuild-release -DCMAKE_BUILD_TYPE=Release`. Si tienes Ninja instalado, agrega `-GNinja` al final de la línea de comandos de CMake para compilaciones más rápidas.
4. Compila el código fuente. Para el ejemplo anterior, ejecuta `cmake --build build-release --parallel`.
5. Ejecuta el binario, que se encuentra en el directorio de compilación en `bin/duckstation-qt`.

### macOS

Requisitos:
 - CMake
 - SDL2 (al menos versión 2.28.2)
 - Qt 6 (al menos versión 6.5.1)

Opcional (recomendado para compilaciones más rápidas):
 - Ninja

1. Clona el repositorio: `git clone https://github.com/stenzek/duckstation.git`.
2. Ejecuta CMake para configurar el sistema de compilación: `cmake -Bbuild-release -DCMAKE_BUILD_TYPE=Release`. Puede que necesites especificar `-DQt6_DIR` dependiendo de tu sistema. Si tienes Ninja instalado, agrega `-GNinja` al final de la línea de comandos de CMake para compilaciones más rápidas.
4. Compila el código fuente: `cmake --build build-release --parallel`.
5. Ejecuta el binario, que se encuentra en el directorio de compilación en `bin/DuckStation.app`.

## Directorios de Usuario
El "Directorio de Usuario" es donde debes colocar tus imágenes de BIOS, donde se guardan las configuraciones, y donde las tarjetas de memoria y estados guardados se guardan por defecto. Un [archivo opcional de base de datos de controles SDL](#base-de-datos-de-controles-sdl) también puede colocarse aquí.

Se encuentra en los siguientes lugares, dependiendo de la plataforma que estés usando:

- Windows: Mis Documentos\DuckStation
- Linux: `$XDG_DATA_HOME/duckstation`, o `~/.local/share/duckstation`.
- macOS: `~/Library/Application Support/DuckStation`.

Así que si estás usando Linux, sugiero colocar tus imágenes de BIOS en `~/.local/share/duckstation/bios`. Este directorio se creará la primera vez que ejecutes DuckStation.

Si deseas usar una compilación "portátil", donde el directorio de usuario es el mismo donde se encuentra el ejecutable, crea un archivo vacío llamado `portable.txt` en el mismo directorio donde está el ejecutable de DuckStation.

## Asignaciones para la interfaz Qt
Tu teclado o control de juego pueden usarse para simular una variedad de controles de PlayStation. La entrada del control es soportada a través de los backends DInput, XInput y SDL y puede cambiarse en `Configuración -> Configuración de Controles`.

Para asignar tu dispositivo de entrada, ve a `Configuración -> Configuración de Controles`. Cada uno de los botones/ejes del control emulado se listará, junto con la tecla/botón correspondiente de tu dispositivo actualmente en uso. Para reasignar, haz clic en la caja al lado del nombre del botón/eje y presiona la tecla o botón de tu dispositivo de entrada que desees asignar. Al asignar la vibración, simplemente presiona cualquier botón en el control al que quieras configurarla.

## Base de Datos de Controles SDL
Los lanzamientos de DuckStation incluyen una base de datos de mapeos de controles de juego para el backend SDL, cortesía de https://github.com/gabomdq/SDL_GameControllerDB. El archivo `gamecontrollerdb.txt` incluido puede encontrarse en el subdirectorio `database` del directorio del programa DuckStation.

Si estás teniendo problemas para asignar tu control con el backend SDL, puede que necesites agregar un mapeo personalizado al archivo de base de datos. Haz una copia de `gamecontrollerdb.txt` y colócala en tu [directorio de usuario](#directorios-de-usuario) (o directamente en el directorio del programa, si estás ejecutando en modo portátil) y sigue las instrucciones en el [repositorio SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB) para crear un nuevo mapeo. Agrega este mapeo a la nueva copia de `gamecontrollerdb.txt` y tu control debería ser reconocido correctamente.

## Asignaciones por defecto
Control 1:
 - **D-Pad:** W/A/S/D
 - **Triángulo/Cuadrado/Círculo/Cruz:** Numpad8/Numpad4/Numpad6/Numpad2
 - **L1/R1:** Q/E
 - **L2/R2:** 1/3
 - **Start:** Enter
 - **Select:** Backspace

Atajos:
 - **Esc:** Abrir Menú de Pausa
 - **F11:** Alternar Pantalla Completa
 - **Tab:** Deshabilitar Temporalmente el Limitador de Velocidad
 - **Espacio:** Pausar/Reanudar Emulación

## Avisos Legales

Ícono por icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

"PlayStation" y "PSX" son marcas registradas de Sony Interactive Entertainment Europe Limited. Este proyecto no está afiliado de ninguna manera con Sony Interactive Entertainment.
