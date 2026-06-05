// SPDX-License-Identifier: GPL-2.0
//
// cdd_sensor.c — Character Device Driver: dual-sensor sampler @ 1 Hz
//
// Señal 1: temperatura simulada (random walk, rango 15-35 °C, raw = centidegrees)
// Señal 2: voltaje simulado (onda triangular 0-3300 mV)
//
// Interfaz de usuario:
//   read()  — bloquea hasta la próxima muestra, devuelve "<valor>\n"
//   write() — escribe "1" o "2" para seleccionar la señal activa
//   ioctl() — SENSOR_IOC_SET / SENSOR_IOC_GET / SENSOR_IOC_INFO
//
// Cross-compilación apuntada a ARM (Raspberry Pi).

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
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/kfifo.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RdC TP5");
MODULE_DESCRIPTION("CDD dual-sensor 1 Hz sampler");
MODULE_VERSION("1.0");

/* ──────────────────────────────────────────────
 * IOCTL interface (shared with user-space)
 * ────────────────────────────────────────────── */

struct sensor_info {
	int  signal_num;	/* 1 o 2               */
	int  scale;		/* divisor: raw/scale  */
	char name[32];		/* "Temperatura"       */
	char unit[16];		/* "centidegC" / "mV"  */
};

#define SENSOR_IOC_MAGIC  'S'
#define SENSOR_IOC_SET    _IOW(SENSOR_IOC_MAGIC, 0, int)
#define SENSOR_IOC_GET    _IOR(SENSOR_IOC_MAGIC, 1, int)
#define SENSOR_IOC_INFO   _IOR(SENSOR_IOC_MAGIC, 2, struct sensor_info)

/* ──────────────────────────────────────────────
 * Module parameters
 * ────────────────────────────────────────────── */
#define DRIVER_NAME  "cdd_sensor"
#define DEVICE_NAME  "sensor_cdd"
#define FIFO_SIZE    64u   /* debe ser potencia de 2 */

/* ──────────────────────────────────────────────
 * Generación de señales
 * ────────────────────────────────────────────── */
static int32_t s1_raw = 2500;	/* temperatura inicial: 25.00 °C */
static int     s2_step;		/* paso de la onda triangular    */

static int32_t sample_signal1(void)
{
	uint8_t r;
	int32_t delta;

	get_random_bytes(&r, sizeof(r));
	delta = (int32_t)r - 128;	/* -128 .. +127 */
	delta /= 10;			/* ≈ ±12 centidegrees por paso */
	s1_raw = clamp(s1_raw + delta, (int32_t)1500, (int32_t)3500);
	return s1_raw;
}

static int32_t sample_signal2(void)
{
	int32_t v;

	s2_step = (s2_step + 1) % 100;
	v = (s2_step < 50) ? s2_step * 66 : (100 - s2_step) * 66;
	return v;	/* 0 .. 3300 mV */
}

/* ──────────────────────────────────────────────
 * KFIFO + sincronización
 * ────────────────────────────────────────────── */
DEFINE_KFIFO(sensor_fifo, int32_t, FIFO_SIZE);
static DEFINE_SPINLOCK(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(read_wq);

/* señal activa (1 ó 2) */
static int current_signal = 1;

/* ──────────────────────────────────────────────
 * Timer kernel: disparo cada HZ jiffies (1 s)
 * ────────────────────────────────────────────── */
static struct timer_list sample_timer;

static void timer_cb(struct timer_list *t)
{
	int32_t val = (current_signal == 1) ? sample_signal1() : sample_signal2();

	spin_lock(&fifo_lock);
	/* Si el fifo está lleno descartamos el dato más viejo */
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
 * Carácter device: fops
 * ────────────────────────────────────────────── */
static atomic_t open_count = ATOMIC_INIT(0);

static int cdd_open(struct inode *inode, struct file *filp)
{
	/* Acceso exclusivo: sólo un lector a la vez */
	if (atomic_inc_return(&open_count) > 1) {
		atomic_dec(&open_count);
		return -EBUSY;
	}
	return 0;
}

static int cdd_release(struct inode *inode, struct file *filp)
{
	atomic_dec(&open_count);
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

	current_signal = sig;

	/* Vaciar el fifo para que el lector no reciba datos de la señal anterior */
	spin_lock(&fifo_lock);
	kfifo_reset(&sensor_fifo);
	spin_unlock(&fifo_lock);

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
		current_signal = sig;
		spin_lock(&fifo_lock);
		kfifo_reset(&sensor_fifo);
		spin_unlock(&fifo_lock);
		pr_info("%s: IOCTL señal -> %d\n", DRIVER_NAME, sig);
		break;

	case SENSOR_IOC_GET:
		sig = current_signal;
		if (copy_to_user((int __user *)arg, &sig, sizeof(sig)))
			return -EFAULT;
		break;

	case SENSOR_IOC_INFO:
		memset(&info, 0, sizeof(info));
		info.signal_num = current_signal;
		if (current_signal == 1) {
			info.scale = 100;
			strncpy(info.name, "Temperatura", sizeof(info.name) - 1);
			strncpy(info.unit, "centidegC",   sizeof(info.unit) - 1);
		} else {
			info.scale = 1;
			strncpy(info.name, "Voltaje",  sizeof(info.name) - 1);
			strncpy(info.unit, "mV",       sizeof(info.unit) - 1);
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
 * Init / Exit
 * ────────────────────────────────────────────── */
static dev_t       dev_num;
static struct cdev cdd_cdev;
static struct class  *cdd_class;
static struct device *cdd_device;

static int __init cdd_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
	if (ret < 0) {
		pr_err("%s: alloc_chrdev_region: %d\n", DRIVER_NAME, ret);
		return ret;
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

	pr_info("%s: cargado. /dev/%s (major %d). Señal inicial: 1\n",
		DRIVER_NAME, DEVICE_NAME, MAJOR(dev_num));
	return 0;

err_class:
	class_destroy(cdd_class);
err_cdev:
	cdev_del(&cdd_cdev);
err_unreg:
	unregister_chrdev_region(dev_num, 1);
	return ret;
}

static void __exit cdd_exit(void)
{
	del_timer_sync(&sample_timer);
	device_destroy(cdd_class, dev_num);
	class_destroy(cdd_class);
	cdev_del(&cdd_cdev);
	unregister_chrdev_region(dev_num, 1);
	pr_info("%s: descargado\n", DRIVER_NAME);
}

module_init(cdd_init);
module_exit(cdd_exit);
