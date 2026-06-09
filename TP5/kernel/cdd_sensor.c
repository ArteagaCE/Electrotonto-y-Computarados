// SPDX-License-Identifier: GPL-2.0
//
// cdd_sensor.c — Character Device Driver: dual external GPIO sampler @ 1 Hz
//
// Señal 1: entrada digital GPIO configurable (default BCM17)
// Señal 2: entrada digital GPIO configurable (default BCM27)
//
// Interfaz de usuario:
//   read()  — bloquea hasta la próxima muestra, devuelve "<valor>\n"
//   write() — escribe "1" o "2" para seleccionar la señal activa
//   ioctl() — SENSOR_IOC_SET / SENSOR_IOC_GET / SENSOR_IOC_INFO
//
// Si simulate=1, el módulo no reserva GPIOs y genera datos sintéticos. Ese modo
// sirve para probar la interfaz sin cableado, pero el modo normal del TP sensa
// las dos señales externas conectadas a la Raspberry Pi.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/kfifo.h>
#include <linux/gpio.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RdC TP5");
MODULE_DESCRIPTION("CDD dual GPIO 1 Hz sampler");
MODULE_VERSION("1.1");

/* ──────────────────────────────────────────────
 * IOCTL interface (shared with user-space)
 * ────────────────────────────────────────────── */

struct sensor_info {
	int  signal_num;    /* 1 o 2              */
	int  scale;         /* divisor: raw/scale */
	char name[32];      /* "GPIO 17"          */
	char unit[16];      /* "logic"            */
};

#define SENSOR_IOC_MAGIC  'S'
#define SENSOR_IOC_SET    _IOW(SENSOR_IOC_MAGIC, 0, int)
#define SENSOR_IOC_GET    _IOR(SENSOR_IOC_MAGIC, 1, int)
#define SENSOR_IOC_INFO   _IOR(SENSOR_IOC_MAGIC, 2, struct sensor_info)

#define DRIVER_NAME  "cdd_sensor"
#define DEVICE_NAME  "sensor_cdd"
#define FIFO_SIZE    64u   /* debe ser potencia de 2 */

/* ──────────────────────────────────────────────
 * Parámetros del módulo
 * ────────────────────────────────────────────── */
static int gpio1 = 17;
module_param(gpio1, int, 0444);
MODULE_PARM_DESC(gpio1, "GPIO BCM usado como señal 1 (default: 17)");

static int gpio2 = 27;
module_param(gpio2, int, 0444);
MODULE_PARM_DESC(gpio2, "GPIO BCM usado como señal 2 (default: 27)");

static bool active_low;
module_param(active_low, bool, 0644);
MODULE_PARM_DESC(active_low, "Invierte la lectura lógica de las entradas");

static bool simulate;
module_param(simulate, bool, 0444);
MODULE_PARM_DESC(simulate, "Usa señales sintéticas en lugar de GPIO externos");

/* ──────────────────────────────────────────────
 * Generación / sensado de señales
 * ────────────────────────────────────────────── */
static int32_t sim_s1_raw = 2500; /* 25.00 °C, sólo para simulate=1 */
static int     sim_s2_step;

static int32_t sample_simulated_signal1(void)
{
	u8 r;
	int32_t delta;

	get_random_bytes(&r, sizeof(r));
	delta = ((int32_t)r - 128) / 10;
	sim_s1_raw = clamp(sim_s1_raw + delta, (int32_t)1500, (int32_t)3500);
	return sim_s1_raw;
}

static int32_t sample_simulated_signal2(void)
{
	sim_s2_step = (sim_s2_step + 1) % 100;
	return (sim_s2_step < 50) ? sim_s2_step * 66 : (100 - sim_s2_step) * 66;
}

static int32_t normalize_gpio_value(int value)
{
	value = !!value;
	return active_low ? !value : value;
}

static int32_t sample_signal(int signal)
{
	if (simulate)
		return (signal == 1) ? sample_simulated_signal1() : sample_simulated_signal2();

	return normalize_gpio_value(gpio_get_value(signal == 1 ? gpio1 : gpio2));
}

/* ──────────────────────────────────────────────
 * KFIFO + sincronización
 * ────────────────────────────────────────────── */
DEFINE_KFIFO(sensor_fifo, int32_t, FIFO_SIZE);
static DEFINE_SPINLOCK(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(read_wq);

/* señal activa (1 ó 2) */
static atomic_t current_signal = ATOMIC_INIT(1);

static void set_current_signal(int signal)
{
	atomic_set(&current_signal, signal);

	/* Vaciar el fifo para que el lector no reciba datos de la señal anterior. */
	spin_lock(&fifo_lock);
	kfifo_reset(&sensor_fifo);
	spin_unlock(&fifo_lock);
}

/* ──────────────────────────────────────────────
 * Timer kernel: disparo cada HZ jiffies (1 s)
 * ────────────────────────────────────────────── */
static struct timer_list sample_timer;

static void timer_cb(struct timer_list *t)
{
	int32_t val = sample_signal(atomic_read(&current_signal));

	spin_lock(&fifo_lock);
	/* Si el fifo está lleno descartamos el dato más viejo. */
	if (kfifo_is_full(&sensor_fifo)) {
		int32_t dummy;

		kfifo_get(&sensor_fifo, &dummy);
	}
	kfifo_put(&sensor_fifo, val);
	spin_unlock(&fifo_lock);

	wake_up_interruptible(&read_wq);
	mod_timer(t, jiffies + HZ);
}

/* ──────────────────────────────────────────────
 * Character device: fops
 * ────────────────────────────────────────────── */
static int cdd_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int cdd_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/* read() bloquea hasta que haya un dato nuevo en el fifo */
static ssize_t cdd_read(struct file *filp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	char    kbuf[32];
	int     len;
	int32_t val;

retry:
	if (wait_event_interruptible(read_wq, !kfifo_is_empty(&sensor_fifo)))
		return -ERESTARTSYS;

	spin_lock(&fifo_lock);
	if (!kfifo_get(&sensor_fifo, &val)) {
		/* wake-up espurio (puede ocurrir tras reset del fifo) */
		spin_unlock(&fifo_lock);
		goto retry;
	}
	spin_unlock(&fifo_lock);

	len = snprintf(kbuf, sizeof(kbuf), "%d\n", (int)val);
	if ((size_t)len > count)
		return -EINVAL;
	if (copy_to_user(buf, kbuf, len))
		return -EFAULT;

	return len;
}

/* write() recibe "1" o "2" para cambiar la señal activa */
static ssize_t cdd_write(struct file *filp, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char kbuf[4];
	int  sig;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (kbuf[0] == '1')
		sig = 1;
	else if (kbuf[0] == '2')
		sig = 2;
	else
		return -EINVAL;

	set_current_signal(sig);
	pr_info("%s: señal activa -> %d\n", DRIVER_NAME, sig);
	return count;
}

static long cdd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct sensor_info info;
	int sig;

	switch (cmd) {
	case SENSOR_IOC_SET:
		if (copy_from_user(&sig, (int __user *)arg, sizeof(sig)))
			return -EFAULT;
		if (sig != 1 && sig != 2)
			return -EINVAL;
		set_current_signal(sig);
		pr_info("%s: IOCTL señal -> %d\n", DRIVER_NAME, sig);
		break;

	case SENSOR_IOC_GET:
		sig = atomic_read(&current_signal);
		if (copy_to_user((int __user *)arg, &sig, sizeof(sig)))
			return -EFAULT;
		break;

	case SENSOR_IOC_INFO:
		sig = atomic_read(&current_signal);
		memset(&info, 0, sizeof(info));
		info.signal_num = sig;
		info.scale = simulate ? (sig == 1 ? 100 : 1) : 1;
		if (simulate && sig == 1) {
			strscpy(info.name, "Temperatura simulada", sizeof(info.name));
			strscpy(info.unit, "centidegC", sizeof(info.unit));
		} else if (simulate) {
			strscpy(info.name, "Voltaje simulado", sizeof(info.name));
			strscpy(info.unit, "mV", sizeof(info.unit));
		} else {
			snprintf(info.name, sizeof(info.name), "GPIO %d", sig == 1 ? gpio1 : gpio2);
			strscpy(info.unit, "logic", sizeof(info.unit));
		}
		if (copy_to_user((struct sensor_info __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		break;

	default:
		return -ENOTTY;
	}
	return 0;
}

static const struct file_operations cdd_fops = {
	.owner          = THIS_MODULE,
	.open           = cdd_open,
	.release        = cdd_release,
	.read           = cdd_read,
	.write          = cdd_write,
	.unlocked_ioctl = cdd_ioctl,
};

/* ──────────────────────────────────────────────
 * GPIO setup
 * ────────────────────────────────────────────── */
static int request_input_gpio(int gpio, const char *label)
{
	int ret;

	if (!gpio_is_valid(gpio)) {
		pr_err("%s: GPIO inválido para %s: %d\n", DRIVER_NAME, label, gpio);
		return -EINVAL;
	}

	ret = gpio_request(gpio, label);
	if (ret) {
		pr_err("%s: no se pudo reservar %s (GPIO %d): %d\n",
		       DRIVER_NAME, label, gpio, ret);
		return ret;
	}

	ret = gpio_direction_input(gpio);
	if (ret) {
		pr_err("%s: no se pudo configurar %s como entrada: %d\n",
		       DRIVER_NAME, label, ret);
		gpio_free(gpio);
		return ret;
	}

	return 0;
}

static int setup_gpios(void)
{
	int ret;

	if (simulate)
		return 0;

	ret = request_input_gpio(gpio1, "cdd_sensor_sig1");
	if (ret)
		return ret;

	ret = request_input_gpio(gpio2, "cdd_sensor_sig2");
	if (ret) {
		gpio_free(gpio1);
		return ret;
	}

	return 0;
}

static void release_gpios(void)
{
	if (simulate)
		return;

	gpio_free(gpio2);
	gpio_free(gpio1);
}

/* ──────────────────────────────────────────────
 * Init / Exit
 * ────────────────────────────────────────────── */
static dev_t       dev_num;
static struct cdev cdd_cdev;
static struct class  *cdd_class;
static struct device *cdd_device;

static int __init cdd_init(void)
{
	int ret;

	ret = setup_gpios();
	if (ret)
		return ret;

	ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
	if (ret < 0) {
		pr_err("%s: alloc_chrdev_region: %d\n", DRIVER_NAME, ret);
		goto err_gpio;
	}

	cdev_init(&cdd_cdev, &cdd_fops);
	cdd_cdev.owner = THIS_MODULE;
	ret = cdev_add(&cdd_cdev, dev_num, 1);
	if (ret < 0) {
		pr_err("%s: cdev_add: %d\n", DRIVER_NAME, ret);
		goto err_unreg;
	}

/* class_create() cambió de firma en kernel 6.4 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	cdd_class = class_create(DRIVER_NAME);
#else
	cdd_class = class_create(THIS_MODULE, DRIVER_NAME);
#endif
	if (IS_ERR(cdd_class)) {
		ret = PTR_ERR(cdd_class);
		pr_err("%s: class_create: %d\n", DRIVER_NAME, ret);
		goto err_cdev;
	}

	cdd_device = device_create(cdd_class, NULL, dev_num, NULL, DEVICE_NAME);
	if (IS_ERR(cdd_device)) {
		ret = PTR_ERR(cdd_device);
		pr_err("%s: device_create: %d\n", DRIVER_NAME, ret);
		goto err_class;
	}

	timer_setup(&sample_timer, timer_cb, 0);
	mod_timer(&sample_timer, jiffies + HZ);

	if (simulate)
		pr_info("%s: cargado en modo simulación. /dev/%s (major %d)\n",
			DRIVER_NAME, DEVICE_NAME, MAJOR(dev_num));
	else
		pr_info("%s: cargado. /dev/%s (major %d). GPIOs: señal1=%d, señal2=%d\n",
			DRIVER_NAME, DEVICE_NAME, MAJOR(dev_num), gpio1, gpio2);
	return 0;

err_class:
	class_destroy(cdd_class);
err_cdev:
	cdev_del(&cdd_cdev);
err_unreg:
	unregister_chrdev_region(dev_num, 1);
err_gpio:
	release_gpios();
	return ret;
}

static void __exit cdd_exit(void)
{
	del_timer_sync(&sample_timer);
	device_destroy(cdd_class, dev_num);
	class_destroy(cdd_class);
	cdev_del(&cdd_cdev);
	unregister_chrdev_region(dev_num, 1);
	release_gpios();
	pr_info("%s: descargado\n", DRIVER_NAME);
}

module_init(cdd_init);
module_exit(cdd_exit);