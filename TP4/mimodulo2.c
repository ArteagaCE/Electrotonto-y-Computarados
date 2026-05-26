#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/utsname.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ariel");
MODULE_DESCRIPTION("Modulo de kernel firmado para TP");
MODULE_VERSION("1.0");

static int __init mimodulo_init(void)
{
    pr_info("mimodulo: modulo cargado correctamente en el equipo %s\n",
            utsname()->nodename);
    return 0;
}

static void __exit mimodulo_exit(void)
{
    pr_info("mimodulo: modulo descargado correctamente del equipo %s\n",
            utsname()->nodename);
}

module_init(mimodulo_init);
module_exit(mimodulo_exit);