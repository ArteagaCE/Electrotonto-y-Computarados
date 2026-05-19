# TP4: Módulos de Kernel y Llamadas al Sistema

**Materia:** Sistemas de Computación
**Grupo:** Electrotonto y Computarados
**Integrantes:**

* Martina Juri
* Marcos Morán
* Francisco Gomez Neimann
* Cristian Arteaga

**Docentes:**

* Miguel Ángel Solinas
* Javier Alejandro Jorge

---

# Introducción

En este trabajo práctico se investigó el funcionamiento de los módulos del kernel en Linux, su carga y descarga dinámica, la diferencia entre espacio de usuario y espacio del kernel, y distintas herramientas relacionadas con el análisis del sistema operativo.

Un módulo del kernel es una porción de código que puede cargarse y descargarse dinámicamente en el núcleo del sistema operativo sin necesidad de reiniciar el sistema. Esto permite extender funcionalidades del kernel de manera flexible, por ejemplo agregando drivers, soporte para hardware o sistemas de archivos.

Durante el desarrollo del trabajo se compiló y cargó un módulo propio, se analizaron los mensajes del kernel utilizando `dmesg`, se inspeccionaron módulos cargados con `lsmod`, se investigó el contenido de `/dev` y se comparó un módulo desarrollado por nosotros con módulos reales del sistema Linux.

Además, se exploraron conceptos relacionados con Secure Boot y la firma de módulos del kernel, aunque la máquina virtual utilizada para esta primera parte del trabajo no poseía soporte EFI habilitado.

---

# Preparación del entorno

Para realizar el trabajo se utilizó una máquina virtual con Ubuntu Linux. Se instalaron las herramientas necesarias para compilar módulos del kernel y trabajar con paquetes.

```bash
sudo apt update
sudo apt install build-essential checkinstall linux-headers-$(uname -r)
```

También se verificó la versión del kernel utilizada:

```bash
uname -r
```

Resultado:

```text
6.8.0-48-generic
```

---

# Compilación del módulo

Se utilizó el módulo de ejemplo provisto por la cátedra dentro del directorio `part1/module`.

El módulo fue compilado utilizando `make`:

```bash
make
```

Esto generó el archivo:

```text
mimodulo.ko
```

que corresponde al módulo compilado listo para cargarse en el kernel.

---

# Carga del módulo en el kernel

Para cargar el módulo se utilizó:

```bash
sudo insmod mimodulo.ko
```

Luego se verificaron los mensajes del kernel mediante:

```bash
sudo dmesg | tail
```

Obteniendo:

!["Carga del módulo mimodulo.ko exitosa."](images/carga-modulo.png)

Esto indica que el módulo fue cargado correctamente y que la función de inicialización del módulo se ejecutó exitosamente.

---

# Verificación de módulos cargados

Una vez cargado el módulo, se verificó su presencia mediante `lsmod`:

```bash
lsmod | grep mimodulo
```

Resultado:

```text
mimodulo 12288 0
```

Esto indica:

* nombre del módulo
* tamaño ocupado en memoria
* cantidad de referencias activas

También se inspeccionó `/proc/modules`:

```bash
cat /proc/modules | grep mimodulo
```

Resultado:

```text
mimodulo 12280 0 - Live 0x0000000000000000 (OE)
```

El sufijo `(OE)` indica que el módulo es:

* **O:** Out-of-tree → módulo externo al árbol oficial del kernel.
* **E:** unsigned/external → módulo externo no firmado.

---

# Información del módulo utilizando modinfo

Se utilizó:

```bash
modinfo mimodulo.ko
```

Obteniendo:

![Información del módulo mimodulo.ko obtenida con modinfo.](images/modinfo.png)

El campo `vermagic` resulta especialmente importante, ya que indica la versión del kernel con la cual el módulo fue compilado. Si el módulo se compila utilizando headers incompatibles, el kernel puede rechazar su carga.

---

# Comparación con un módulo real del sistema

Se buscaron módulos disponibles dentro del sistema mediante:

```bash
find /lib/modules/$(uname -r) | grep '\.ko$' | head -20
```

Entre ellos se seleccionó el módulo:

```text
/lib/modules/6.8.0-48-generic/misc/vboxvideo.ko
```

Luego se ejecutó:

```bash
modinfo /lib/modules/6.8.0-48-generic/misc/vboxvideo.ko
```

Resultado parcial:

![Información del módulo vboxvideo.ko obtenida con modinfo.](images/vboxvideo-modinfo.png)

A diferencia de nuestro módulo simple, este módulo posee:

* dependencias con otros módulos
* parámetros configurables
* mayor cantidad de metadata
* funcionalidad real de driver gráfico para VirtualBox

Esto refleja la diferencia entre un módulo de ejemplo y un driver utilizado realmente por el sistema operativo.

---

# Descarga del módulo

Para descargar el módulo del kernel se utilizó:

```bash
sudo rmmod mimodulo
```

Posteriormente se revisaron nuevamente los logs del kernel:

```bash
sudo dmesg | tail
```

Durante la descarga apareció un mensaje de error del kernel acompañado por registros internos y stack trace.

Finalmente se observó:

```text
Modulo descargado del kernel.
```

Esto permitió observar cómo un error dentro de un módulo afecta directamente al kernel, ya que los módulos se ejecutan en espacio privilegiado y no poseen el aislamiento de memoria que tienen los programas normales de usuario.

![Mensaje de error del kernel al descargar el módulo mimodulo.](images/error-rmmod.png)
---

# Diferencia entre un programa y un módulo

## Programa de usuario

Un programa normal:

* se ejecuta en espacio de usuario
* utiliza bibliotecas como libc
* accede al hardware mediante llamadas al sistema
* posee memoria aislada y protegida

Si ocurre un error grave, normalmente solo se termina el proceso.

---

## Módulo del kernel

Un módulo:

* se ejecuta en espacio del kernel
* posee privilegios totales
* accede directamente al hardware y memoria
* utiliza funciones internas del kernel

Errores dentro de un módulo pueden provocar:

* kernel oops
* kernel panic
* congelamiento del sistema

---

# Espacio de usuario y espacio del kernel

## Espacio de usuario

Es el entorno donde se ejecutan aplicaciones comunes. Los programas no poseen acceso directo al hardware y deben interactuar con el kernel mediante llamadas al sistema.

## Espacio del kernel

Es el entorno donde se ejecuta el núcleo del sistema operativo y los módulos cargados. Posee acceso completo al hardware y a toda la memoria del sistema.

Todos los módulos comparten el mismo espacio de memoria del kernel, por lo que un error puede afectar a todo el sistema operativo.

---

# Drivers y contenido de /dev

Se exploró el contenido del directorio `/dev` mediante:

```bash
ls /dev | head
```

Resultado:

```text
autofs
block
bsg
btrfs-control
bus
cdrom
char
console
core
cpu
```

El directorio `/dev` contiene archivos especiales que representan dispositivos físicos y virtuales del sistema.

En Linux, muchos dispositivos se acceden como si fueran archivos.

Ejemplos:

* `/dev/sda` → disco rígido
* `/dev/cdrom` → lectora
* `/dev/null` → dispositivo virtual
* `/dev/random` → generador aleatorio

Los drivers del kernel son los encargados de gestionar estos dispositivos y exponer interfaces accesibles desde espacio de usuario.

![Contenido del directorio /dev.](images/dev.png)
---

# Información de dispositivos del sistema

También se utilizó:

```bash
lsblk
```

Resultado parcial:

```text
sda      79,4G
├─sda1      1M
├─sda2    513M /boot/efi
└─sda3   78,9G /
```

Esto permitió observar:

* discos detectados
* particiones
* puntos de montaje
* dispositivos virtuales utilizados por VirtualBox

![Información de dispositivos del sistema obtenida con lsblk.](images/lsblk.png)
---

# Secure Boot y EFI en la máquina virtual

Se intentó verificar el estado de Secure Boot mediante:

```bash
mokutil --sb-state
```

Resultado:

```text
EFI variables are not supported on this system
```

Además se verificó:

```bash
ls /sys/firmware/efi
```

Resultado:

```text
No such file or directory
```

Esto indica que la máquina virtual utilizada estaba funcionando en modo BIOS legacy y no utilizando UEFI con soporte EFI real.

Por este motivo:

* no fue posible probar Secure Boot completamente
* no fue posible registrar claves MOK
* el kernel permitió cargar módulos sin firma digital

Aun así, se investigó teóricamente el funcionamiento de Secure Boot y la firma de módulos del kernel.

![Mensaje de error al intentar verificar el estado de Secure Boot.](images/secureboot.png)

---

# ¿Qué es checkinstall?

`checkinstall` es una herramienta que permite crear paquetes instalables (`.deb`) a partir de software compilado manualmente.

En lugar de utilizar:

```bash
sudo make install
```

se puede utilizar:

```bash
sudo checkinstall
```

Esto genera un paquete administrable por el sistema de paquetes de Linux, permitiendo:

* instalación limpia
* desinstalación sencilla
* seguimiento de archivos instalados

---

# ¿Qué ocurre si falta un driver?

Cuando un driver no está disponible:

* el sistema no puede inicializar correctamente el hardware
* el dispositivo puede aparecer como desconocido
* no se crea la entrada correspondiente en `/dev`
* el hardware queda inutilizable

Esto demuestra la importancia de los módulos del kernel y drivers dentro del sistema operativo.

---

# Conclusión

Durante el desarrollo del TP4 se trabajó directamente con módulos del kernel Linux, observando cómo se compilan, cargan y descargan dinámicamente dentro del sistema operativo.

La práctica permitió comprender las diferencias fundamentales entre:

* programas de usuario
* módulos del kernel
* espacio de usuario
* espacio privilegiado del kernel

También se analizaron herramientas importantes del sistema como:

* `lsmod`
* `modinfo`
* `dmesg`
* `/proc/modules`
* `lsblk`

Además, se investigó el rol de Secure Boot y la importancia de las firmas digitales en módulos del kernel para prevenir la carga de código malicioso o rootkits.

Aunque la máquina virtual utilizada no contaba con soporte EFI ni Secure Boot habilitado, la experiencia permitió comprender el funcionamiento general de estos mecanismos de seguridad y su importancia en sistemas modernos.
