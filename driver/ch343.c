// SPDX-License-Identifier: GPL-2.0+
/*
 * USB serial driver for USB to UART(s) chip ch342/ch343/ch344/ch346/ch347/
 * ch339/ch9101/ch9102/ch9103/ch9104/ch9143/ch9111/ch9114, etc.
 *
 * Copyright (C) 2025 Nanjing Qinheng Microelectronics Co., Ltd.
 * Web:		http://wch.cn
 * Author:	WCH <tech@wch.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * System required:
 * Kernel version beyond 3.2.x
 * Update Log:
 * V1.0 - initial version
 * V1.1 - add support of chip ch344, ch9101 and ch9103
 * V1.2 - add gpio support of chip ch344
 * V1.3 - add support of chip ch347
 * V1.4 - add support of chip ch9104
 * V1.5 - add gpio character device
 *      - add support for kernel version beyond 5.14.x
 * V1.6 - add support for non-standard baud rates above 2Mbps of ch347 etc.
 *      - add support for kernel version beyond 6.3.x
 *      - fix bugs when usb device disconnect
 * V1.7 - add support for kernel version 3.3.x~3.4.x
 *      - add support of chip ch9143
 * V1.8 - add support for kernel version beyond 6.5.x
 * V1.9 - add support of chip ch9111, ch9114 and ch346
 * V2.0 - set default termios baudrate to B115200
 *      - fix issue of automatically sending data upon opening tty
 *        for the first time on some systems
 */

#define DEBUG
#define VERBOSE_DEBUG

#undef DEBUG
#undef VERBOSE_DEBUG

#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/serial.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>
#include <linux/version.h>
#include <asm/byteorder.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0))
#include <linux/sched/signal.h>
#endif

#include "ch343.h"

#define DRIVER_AUTHOR "WCH"
#define DRIVER_DESC                                                  \
	"USB serial driver for ch342/ch343/ch344/ch346/ch347/ch339/" \
	"ch9101/ch9102/ch9103/ch9104/ch9143, etc."
#define VERSION_DESC "V2.0 On 2025.04"

#define IOCTL_MAGIC 'W'

#define IOCTL_CMD_GETCHIPTYPE _IOR(IOCTL_MAGIC, 0x84, u16)
#define IOCTL_CMD_GETUARTINDEX _IOR(IOCTL_MAGIC, 0x85, u16)
#define IOCTL_CMD_CTRLIN _IOWR(IOCTL_MAGIC, 0x90, u16)
#define IOCTL_CMD_CTRLOUT _IOW(IOCTL_MAGIC, 0x91, u16)

#ifndef USB_DEVICE_INTERFACE_NUMBER
#define USB_DEVICE_INTERFACE_NUMBER(vend, prod, num)   \
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE |    \
		       USB_DEVICE_ID_MATCH_INT_NUMBER, \
	.idVendor = (vend), .idProduct = (prod),       \
	.bInterfaceNumber = (num)
#endif

static struct usb_driver ch343_driver;
static struct tty_driver *ch343_tty_driver;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
static DEFINE_IDR(ch343_minors);
#else
static struct ch343 *ch343_table[CH343_TTY_MINORS];
#endif

static DEFINE_MUTEX(ch343_minors_lock);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
static void ch343_tty_set_termios(struct tty_struct *tty,
				  const struct ktermios *termios_old);
#else
static void ch343_tty_set_termios(struct tty_struct *tty,
				  struct ktermios *termios_old);
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))

/*
 * Look up an ch343 structure by minor. If found and not disconnected,
 * increment its refcount and return it with its mutex held.
 */
static struct ch343 *ch343_get_by_index(unsigned int minor)
{
	struct ch343 *ch343;

	mutex_lock(&ch343_minors_lock);
	ch343 = idr_find(&ch343_minors, minor);
	if (ch343) {
		mutex_lock(&ch343->mutex);
		if (ch343->disconnected) {
			mutex_unlock(&ch343->mutex);
			ch343 = NULL;
		} else {
			tty_port_get(&ch343->port);
			mutex_unlock(&ch343->mutex);
		}
	}
	mutex_unlock(&ch343_minors_lock);
	return ch343;
}

static int ch343_alloc_minor(struct ch343 *ch343)
{
	int minor;

	mutex_lock(&ch343_minors_lock);
	minor = idr_alloc(&ch343_minors, ch343, 0, CH343_TTY_MINORS,
			  GFP_KERNEL);
	mutex_unlock(&ch343_minors_lock);

	return minor;
}

/* Release the minor number associated with 'ch343'. */
static void ch343_release_minor(struct ch343 *ch343)
{
	mutex_lock(&ch343_minors_lock);
	idr_remove(&ch343_minors, ch343->minor);
	mutex_unlock(&ch343_minors_lock);
}

#else

/*
 * Look up an CH343 structure by index. If found and not disconnected,
 * increment its refcount and return it with its mutex held.
 */
static struct ch343 *ch343_get_by_index(unsigned int index)
{
	struct ch343 *ch343;

	mutex_lock(&ch343_minors_lock);
	ch343 = ch343_table[index];
	if (ch343) {
		mutex_lock(&ch343->mutex);
		if (ch343->disconnected) {
			mutex_unlock(&ch343->mutex);
			ch343 = NULL;
		} else {
			tty_port_get(&ch343->port);
			mutex_unlock(&ch343->mutex);
		}
	}
	mutex_unlock(&ch343_minors_lock);

	return ch343;
}

/*
 * Try to find an available minor number and if found,
 * associate it with 'ch343'.
 */
static int ch343_alloc_minor(struct ch343 *ch343)
{
	int minor;

	mutex_lock(&ch343_minors_lock);
	for (minor = 0; minor < ch343_TTY_MINORS; minor++) {
		if (!ch343_table[minor]) {
			ch343_table[minor] = ch343;
			break;
		}
	}
	mutex_unlock(&ch343_minors_lock);

	return minor;
}

/* Release the minor number associated with 'ch343'. */
static void ch343_release_minor(struct ch343 *ch343)
{
	mutex_lock(&ch343_minors_lock);
	ch343_table[ch343->minor] = NULL;
	mutex_unlock(&ch343_minors_lock);
}

#endif

static int ch343_control_out(struct ch343 *ch343, u8 request, u16 value,
			     u16 index)
{
	int retval;

	retval = usb_autopm_get_interface(ch343->control);
	if (retval)
		return retval;
	retval = usb_control_msg(
		ch343->dev, usb_sndctrlpipe(ch343->dev, 0), request,
		USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_OUT, value,
		index, NULL, 0, DEFAULT_TIMEOUT);
	usb_autopm_put_interface(ch343->control);

	return retval;
}

static int ch343_control_in(struct ch343 *ch343, u8 request, u16 value,
			    u16 index, char *buf, unsigned bufsize)
{
	int retval;

	retval = usb_autopm_get_interface(ch343->control);
	if (retval)
		return retval;
	retval = usb_control_msg(
		ch343->dev, usb_rcvctrlpipe(ch343->dev, 0), request,
		USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN, value,
		index, buf, bufsize, DEFAULT_TIMEOUT);
	usb_autopm_put_interface(ch343->control);

	return retval;
}

static int ch343_control_msg_out(struct ch343 *ch343, u8 request,
				 u8 requesttype, u16 value, u16 index,
				 void *buf, unsigned bufsize)
{
	int retval;
	char *buffer;

	buffer = kmalloc(bufsize, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	retval = copy_from_user(buffer, (char __user *)buf, bufsize);
	if (retval)
		goto out;

	retval = usb_autopm_get_interface(ch343->control);
	if (retval)
		goto out;
	retval = usb_control_msg(ch343->dev,
				 usb_sndctrlpipe(ch343->dev, 0), request,
				 requesttype, value, index, buffer,
				 bufsize, DEFAULT_TIMEOUT);
	usb_autopm_put_interface(ch343->control);

out:
	kfree(buffer);
	return retval;
}

static int ch343_control_msg_in(struct ch343 *ch343, u8 request,
				u8 requesttype, u16 value, u16 index,
				void *buf, unsigned bufsize)
{
	int retval;
	char *buffer;

	buffer = kmalloc(bufsize, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	retval = usb_autopm_get_interface(ch343->control);
	if (retval)
		goto out;
	retval = usb_control_msg(ch343->dev,
				 usb_rcvctrlpipe(ch343->dev, 0), request,
				 requesttype, value, index, buffer,
				 bufsize, DEFAULT_TIMEOUT);
	if (retval > 0) {
		if (copy_to_user((char __user *)buf, buffer, retval)) {
			retval = -EFAULT;
		}
	}
	usb_autopm_put_interface(ch343->control);

out:
	kfree(buffer);
	return retval;
}

static inline int ch343_set_control(struct ch343 *ch343, int control)
{
	if (ch343->iface <= 1)
		return ch343_control_out(ch343, CMD_C2 + ch343->iface,
					 ~control, 0x0000);
	else if (ch343->iface <= 3)
		return ch343_control_out(
			ch343, CMD_C2 + 0x10 + (ch343->iface - 2),
			~control, 0x0000);
	else
		return -1;
}

static inline int ch343_set_line(struct ch343 *ch343,
				 struct usb_cdc_line_coding *line)
{
	return 0;
}

static int ch343_configure(struct ch343 *ch343)
{
	char *buffer;
	int r;
	const unsigned size = 8;
	u8 chiptype;
	u8 chipver;

	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	r = ch343_control_in(ch343, CMD_C6, 0, 0, buffer, size);
	if (r <= 0)
		goto out;

	chipver = buffer[0];
	chiptype = buffer[1];
	ch343->iosupport = true;

	switch (ch343->idProduct) {
	case 0x55D2:
		ch343->num_ports = 2;
		if (chiptype == 0x41)
			ch343->chiptype = CHIP_CH342K;
		else
			ch343->chiptype = CHIP_CH342F;
		if (chiptype != 0x48 || chipver < 0x45)
			ch343->iosupport = false;
		break;
	case 0x55D3:
		ch343->num_ports = 1;
		if (chiptype == 0x02)
			ch343->chiptype = CHIP_CH343J;
		else if (chiptype == 0x01)
			ch343->chiptype = CHIP_CH343K;
		else if (chiptype == 0x18)
			ch343->chiptype = CHIP_CH343G_AUTOBAUD;
		else
			ch343->chiptype = CHIP_CH343GP;
		ch343->iosupport = false;
		break;
	case 0x55D4:
		ch343->num_ports = 1;
		if (chiptype == 0x09)
			ch343->chiptype = CHIP_CH9102X;
		else
			ch343->chiptype = CHIP_CH9102F;
		break;
	case 0x55D5:
		ch343->num_ports = 4;
		if (chiptype == 0xC0) {
			if ((buffer[0] & 0xF0) == 0x40)
				ch343->chiptype = CHIP_CH344L;
			else
				ch343->chiptype = CHIP_CH344L_V2;
		} else
			ch343->chiptype = CHIP_CH344Q;
		break;
	case 0x55D7:
		ch343->num_ports = 2;
		ch343->chiptype = CHIP_CH9103M;
		break;
	case 0x55D8:
		ch343->num_ports = 1;
		if (chiptype == 0x0A)
			ch343->chiptype = CHIP_CH9101RY;
		else
			ch343->chiptype = CHIP_CH9101UH;
		break;
	case 0x55DB:
	case 0x55DD:
		ch343->num_ports = 1;
		ch343->chiptype = CHIP_CH347TF;
		break;
	case 0x55DA:
	case 0x55DE:
		ch343->num_ports = 2;
		ch343->chiptype = CHIP_CH347TF;
		break;
	case 0x55E7:
		ch343->num_ports = 1;
		ch343->chiptype = CHIP_CH339W;
		ch343->iosupport = false;
		break;
	case 0x55DF:
		ch343->num_ports = 4;
		ch343->chiptype = CHIP_CH9104L;
		break;
	case 0x55E9:
		ch343->num_ports = 1;
		ch343->chiptype = CHIP_CH9111L_M0;
		break;
	case 0x55EA:
		ch343->num_ports = 1;
		ch343->chiptype = CHIP_CH9111L_M1;
		break;
	case 0x55E8:
		ch343->num_ports = 4;
		chiptype = buffer[2];
		if (chiptype == 0x48)
			ch343->chiptype = CHIP_CH9114L;
		else if (chiptype == 0x49)
			ch343->chiptype = CHIP_CH9114W;
		else if (chiptype == 0x4A)
			ch343->chiptype = CHIP_CH9114F;
		break;
	case 0x55EB:
		ch343->num_ports = 1;
		if (buffer[4] & 0x01)
			ch343->chiptype = CHIP_CH346C_M1;
		else
			ch343->chiptype = CHIP_CH346C_M0;
		break;
	case 0x55EC:
		ch343->num_ports = 2;
		ch343->chiptype = CHIP_CH346C_M2;
		break;
	default:
		break;
	}

out:
	kfree(buffer);
	return r < 0 ? r : 0;
}

static int ch343_wb_alloc(struct ch343 *ch343)
{
	int i, wbn;
	struct ch343_wb *wb;

	wbn = 0;
	i = 0;
	for (;;) {
		wb = &ch343->wb[wbn];
		if (!wb->use) {
			wb->use = 1;
			return wbn;
		}
		wbn = (wbn + 1) % CH343_NW;
		if (++i >= CH343_NW)
			return -1;
	}
}

static int ch343_wb_is_avail(struct ch343 *ch343)
{
	int i, n;
	unsigned long flags;

	n = CH343_NW;
	spin_lock_irqsave(&ch343->write_lock, flags);
	for (i = 0; i < CH343_NW; i++)
		n -= ch343->wb[i].use;
	spin_unlock_irqrestore(&ch343->write_lock, flags);
	return n;
}

static void ch343_write_done(struct ch343 *ch343, struct ch343_wb *wb)
{
	wb->use = 0;
	ch343->transmitting--;
	usb_autopm_put_interface_async(ch343->control);
}

static int ch343_start_wb(struct ch343 *ch343, struct ch343_wb *wb)
{
	int rc;

	ch343->transmitting++;

	wb->urb->transfer_buffer = wb->buf;
	wb->urb->transfer_dma = wb->dmah;
	wb->urb->transfer_buffer_length = wb->len;
	wb->urb->dev = ch343->dev;

	rc = usb_submit_urb(wb->urb, GFP_ATOMIC);
	if (rc < 0) {
		dev_err(&ch343->data->dev,
			"%s - usb_submit_urb(write bulk) failed: %d\n",
			__func__, rc);
		ch343_write_done(ch343, wb);
	}
	return rc;
}

static void ch343_update_status(struct ch343 *ch343, unsigned char *data,
				size_t len)
{
	unsigned long flags;
	u8 status;
	u8 difference;
	u8 type = data[0];
	u8 handled = 0;

	if (len < 4)
		return;

	if (ch343->chiptype == CHIP_CH344L) {
		if (data[0] != 0x00)
			return;
		type = data[1];
	} else if (ch343->chiptype == CHIP_CH346C_M0 ||
		   ch343->chiptype == CHIP_CH346C_M1 ||
		   ch343->chiptype == CHIP_CH346C_M2 ||
		   ch343->chiptype == CHIP_CH339W ||
		   ch343->chiptype == CHIP_CH347TF ||
		   ch343->chiptype == CHIP_CH344Q ||
		   ch343->chiptype == CHIP_CH344L_V2 ||
		   ch343->chiptype == CHIP_CH9104L ||
		   ch343->chiptype == CHIP_CH9114L ||
		   ch343->chiptype == CHIP_CH9114F ||
		   ch343->chiptype == CHIP_CH9114W ||
		   ch343->chiptype == CHIP_CH9111L_M0 ||
		   ch343->chiptype == CHIP_CH9111L_M1) {
		type = data[1];
	}

	if (type & CH343_CTT_M) {
		status = ~data[len - 1] & CH343_CTI_ST;
		if (ch343->chiptype == CHIP_CH344L ||
		    ch343->chiptype == CHIP_CH344L_V2)
			status &= CH343_CTI_C;

		if (!ch343->clocal &&
		    (ch343->ctrlin & status & CH343_CTI_DC)) {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0))
			struct tty_struct *tty =
				tty_port_tty_get(&ch343->port);
			tty_hangup(tty);
#else
			tty_port_tty_hangup(&ch343->port, false);
#endif
		}

		spin_lock_irqsave(&ch343->read_lock, flags);
		difference = status ^ ch343->ctrlin;
		ch343->ctrlin = status;
		ch343->oldcount = ch343->iocount;

		if (difference) {
			if (difference & CH343_CTI_C) {
				ch343->iocount.cts++;
			}
			if (difference & CH343_CTI_DS) {
				ch343->iocount.dsr++;
			}
			if (difference & CH343_CTI_R) {
				ch343->iocount.rng++;
			}
			if (difference & CH343_CTI_DC) {
				ch343->iocount.dcd++;
			}
			spin_unlock_irqrestore(&ch343->read_lock, flags);
			wake_up_interruptible(&ch343->wioctl);
		} else
			spin_unlock_irqrestore(&ch343->read_lock, flags);
		handled = 1;
	}
	if (type & CH343_CTT_O) {
		spin_lock_irqsave(&ch343->read_lock, flags);
		ch343->oldcount = ch343->iocount;
		ch343->iocount.overrun++;
		spin_unlock_irqrestore(&ch343->read_lock, flags);
		handled = 1;
	}
	if ((type & CH343_CTT_F) == CH343_CTT_F) {
		spin_lock_irqsave(&ch343->read_lock, flags);
		ch343->oldcount = ch343->iocount;
		ch343->iocount.frame++;
		spin_unlock_irqrestore(&ch343->read_lock, flags);
		handled = 1;
	} else if (type & CH343_CTT_P) {
		spin_lock_irqsave(&ch343->read_lock, flags);
		ch343->oldcount = ch343->iocount;
		ch343->iocount.parity++;
		spin_unlock_irqrestore(&ch343->read_lock, flags);
		handled = 1;
	}
	if (!handled)
		dev_err(&ch343->control->dev,
			"%s - unknown status received:"
			"len:%d, data0:0x%x, data1:0x%x\n",
			__func__, (int)len, data[0], data[1]);
}

static void ch343_ctrl_irq(struct urb *urb)
{
	struct ch343 *ch343 = urb->context;
	unsigned char *data = urb->transfer_buffer;
	unsigned int len = urb->actual_length;
	int status = urb->status;
	int retval;

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(&ch343->control->dev,
			"%s - urb shutting down with status: %d\n",
			__func__, status);
		return;
	default:
		dev_dbg(&ch343->control->dev,
			"%s - nonzero urb status received: %d\n", __func__,
			status);
		goto exit;
	}

	usb_mark_last_busy(ch343->dev);
	ch343_update_status(ch343, data, len);
exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval && retval != -EPERM)
		dev_err(&ch343->control->dev,
			"%s - usb_submit_urb failed: %d\n", __func__,
			retval);
}

static int ch343_submit_read_urb(struct ch343 *ch343, int index,
				 gfp_t mem_flags)
{
	int res;

	if (!test_and_clear_bit(index, &ch343->read_urbs_free))
		return 0;

	res = usb_submit_urb(ch343->read_urbs[index], mem_flags);
	if (res) {
		if (res != -EPERM) {
			dev_err(&ch343->data->dev,
				"%s - usb_submit_urb failed: %d\n",
				__func__, res);
		}
		set_bit(index, &ch343->read_urbs_free);
		return res;
	}
	return 0;
}

static int ch343_submit_read_urbs(struct ch343 *ch343, gfp_t mem_flags)
{
	int res;
	int i;

	for (i = 0; i < ch343->rx_buflimit; ++i) {
		res = ch343_submit_read_urb(ch343, i, mem_flags);
		if (res)
			return res;
	}
	return 0;
}

static void ch343_process_read_urb(struct ch343 *ch343, struct urb *urb)
{
	if (!urb->actual_length)
		return;

	ch343->iocount.rx += urb->actual_length;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0))
	tty_insert_flip_string(&ch343->port, urb->transfer_buffer,
			       urb->actual_length);
	tty_flip_buffer_push(&ch343->port);
#else
	struct tty_struct *tty = tty_port_tty_get(&ch343->port);
	tty_insert_flip_string(tty, urb->transfer_buffer,
			       urb->actual_length);
	tty_flip_buffer_push(tty);
#endif
}

static void ch343_read_bulk_callback(struct urb *urb)
{
	struct ch343_rb *rb = urb->context;
	struct ch343 *ch343 = rb->instance;
	int status = urb->status;

	if (!ch343->dev) {
		set_bit(rb->index, &ch343->read_urbs_free);
		dev_dbg(&ch343->data->dev, "%s - disconnected\n",
			__func__);
		return;
	}

	if (status) {
		set_bit(rb->index, &ch343->read_urbs_free);
		dev_dbg(&ch343->data->dev,
			"%s - non-zero urb status: %d\n", __func__,
			status);
		return;
	}

	usb_mark_last_busy(ch343->dev);
	ch343_process_read_urb(ch343, urb);
	set_bit(rb->index, &ch343->read_urbs_free);
	ch343_submit_read_urb(ch343, rb->index, GFP_ATOMIC);
}

static void ch343_write_bulk(struct urb *urb)
{
	struct ch343_wb *wb = urb->context;
	struct ch343 *ch343 = wb->instance;
	unsigned long flags;
	int status = urb->status;

	if (status || (urb->actual_length != urb->transfer_buffer_length))
		dev_vdbg(&ch343->data->dev, "%s - len %d/%d, status %d\n",
			 __func__, urb->actual_length,
			 urb->transfer_buffer_length, status);

	ch343->iocount.tx += urb->actual_length;
	spin_lock_irqsave(&ch343->write_lock, flags);
	ch343_write_done(ch343, wb);
	wake_up_interruptible(&ch343->sendioctl);
	spin_unlock_irqrestore(&ch343->write_lock, flags);
	schedule_work(&ch343->work);
}

static void ch343_softint(struct work_struct *work)
{
	struct ch343 *ch343 = container_of(work, struct ch343, work);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0))
	struct tty_struct *tty;

	tty = tty_port_tty_get(&ch343->port);
	if (!tty)
		return;
	tty_wakeup(tty);
	tty_kref_put(tty);
#else
	tty_port_tty_wakeup(&ch343->port);
#endif
}

static int ch343_tty_install(struct tty_driver *driver,
			     struct tty_struct *tty)
{
	struct ch343 *ch343;
	int retval;

	ch343 = ch343_get_by_index(tty->index);
	if (!ch343)
		return -ENODEV;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	retval = tty_standard_install(driver, tty);
	if (retval)
		goto error_init_termios;

	tty->driver_data = ch343;
#else
	retval = tty_init_termios(tty);
	if (retval)
		goto error_init_termios;

	tty->driver_data = ch343;

	/* Final install (we use the default method) */
	tty_driver_kref_get(driver);
	tty->count++;
	driver->ttys[tty->index] = tty;
#endif

	return 0;

error_init_termios:
	tty_port_put(&ch343->port);
	return retval;
}

static int ch343_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct ch343 *ch343 = tty->driver_data;

	if (tty)
		ch343_tty_set_termios(tty, NULL);

	return tty_port_open(&ch343->port, tty, filp);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0))
static void ch343_port_dtr_rts(struct tty_port *port, bool raise)
#else
static void ch343_port_dtr_rts(struct tty_port *port, int raise)
#endif
{
	struct ch343 *ch343 = container_of(port, struct ch343, port);
	int res;

#ifdef IGNORE_RTSDTR
	return;
#endif

	if (raise)
		ch343->ctrlout |= CH343_CTO_D | CH343_CTO_R;
	else
		ch343->ctrlout &= ~(CH343_CTO_D | CH343_CTO_R);

	res = ch343_set_control(ch343, ch343->ctrlout);
	if (res)
		dev_err(&ch343->control->dev, "failed to set dtr/rts\n");
}

static int ch343_port_activate(struct tty_port *port,
			       struct tty_struct *tty)
{
	struct ch343 *ch343 = container_of(port, struct ch343, port);
	int retval = -ENODEV;
	int i;

	mutex_lock(&ch343->mutex);
	if (ch343->disconnected)
		goto disconnected;

	retval = usb_autopm_get_interface(ch343->control);
	if (retval)
		goto error_get_interface;

	set_bit(TTY_NO_WRITE_SPLIT, &tty->flags);
	ch343->control->needs_remote_wakeup = 1;

	retval = usb_submit_urb(ch343->ctrlurb, GFP_KERNEL);
	if (retval) {
		dev_err(&ch343->control->dev,
			"%s - usb_submit_urb(ctrl cmd) failed\n",
			__func__);
		goto error_submit_urb;
	}
	retval = ch343_submit_read_urbs(ch343, GFP_KERNEL);
	if (retval)
		goto error_submit_read_urbs;

	if (ch343->chiptype == CHIP_CH9114L ||
	    ch343->chiptype == CHIP_CH9114F ||
	    ch343->chiptype == CHIP_CH9114W ||
	    ch343->chiptype == CHIP_CH346C_M2) {
		retval = ch343_control_out(
			ch343, CMD_C8, 0x01 | (ch343->iface << 8), 0x00);
		if (retval) {
			goto error_submit_read_urbs;
		}
	}

	usb_autopm_put_interface(ch343->control);
	mutex_unlock(&ch343->mutex);

	return 0;

error_submit_read_urbs:
	for (i = 0; i < ch343->rx_buflimit; i++)
		usb_kill_urb(ch343->read_urbs[i]);
error_submit_urb:
	usb_kill_urb(ch343->ctrlurb);
	usb_autopm_put_interface(ch343->control);
error_get_interface:
disconnected:
	mutex_unlock(&ch343->mutex);
	return usb_translate_errors(retval);
}

static void ch343_port_destruct(struct tty_port *port)
{
	struct ch343 *ch343 = container_of(port, struct ch343, port);

	ch343_release_minor(ch343);
	usb_put_intf(ch343->control);
	memset(ch343, 0x00, sizeof(struct ch343));
	kfree(ch343);
}

static void ch343_port_shutdown(struct tty_port *port)
{
	struct ch343 *ch343 = container_of(port, struct ch343, port);
	struct urb *urb;
	struct ch343_wb *wb;
	int i;
	int r;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0))

	usb_autopm_get_interface_no_resume(ch343->control);
	ch343->control->needs_remote_wakeup = 0;
	usb_autopm_put_interface(ch343->control);

	for (;;) {
		urb = usb_get_from_anchor(&ch343->delayed);
		if (!urb)
			break;
		wb = urb->context;
		wb->use = 0;
		usb_autopm_put_interface_async(ch343->control);
	}

	usb_kill_urb(ch343->ctrlurb);
	for (i = 0; i < CH343_NW; i++)
		usb_kill_urb(ch343->wb[i].urb);
	for (i = 0; i < ch343->rx_buflimit; i++)
		usb_kill_urb(ch343->read_urbs[i]);

#else
	mutex_lock(&ch343->mutex);
	if (!ch343->disconnected) {
		usb_autopm_get_interface(ch343->control);
		ch343_set_control(ch343, ch343->ctrlout = 0);

		usb_kill_urb(ch343->ctrlurb);
		for (i = 0; i < CH343_NW; i++)
			usb_kill_urb(ch343->wb[i].urb);
		for (i = 0; i < ch343->rx_buflimit; i++)
			usb_kill_urb(ch343->read_urbs[i]);
		ch343->control->needs_remote_wakeup = 0;

		usb_autopm_put_interface(ch343->control);
	}
	mutex_unlock(&ch343->mutex);
#endif

	if (ch343->chiptype == CHIP_CH9114L ||
	    ch343->chiptype == CHIP_CH9114F ||
	    ch343->chiptype == CHIP_CH9114W ||
	    ch343->chiptype == CHIP_CH346C_M2) {
		r = ch343_control_out(ch343, CMD_C8,
				      0x02 | (ch343->iface << 8), 0x00);
		if (r)
			dev_err(&ch343->control->dev, "%s - failed: %d\n",
				__func__, r);
	}
}

static void ch343_tty_cleanup(struct tty_struct *tty)
{
	struct ch343 *ch343 = tty->driver_data;
	tty_port_put(&ch343->port);
}

static void ch343_tty_hangup(struct tty_struct *tty)
{
	struct ch343 *ch343 = tty->driver_data;
	tty_port_hangup(&ch343->port);
}

static void ch343_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct ch343 *ch343 = tty->driver_data;
	tty_port_close(&ch343->port, tty, filp);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0))
static ssize_t ch343_tty_write(struct tty_struct *tty, const u8 *buf,
			       size_t count)
#else
static int ch343_tty_write(struct tty_struct *tty,
			   const unsigned char *buf, int count)
#endif
{
	struct ch343 *ch343 = tty->driver_data;
	int stat;
	unsigned long flags;
	int wbn;
	struct ch343_wb *wb;
	int timeout;

	if (!count)
		return 0;

retry:
	spin_lock_irqsave(&ch343->write_lock, flags);
	wbn = ch343_wb_alloc(ch343);
	if (wbn < 0) {
		spin_unlock_irqrestore(&ch343->write_lock, flags);
		timeout = wait_event_interruptible_timeout(
			ch343->sendioctl, ch343_wb_is_avail(ch343),
			msecs_to_jiffies(DEFAULT_TIMEOUT));
		if (timeout <= 0) {
			return -ETIMEDOUT;
		} else
			goto retry;
	}
	wb = &ch343->wb[wbn];

	if (!ch343->dev) {
		wb->use = 0;
		spin_unlock_irqrestore(&ch343->write_lock, flags);
		return -ENODEV;
	}

	count = (count > ch343->writesize) ? ch343->writesize : count;

	memcpy(wb->buf, buf, count);
	wb->len = count;

	stat = usb_autopm_get_interface_async(ch343->control);
	if (stat) {
		wb->use = 0;
		spin_unlock_irqrestore(&ch343->write_lock, flags);
		return stat;
	}

	if (ch343->susp_count) {
		usb_anchor_urb(wb->urb, &ch343->delayed);
		spin_unlock_irqrestore(&ch343->write_lock, flags);
		return count;
	}

	stat = ch343_start_wb(ch343, wb);
	spin_unlock_irqrestore(&ch343->write_lock, flags);

	if (stat < 0)
		return stat;
	return count;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0))
static unsigned int ch343_tty_write_room(struct tty_struct *tty)
#else
static int ch343_tty_write_room(struct tty_struct *tty)
#endif
{
	struct ch343 *ch343 = tty->driver_data;

	return ch343_wb_is_avail(ch343) ? ch343->writesize : 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0))
static unsigned int ch343_tty_chars_in_buffer(struct tty_struct *tty)
#else
static int ch343_tty_chars_in_buffer(struct tty_struct *tty)
#endif
{
	struct ch343 *ch343 = tty->driver_data;

	if (ch343->disconnected)
		return 0;

	return (CH343_NW - ch343_wb_is_avail(ch343)) * ch343->writesize;
}

static int ch343_tty_break_ctl(struct tty_struct *tty, int state)
{
	struct ch343 *ch343 = tty->driver_data;
	int retval;
	uint16_t reg_contents;
	uint8_t *regbuf;

	regbuf = kmalloc(2, GFP_KERNEL);
	if (!regbuf)
		return -1;

	if (state != 0) {
		if (ch343->chiptype == CHIP_CH346C_M0 ||
		    ch343->chiptype == CHIP_CH346C_M1 ||
		    ch343->chiptype == CHIP_CH346C_M2 ||
		    ch343->chiptype == CHIP_CH339W ||
		    ch343->chiptype == CHIP_CH347TF ||
		    ch343->chiptype == CHIP_CH344L ||
		    ch343->chiptype == CHIP_CH344Q ||
		    ch343->chiptype == CHIP_CH344L_V2 ||
		    ch343->chiptype == CHIP_CH9104L ||
		    ch343->chiptype == CHIP_CH9114L ||
		    ch343->chiptype == CHIP_CH9114F ||
		    ch343->chiptype == CHIP_CH9114W ||
		    ch343->chiptype == CHIP_CH9111L_M0 ||
		    ch343->chiptype == CHIP_CH9111L_M1) {
			regbuf[0] = ch343->iface;
			regbuf[1] = 0x01;
		} else {
			regbuf[0] = CH343_N_B;
			regbuf[1] = 0x00;
		}
	} else {
		if (ch343->chiptype == CHIP_CH346C_M0 ||
		    ch343->chiptype == CHIP_CH346C_M1 ||
		    ch343->chiptype == CHIP_CH346C_M2 ||
		    ch343->chiptype == CHIP_CH339W ||
		    ch343->chiptype == CHIP_CH347TF ||
		    ch343->chiptype == CHIP_CH344L ||
		    ch343->chiptype == CHIP_CH344Q ||
		    ch343->chiptype == CHIP_CH344L_V2 ||
		    ch343->chiptype == CHIP_CH9104L ||
		    ch343->chiptype == CHIP_CH9114L ||
		    ch343->chiptype == CHIP_CH9114F ||
		    ch343->chiptype == CHIP_CH9114W ||
		    ch343->chiptype == CHIP_CH9111L_M0 ||
		    ch343->chiptype == CHIP_CH9111L_M1) {
			regbuf[0] = ch343->iface;
			regbuf[1] = 0x00;
		} else {
			regbuf[0] = CH343_N_B | CH343_N_AB;
			regbuf[1] = 0x00;
		}
	}
	reg_contents = get_unaligned_le16(regbuf);

	if (ch343->chiptype == CHIP_CH346C_M0 ||
	    ch343->chiptype == CHIP_CH346C_M1 ||
	    ch343->chiptype == CHIP_CH346C_M2 ||
	    ch343->chiptype == CHIP_CH339W ||
	    ch343->chiptype == CHIP_CH347TF ||
	    ch343->chiptype == CHIP_CH344L ||
	    ch343->chiptype == CHIP_CH344Q ||
	    ch343->chiptype == CHIP_CH344L_V2 ||
	    ch343->chiptype == CHIP_CH9104L ||
	    ch343->chiptype == CHIP_CH9114L ||
	    ch343->chiptype == CHIP_CH9114F ||
	    ch343->chiptype == CHIP_CH9114W ||
	    ch343->chiptype == CHIP_CH9111L_M0 ||
	    ch343->chiptype == CHIP_CH9111L_M1) {
		retval = ch343_control_out(ch343, CMD_C4, reg_contents,
					   0x00);
	} else {
		if (ch343->iface)
			retval = ch343_control_out(ch343, CMD_C4, 0x00,
						   reg_contents);
		else
			retval = ch343_control_out(ch343, CMD_C4,
						   reg_contents, 0x00);
	}

	if (retval < 0)
		dev_err(&ch343->control->dev,
			"%s - USB control write error (%d)\n", __func__,
			retval);

	kfree(regbuf);
	return retval;
}

static int ch343_tty_tiocmget(struct tty_struct *tty)
{
	struct ch343 *ch343 = tty->driver_data;
	unsigned long flags;
	unsigned int result;

	spin_lock_irqsave(&ch343->read_lock, flags);
	result = (ch343->ctrlout & CH343_CTO_D ? TIOCM_DTR : 0) |
		 (ch343->ctrlout & CH343_CTO_R ? TIOCM_RTS : 0) |
		 (ch343->ctrlin & CH343_CTI_C ? TIOCM_CTS : 0) |
		 (ch343->ctrlin & CH343_CTI_DS ? TIOCM_DSR : 0) |
		 (ch343->ctrlin & CH343_CTI_R ? TIOCM_RI : 0) |
		 (ch343->ctrlin & CH343_CTI_DC ? TIOCM_CD : 0);
	spin_unlock_irqrestore(&ch343->read_lock, flags);

	return result;
}

static int ch343_tty_tiocmset(struct tty_struct *tty, unsigned int set,
			      unsigned int clear)
{
	struct ch343 *ch343 = tty->driver_data;
	unsigned int newctrl;

	newctrl = ch343->ctrlout;
	set = (set & TIOCM_DTR ? CH343_CTO_D : 0) |
	      (set & TIOCM_RTS ? CH343_CTO_R : 0);
	clear = (clear & TIOCM_DTR ? CH343_CTO_D : 0) |
		(clear & TIOCM_RTS ? CH343_CTO_R : 0);

	newctrl = (newctrl & ~clear) | set;

	if (ch343->ctrlout == newctrl) {
		return 0;
	}

	return ch343_set_control(ch343, ch343->ctrlout = newctrl);
}

static int ch343_get_icount(struct tty_struct *tty,
			    struct serial_icounter_struct *icount)
{
	struct ch343 *ch343 = tty->driver_data;
	struct async_icount cnow;
	unsigned long flags;

	spin_lock_irqsave(&ch343->read_lock, flags);
	cnow = ch343->iocount;
	spin_unlock_irqrestore(&ch343->read_lock, flags);

	icount->cts = cnow.cts;
	icount->dsr = cnow.dsr;
	icount->rng = cnow.rng;
	icount->dcd = cnow.dcd;
	icount->tx = cnow.tx;
	icount->rx = cnow.rx;
	icount->frame = cnow.frame;
	icount->parity = cnow.parity;
	icount->overrun = cnow.overrun;
	icount->brk = cnow.brk;
	icount->buf_overrun = cnow.buf_overrun;

	return 0;
}

static int ch343_get_serial_info(struct ch343 *ch343,
				 struct serial_struct __user *info)
{
	struct serial_struct tmp;

	if (!info)
		return -EINVAL;

	memset(&tmp, 0, sizeof(tmp));
	tmp.flags = ASYNC_LOW_LATENCY;
	tmp.xmit_fifo_size = ch343->writesize;
	tmp.baud_base = le32_to_cpu(ch343->line.dwDTERate);
	tmp.close_delay = ch343->port.close_delay / 10;
	tmp.closing_wait = ch343->port.closing_wait ==
					   ASYNC_CLOSING_WAIT_NONE ?
				   ASYNC_CLOSING_WAIT_NONE :
				   ch343->port.closing_wait / 10;

	if (copy_to_user(info, &tmp, sizeof(tmp)))
		return -EFAULT;
	else
		return 0;
}

static int ch343_set_serial_info(struct ch343 *ch343,
				 struct serial_struct __user *newinfo)
{
	struct serial_struct new_serial;
	unsigned int closing_wait, close_delay;
	int retval = 0;

	if (copy_from_user(&new_serial, newinfo, sizeof(new_serial)))
		return -EFAULT;

	close_delay = new_serial.close_delay * 10;
	closing_wait = new_serial.closing_wait == ASYNC_CLOSING_WAIT_NONE ?
			       ASYNC_CLOSING_WAIT_NONE :
			       new_serial.closing_wait * 10;

	mutex_lock(&ch343->port.mutex);

	if (!capable(CAP_SYS_ADMIN)) {
		if ((close_delay != ch343->port.close_delay) ||
		    (closing_wait != ch343->port.closing_wait))
			retval = -EPERM;
		else
			retval = -EOPNOTSUPP;
	} else {
		ch343->port.close_delay = close_delay;
		ch343->port.closing_wait = closing_wait;
	}

	mutex_unlock(&ch343->port.mutex);
	return retval;
}

static int ch343_wait_serial_change(struct ch343 *ch343, unsigned long arg)
{
	int rv = 0;
	DECLARE_WAITQUEUE(wait, current);
	struct async_icount old, new;

	do {
		spin_lock_irq(&ch343->read_lock);
		old = ch343->oldcount;
		new = ch343->iocount;
		ch343->oldcount = new;
		spin_unlock_irq(&ch343->read_lock);

		if ((arg & TIOCM_CTS) && old.cts != new.cts)
			break;
		if ((arg & TIOCM_DSR) && old.dsr != new.dsr)
			break;
		if ((arg & TIOCM_RI) && old.rng != new.rng)
			break;
		if ((arg & TIOCM_CD) && old.dcd != new.dcd)
			break;

		add_wait_queue(&ch343->wioctl, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
		remove_wait_queue(&ch343->wioctl, &wait);
		if (ch343->disconnected) {
			if (arg & TIOCM_CD)
				break;
			else
				rv = -ENODEV;
		} else {
			if (signal_pending(current))
				rv = -ERESTARTSYS;
		}
	} while (!rv);

	return rv;
}

static int
ch343_get_serial_usage(struct ch343 *ch343,
		       struct serial_icounter_struct __user *count)
{
	struct serial_icounter_struct icount;
	int rv = 0;

	memset(&icount, 0, sizeof(icount));
	icount.cts = ch343->iocount.cts;
	icount.dsr = ch343->iocount.dsr;
	icount.rng = ch343->iocount.rng;
	icount.dcd = ch343->iocount.dcd;
	icount.tx = ch343->iocount.tx;
	icount.rx = ch343->iocount.rx;
	icount.frame = ch343->iocount.frame;
	icount.overrun = ch343->iocount.overrun;
	icount.parity = ch343->iocount.parity;
	icount.brk = ch343->iocount.brk;
	icount.buf_overrun = ch343->iocount.buf_overrun;

	if (copy_to_user(count, &icount, sizeof(icount)) > 0)
		rv = -EFAULT;

	return rv;
}

static int ch343_tty_ioctl(struct tty_struct *tty, unsigned int cmd,
			   unsigned long arg)
{
	struct ch343 *ch343 = tty->driver_data;
	int rv = 0;
	unsigned long arg1, arg2, arg3, arg4, arg5, arg6;
	u32 __user *argval = (u32 __user *)arg;
	u8 *buffer;

	buffer = kmalloc(512, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	switch (cmd) {
	case TIOCGSERIAL: /* gets serial port data */
		rv = ch343_get_serial_info(
			ch343, (struct serial_struct __user *)arg);
		break;
	case TIOCSSERIAL:
		rv = ch343_set_serial_info(
			ch343, (struct serial_struct __user *)arg);
		break;
	case TIOCMIWAIT:
		rv = usb_autopm_get_interface(ch343->control);
		if (rv < 0) {
			rv = -EIO;
			break;
		}
		rv = ch343_wait_serial_change(ch343, arg);
		usb_autopm_put_interface(ch343->control);
		break;
	case TIOCGICOUNT:
		rv = ch343_get_serial_usage(
			ch343,
			(struct serial_icounter_struct __user *)arg);
		break;
	case IOCTL_CMD_GETCHIPTYPE:
		if (put_user(ch343->chiptype, argval)) {
			rv = -EFAULT;
			goto out;
		}
		break;
	case IOCTL_CMD_GETUARTINDEX:
		if (put_user(ch343->iface, argval)) {
			rv = -EFAULT;
			goto out;
		}
		break;
	case IOCTL_CMD_CTRLIN:
		get_user(arg1, (u8 __user *)arg);
		get_user(arg2, ((u8 __user *)arg + 1));
		get_user(arg3, (u16 __user *)((u8 *)arg + 2));
		get_user(arg4, (u16 __user *)((u8 *)arg + 4));
		get_user(arg5, (u16 __user *)((u8 *)arg + 6));
		arg6 = (unsigned long)((u8 __user *)arg + 8);
		rv = ch343_control_msg_in(ch343, (u8)arg1, (u8)arg2,
					  (u16)arg3, (u16)arg4,
					  (u8 __user *)arg6, (u16)arg5);
		break;
	case IOCTL_CMD_CTRLOUT:
		get_user(arg1, (u8 __user *)arg);
		get_user(arg2, ((u8 __user *)arg + 1));
		get_user(arg3, (u16 __user *)((u8 *)arg + 2));
		get_user(arg4, (u16 __user *)((u8 *)arg + 4));
		get_user(arg5, (u16 __user *)((u8 *)arg + 6));
		arg6 = (unsigned long)((u8 __user *)arg + 8);
		rv = ch343_control_msg_out(ch343, (u8)arg1, (u8)arg2,
					   (u16)arg3, (u16)arg4,
					   (u8 __user *)arg6, (u16)arg5);
		if (rv != (u16)arg5) {
			rv = -EINVAL;
			goto out;
		}
		break;
	default:
		rv = -ENOIOCTLCMD;
		break;
	}
out:
	kfree(buffer);
	return rv;
}

static int ch343_get(struct ch343 *ch343, enum CHIPTYPE chiptype,
		     unsigned int bval, unsigned char *fct,
		     unsigned char *dvs)
{
	unsigned char a;
	unsigned char b;
	unsigned long c;
	char *buffer;
	int r = 0;
	const unsigned size = 8;
	u32 gfreq;
	u8 change;
	bool bfirst = false;

	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	if ((ch343->chiptype >= CHIP_CH9111L_M0 &&
	     ch343->chiptype <= CHIP_CH346C_M2) &&
	    bval >= 2000000) {
		if ((ch343->chiptype >= CHIP_CH9111L_M0 &&
		     ch343->chiptype <= CHIP_CH346C_M2) &&
		    (bval > 13000000)) {
			*fct = (bval + 500000) / 1000000 - 13;
			*dvs = 0xFF;
		} else {
			*fct = (unsigned char)(bval / 200);
			*dvs = (unsigned char)((bval / 200) >> 8);
		}
	} else if (ch343->chiptype == CHIP_CH347TF && bval > 358400) {
		*fct = (unsigned char)(bval / 200);
		*dvs = (unsigned char)((bval / 200) >> 8);
	} else {
		switch (bval) {
		case 6000000:
		case 4000000:
		case 2400000:
		case 921600:
		case 307200:
		case 256000:
			b = 7;
			c = 12000000;
			break;
		default:
			if (bval > 6000000 / 255) {
				b = 3;
				c = 6000000;
			} else if (bval > 750000 / 255) {
				b = 2;
				c = 750000;
			} else if (bval > 93750 / 255) {
				b = 1;
				c = 93750;
			} else {
				b = 0;
				c = 11719;
			}
			break;
		}
		a = (unsigned char)(c / bval);
		if (a == 0 || a == 0xFF)
			return -EINVAL;
		if ((c / a - bval) > (bval - c / (a + 1)))
			a++;
		a = 256 - a;

		*fct = a;
		*dvs = b;
	}

	if (chiptype == CHIP_CH9114L || chiptype == CHIP_CH9114F ||
	    chiptype == CHIP_CH9114W || chiptype == CHIP_CH346C_M2) {
retry:
		r = ch343_control_in(ch343, CMD_C7, 0x01, 0, buffer, size);
		if (r <= 0)
			goto out;
		else
			r = 0;
		gfreq = *(__le32 *)buffer;
		change = buffer[4];

		if (!change) {
			if ((gfreq == 120000000 && bval > 1000000 &&
			     (15000000 % bval)) ||
			    (gfreq == 96000000 && bval > 1000000 &&
			     (12000000 % bval)) ||
			    (gfreq == 80000000 && bval > 1000000 &&
			     (10000000 % bval))) {
				if (bfirst) {
					r = -EINVAL;
					goto out;
				} else {
					bfirst = true;
					r = ch343_control_out(
						ch343, CMD_C8,
						0x02 | (ch343->iface << 8),
						0x00);
					if (r) {
						dev_err(&ch343->control
								 ->dev,
							"%s - failed: %d\n",
							__func__, r);
						goto out;
					}
					r = ch343_control_out(
						ch343, CMD_C8,
						0x01 | (ch343->iface << 8),
						0x00);
					if (r) {
						dev_err(&ch343->control
								 ->dev,
							"%s - failed: %d\n",
							__func__, r);
						goto out;
					}
					goto retry;
				}
			}
		}
	}

out:
	kfree(buffer);
	return r;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
static void ch343_tty_set_termios(struct tty_struct *tty,
				  const struct ktermios *termios_old)
#else
static void ch343_tty_set_termios(struct tty_struct *tty,
				  struct ktermios *termios_old)
#endif
{
	struct ch343 *ch343 = tty->driver_data;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0))
	struct ktermios *termios = &tty->termios;
#else
	struct ktermios *termios = tty->termios;
#endif
	struct usb_ch343_line_coding newline;
	int newctrl = ch343->ctrlout;
	int r;

	unsigned char dvs = 0;
	unsigned char reg_count = 0;
	unsigned char fct = 0;
	unsigned char reg_value = 0;
	unsigned short value = 0;
	unsigned short index = 0;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0))
	if (termios_old &&
	    !tty_termios_hw_change(&tty->termios, termios_old)) {
#else
	if (termios_old &&
	    !tty_termios_hw_change(tty->termios, termios_old)) {
#endif
		return;
	}

	newline.dwDTERate = tty_get_baud_rate(tty);
	if (newline.dwDTERate == 0)
		newline.dwDTERate = 9600;
	r = ch343_get(ch343, ch343->chiptype, newline.dwDTERate, &fct,
		      &dvs);
	if (r) {
		dev_err(&ch343->control->dev,
			"%s - Bad termios setting, DTERate: %d.\n",
			__func__, newline.dwDTERate);
		return;
	}

	newline.bCharFormat = termios->c_cflag & CSTOPB ? 2 : 1;
	if (newline.bCharFormat == 2)
		reg_value |= CH343_L_SB;

	newline.bParityType =
		termios->c_cflag & PARENB ?
			(termios->c_cflag & PARODD ? 1 : 2) +
				(termios->c_cflag & CMSPAR ? 2 : 0) :
			0;

	switch (newline.bParityType) {
	case 0x01:
		reg_value |= CH343_L_P_O;
		break;
	case 0x02:
		reg_value |= CH343_L_P_E;
		break;
	case 0x03:
		reg_value |= CH343_L_P_M;
		break;
	case 0x04:
		reg_value |= CH343_L_P_S;
		break;
	default:
		break;
	}

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		newline.bDataBits = 5;
		reg_value |= CH343_L_C5;
		break;
	case CS6:
		newline.bDataBits = 6;
		reg_value |= CH343_L_C6;
		break;
	case CS7:
		newline.bDataBits = 7;
		reg_value |= CH343_L_C7;
		break;
	case CS8:
	default:
		newline.bDataBits = 8;
		reg_value |= CH343_L_C8;
		break;
	}

	ch343->clocal = ((termios->c_cflag & CLOCAL) != 0);

	if (C_BAUD(tty) == B0) {
		newline.dwDTERate = ch343->line.dwDTERate;
		newctrl &= ~(CH343_CTO_D | CH343_CTO_R);
	} else if (termios_old && (termios_old->c_cflag & CBAUD) == B0) {
		newctrl |= CH343_CTO_D | CH343_CTO_R;
	}

	reg_value |= CH343_L_E_R | CH343_L_E_T;
	reg_count |= CH343_L_R_CT | CH343_L_R_CL | CH343_L_R_T;

	value |= reg_count;
	value |= (unsigned short)reg_value << 8;

	index |= 0x00 | dvs;
	index |= (unsigned short)fct << 8;
	if (ch343->iface <= 1)
		ch343_control_out(ch343, CMD_C1 + ch343->iface, value,
				  index);
	else if (ch343->iface <= 3)
		ch343_control_out(ch343,
				  CMD_C1 + 0x10 + (ch343->iface - 2),
				  value, index);

	if (memcmp(&ch343->line, &newline, sizeof newline))
		memcpy(&ch343->line, &newline, sizeof newline);

	if (C_CRTSCTS(tty)) {
		newctrl |= CH343_CTO_A | CH343_CTO_R;
	} else
		newctrl &= ~CH343_CTO_A;

	if (newctrl != ch343->ctrlout)
		ch343_set_control(ch343, ch343->ctrlout = newctrl);

	tty_encode_baud_rate(tty, newline.dwDTERate, newline.dwDTERate);
}

static const struct tty_port_operations ch343_port_ops = {
	.dtr_rts = ch343_port_dtr_rts,
	.shutdown = ch343_port_shutdown,
	.activate = ch343_port_activate,
	.destruct = ch343_port_destruct,
};

static int ch343_proc_show(struct seq_file *m, void *v)
{
	struct ch343 *ch343;
	int i;
	char tmp[40];

	seq_puts(m, "ch343serinfo:1.0 driver:1.8\n");
	for (i = 0; i < CH343_TTY_MINORS; ++i) {
		ch343 = ch343_get_by_index(i);
		if (!ch343)
			continue;
		mutex_lock(&ch343->proc_mutex);
		seq_printf(m, "%d:", i);
		seq_printf(m, " module:%s", "ch343");
		seq_printf(m, " name:\"%s\"", "usb_ch343");
		seq_printf(m, " vendor:%04x product:%04x",
			   le16_to_cpu(ch343->idVendor),
			   le16_to_cpu(ch343->idProduct));
		seq_printf(m, " num_ports:%d", ch343->num_ports);
		seq_printf(m, " port:%d", ch343->iface);
		usb_make_path(ch343->dev, tmp, sizeof(tmp));
		seq_printf(m, " path:%s", tmp);
		seq_putc(m, '\n');
		mutex_unlock(&ch343->proc_mutex);
	}
	return 0;
}

static int ch343_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ch343_proc_show, NULL);
}

static const struct file_operations ch343_proc_fops = {
	.owner = THIS_MODULE,
	.open = ch343_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void ch343_write_buffers_free(struct ch343 *ch343)
{
	int i;
	struct ch343_wb *wb;
	struct usb_device *usb_dev = interface_to_usbdev(ch343->control);

	for (wb = &ch343->wb[0], i = 0; i < CH343_NW; i++, wb++)
		usb_free_coherent(usb_dev, ch343->writesize, wb->buf,
				  wb->dmah);
}

static void ch343_read_buffers_free(struct ch343 *ch343)
{
	struct usb_device *usb_dev = interface_to_usbdev(ch343->control);
	int i;

	for (i = 0; i < ch343->rx_buflimit; i++)
		usb_free_coherent(usb_dev, ch343->readsize,
				  ch343->read_buffers[i].base,
				  ch343->read_buffers[i].dma);
}

static int ch343_write_buffers_alloc(struct ch343 *ch343)
{
	int i;
	struct ch343_wb *wb;

	for (wb = &ch343->wb[0], i = 0; i < CH343_NW; i++, wb++) {
		wb->buf = usb_alloc_coherent(ch343->dev, ch343->writesize,
					     GFP_KERNEL, &wb->dmah);
		if (!wb->buf) {
			while (i != 0) {
				--i;
				--wb;
				usb_free_coherent(ch343->dev,
						  ch343->writesize,
						  wb->buf, wb->dmah);
			}
			return -ENOMEM;
		}
	}
	return 0;
}

static int ch343_open(struct inode *inode, struct file *file)
{
	struct ch343 *ch343;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&ch343_driver, subminor);
	if (!interface) {
		retval = -ENODEV;
		goto exit;
	}

	ch343 = usb_get_intfdata(interface);
	if (!ch343) {
		retval = -ENODEV;
		goto exit;
	}

	file->private_data = ch343;
exit:
	return retval;
}

static int ch343_release(struct inode *inode, struct file *file)
{
	struct ch343 *ch343 = file->private_data;

	if (ch343 == NULL || ch343->io_id != IOID)
		return -ENODEV;

	mutex_lock(&ch343->mutex);

	if (ch343->disconnected) {
		mutex_unlock(&ch343->mutex);
		return -ENODEV;
	}

	mutex_unlock(&ch343->mutex);
	return 0;
}

static long ch343_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct ch343 *ch343 = file->private_data;
	int rv = 0;
	u8 *buffer;
	unsigned long arg1, arg2, arg3, arg4, arg5, arg6;
	u32 __user *argval = (u32 __user *)arg;

	if (ch343 == NULL || ch343->io_id != IOID)
		return -ENODEV;

	mutex_lock(&ch343->mutex);

	if (ch343->disconnected) {
		rv = -ENODEV;
		goto out_unfree;
	}

	buffer = kmalloc(512, GFP_KERNEL);
	if (!buffer) {
		rv = -ENOMEM;
		goto out_unfree;
	}

	switch (cmd) {
	case IOCTL_CMD_GETCHIPTYPE:
		if (put_user(ch343->chiptype, argval)) {
			rv = -EFAULT;
			goto out;
		}
		break;
	case IOCTL_CMD_CTRLIN:
		get_user(arg1, (u8 __user *)arg);
		get_user(arg2, ((u8 __user *)arg + 1));
		get_user(arg3, (u16 __user *)((u8 *)arg + 2));
		get_user(arg4, (u16 __user *)((u8 *)arg + 4));
		get_user(arg5, (u16 __user *)((u8 *)arg + 6));
		arg6 = (unsigned long)((u8 __user *)arg + 8);
		rv = ch343_control_msg_in(ch343, (u8)arg1, (u8)arg2,
					  (u16)arg3, (u16)arg4,
					  (u8 __user *)arg6, (u16)arg5);
		break;
	case IOCTL_CMD_CTRLOUT:
		get_user(arg1, (u8 __user *)arg);
		get_user(arg2, ((u8 __user *)arg + 1));
		get_user(arg3, (u16 __user *)((u8 *)arg + 2));
		get_user(arg4, (u16 __user *)((u8 *)arg + 4));
		get_user(arg5, (u16 __user *)((u8 *)arg + 6));
		arg6 = (unsigned long)((u8 __user *)arg + 8);
		rv = ch343_control_msg_out(ch343, (u8)arg1, (u8)arg2,
					   (u16)arg3, (u16)arg4,
					   (u8 __user *)arg6, (u16)arg5);
		if (rv != (u16)arg5) {
			rv = -EINVAL;
			goto out;
		}
		break;
	default:
		rv = -ENOIOCTLCMD;
		break;
	}

out:
	kfree(buffer);
out_unfree:
	mutex_unlock(&ch343->mutex);

	return rv;
}

#ifdef CONFIG_COMPAT
static long ch343_compat_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	return ch343_ioctl(file, cmd, arg);
}
#endif

static const struct file_operations ch343_fops = {
	.owner = THIS_MODULE,
	.open = ch343_open,
	.unlocked_ioctl = ch343_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ch343_compat_ioctl,
#endif
	.release = ch343_release,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver ch343_class = {
	.name = "ch343_iodev%d",
	.fops = &ch343_fops,
	.minor_base = USB_MINOR_BASE,
};

/*
 * USB probe and disconnect routines.
 */
static int ch343_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_cdc_union_desc *union_header = NULL;
	unsigned char *buffer = intf->altsetting->extra;
	int buflen = intf->altsetting->extralen;
	struct usb_interface *control_interface;
	struct usb_interface *data_interface;
	struct usb_endpoint_descriptor *epctrl = NULL;
	struct usb_endpoint_descriptor *epread = NULL;
	struct usb_endpoint_descriptor *epwrite = NULL;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ch343 *ch343;
	int minor;
	int ctrlsize, readsize;
	u8 *buf;
	unsigned long quirks;
	int num_rx_buf = CH343_NR;
	int i;
	unsigned int elength = 0;
	struct device *tty_dev;
	int rv = -ENOMEM;

	quirks = (unsigned long)id->driver_info;
	if ((!buffer) || (buflen == 0)) {
		dev_err(&intf->dev, "Weird descriptor references\n");
		return -EINVAL;
	}

	while (buflen > 0) {
		elength = buffer[0];
		if (!elength) {
			dev_err(&intf->dev, "skipping garbage byte\n");
			elength = 1;
			goto next_desc;
		}
		if (buffer[1] != USB_DT_CS_INTERFACE) {
			dev_err(&intf->dev, "skipping garbage\n");
			goto next_desc;
		}

		switch (buffer[2]) {
		case USB_CDC_UNION_TYPE:
			if (elength < sizeof(struct usb_cdc_union_desc))
				goto next_desc;
			if (union_header) {
				dev_err(&intf->dev,
					"More than one "
					"union descriptor, skipping ...\n");
				goto next_desc;
			}
			union_header = (struct usb_cdc_union_desc *)buffer;
			break;
		default:
			/*
			 * there are LOTS more CDC descriptors that
			 * could legitimately be found here.
			 */
			break;
		}
next_desc:
		buflen -= elength;
		buffer += elength;
	}

	if (!union_header) {
		dev_err(&intf->dev, "Weird descriptor references\n");
		return -EINVAL;
	}

	control_interface =
		usb_ifnum_to_if(usb_dev, union_header->bMasterInterface0);
	data_interface =
		usb_ifnum_to_if(usb_dev, union_header->bSlaveInterface0);

	if (intf != control_interface)
		return -ENODEV;

	if (usb_interface_claimed(data_interface)) {
		dev_err(&intf->dev,
			"The data interface isn't available\n");
		return -EBUSY;
	}

	if (data_interface->cur_altsetting->desc.bNumEndpoints < 2 ||
	    control_interface->cur_altsetting->desc.bNumEndpoints == 0)
		return -EINVAL;

	epctrl = &control_interface->cur_altsetting->endpoint[0].desc;
	epwrite = &data_interface->cur_altsetting->endpoint[0].desc;
	epread = &data_interface->cur_altsetting->endpoint[1].desc;

	if (!usb_endpoint_dir_in(epread))
		swap(epread, epwrite);

	ch343 = kzalloc(sizeof(struct ch343), GFP_KERNEL);
	if (!ch343)
		return -ENOMEM;

	ch343->idVendor = le16_to_cpu(id->idVendor);
	ch343->idProduct = le16_to_cpu(id->idProduct);
	ch343->iface =
		control_interface->cur_altsetting->desc.bInterfaceNumber /
		2;

	usb_get_intf(control_interface);

	minor = ch343_alloc_minor(ch343);
	if (minor < 0) {
		dev_err(&intf->dev, "no more free ch343 devices\n");
		ch343->minor = CH343_MINOR_INVALID;
		kfree(ch343);
		return minor;
	}

	ctrlsize = usb_endpoint_maxp(epctrl);
	readsize = usb_endpoint_maxp(epread);
	ch343->writesize = usb_endpoint_maxp(epwrite) * 20;
	ch343->control = control_interface;
	ch343->data = data_interface;
	ch343->minor = minor;
	ch343->dev = usb_dev;
	ch343->ctrlsize = ctrlsize;
	ch343->readsize = readsize;
	ch343->rx_buflimit = num_rx_buf;

	INIT_WORK(&ch343->work, ch343_softint);
	init_waitqueue_head(&ch343->wioctl);
	init_waitqueue_head(&ch343->sendioctl);
	spin_lock_init(&ch343->write_lock);
	spin_lock_init(&ch343->read_lock);
	mutex_init(&ch343->mutex);
	mutex_init(&ch343->proc_mutex);
	ch343->rx_endpoint =
		usb_rcvbulkpipe(usb_dev, epread->bEndpointAddress);
	tty_port_init(&ch343->port);
	ch343->port.ops = &ch343_port_ops;
	init_usb_anchor(&ch343->delayed);
	ch343->quirks = quirks;

	buf = usb_alloc_coherent(usb_dev, ctrlsize, GFP_KERNEL,
				 &ch343->ctrl_dma);
	if (!buf)
		goto err_put_port;
	ch343->ctrl_buffer = buf;

	if (ch343_write_buffers_alloc(ch343) < 0)
		goto err_free_ctrl_buffer;

	ch343->ctrlurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ch343->ctrlurb)
		goto err_free_write_buffers;

	for (i = 0; i < num_rx_buf; i++) {
		struct ch343_rb *rb = &(ch343->read_buffers[i]);
		struct urb *urb;

		rb->base = usb_alloc_coherent(ch343->dev, readsize,
					      GFP_KERNEL, &rb->dma);
		if (!rb->base)
			goto err_free_read_urbs;
		rb->index = i;
		rb->instance = ch343;

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			goto err_free_read_urbs;

		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_dma = rb->dma;
		usb_fill_bulk_urb(urb, ch343->dev, ch343->rx_endpoint,
				  rb->base, ch343->readsize,
				  ch343_read_bulk_callback, rb);

		ch343->read_urbs[i] = urb;
		__set_bit(i, &ch343->read_urbs_free);
	}
	for (i = 0; i < CH343_NW; i++) {
		struct ch343_wb *snd = &(ch343->wb[i]);

		snd->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (snd->urb == NULL)
			goto err_free_write_urbs;

		usb_fill_bulk_urb(
			snd->urb, usb_dev,
			usb_sndbulkpipe(usb_dev,
					epwrite->bEndpointAddress),
			NULL, ch343->writesize, ch343_write_bulk, snd);
		snd->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		snd->instance = ch343;
	}

	usb_set_intfdata(intf, ch343);

	usb_fill_int_urb(ch343->ctrlurb, usb_dev,
			 usb_rcvintpipe(usb_dev, epctrl->bEndpointAddress),
			 ch343->ctrl_buffer, ctrlsize, ch343_ctrl_irq,
			 ch343,
			 epctrl->bInterval ? epctrl->bInterval : 16);
	ch343->ctrlurb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	ch343->ctrlurb->transfer_dma = ch343->ctrl_dma;

	dev_info(&intf->dev, "ttyCH343USB%d: usb to uart device\n", minor);

	rv = ch343_configure(ch343);
	if (rv)
		goto err_free_write_urbs;

	if (ch343->iosupport && (ch343->iface == 0) &&
	    (ch343->io_intf == NULL)) {
		/* register the device now, as it is ready */
		rv = usb_register_dev(intf, &ch343_class);
		if (rv) {
			/* error when registering this driver */
			dev_err(&intf->dev,
				"Not able to get a minor for this device.\n");
		} else {
			ch343->io_id = IOID;
			ch343->io_intf = intf;
			dev_info(
				&intf->dev,
				"USB to GPIO device now attached to ch343_iodev%d\n",
				intf->minor);
		}
	}

	usb_driver_claim_interface(&ch343_driver, data_interface, ch343);
	usb_set_intfdata(data_interface, ch343);

	ch343->line.dwDTERate = 9600;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0))
	tty_dev = tty_port_register_device(&ch343->port, ch343_tty_driver,
					   minor, &control_interface->dev);
	if (IS_ERR(tty_dev)) {
		rv = PTR_ERR(tty_dev);
		goto err_release_data_interface;
	}
#else
	tty_register_device(ch343_tty_driver, minor,
			    &control_interface->dev);
#endif

	return 0;

err_release_data_interface:
	usb_set_intfdata(data_interface, NULL);
	usb_driver_release_interface(&ch343_driver, data_interface);
err_free_write_urbs:
	for (i = 0; i < CH343_NW; i++)
		usb_free_urb(ch343->wb[i].urb);
err_free_read_urbs:
	for (i = 0; i < num_rx_buf; i++)
		usb_free_urb(ch343->read_urbs[i]);
	ch343_read_buffers_free(ch343);
	usb_free_urb(ch343->ctrlurb);
err_free_write_buffers:
	ch343_write_buffers_free(ch343);
err_free_ctrl_buffer:
	usb_free_coherent(usb_dev, ctrlsize, ch343->ctrl_buffer,
			  ch343->ctrl_dma);
err_put_port:
	tty_port_put(&ch343->port);

	return rv;
}

static void stop_data_traffic(struct ch343 *ch343)
{
	struct urb *urb;
	struct ch343_wb *wb;
	int i;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0))

	usb_autopm_get_interface_no_resume(ch343->control);
	ch343->control->needs_remote_wakeup = 0;
	usb_autopm_put_interface(ch343->control);

	for (;;) {
		urb = usb_get_from_anchor(&ch343->delayed);
		if (!urb)
			break;
		wb = urb->context;
		wb->use = 0;
		usb_autopm_put_interface_async(ch343->control);
	}

	usb_kill_urb(ch343->ctrlurb);
	for (i = 0; i < CH343_NW; i++)
		usb_kill_urb(ch343->wb[i].urb);
	for (i = 0; i < ch343->rx_buflimit; i++)
		usb_kill_urb(ch343->read_urbs[i]);

#else
	mutex_lock(&ch343->mutex);
	if (!ch343->disconnected) {
		usb_autopm_get_interface(ch343->control);
		ch343_set_control(ch343, ch343->ctrlout = 0);

		usb_kill_urb(ch343->ctrlurb);
		for (i = 0; i < CH343_NW; i++)
			usb_kill_urb(ch343->wb[i].urb);
		for (i = 0; i < ch343->rx_buflimit; i++)
			usb_kill_urb(ch343->read_urbs[i]);
		ch343->control->needs_remote_wakeup = 0;

		usb_autopm_put_interface(ch343->control);
	}
	mutex_unlock(&ch343->mutex);
#endif
}

static void ch343_disconnect(struct usb_interface *intf)
{
	struct ch343 *ch343 = usb_get_intfdata(intf);
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct tty_struct *tty;
	int i;

	/* sibling interface is already cleaning up */
	if (!ch343)
		return;

	/* give back minor */
	if (ch343->iosupport && (ch343->iface == 0) &&
	    (ch343->io_intf != NULL)) {
		usb_deregister_dev(ch343->io_intf, &ch343_class);
		ch343->io_intf = NULL;
	}

	mutex_lock(&ch343->mutex);
	ch343->disconnected = true;
	wake_up_all(&ch343->wioctl);
	wake_up_all(&ch343->sendioctl);
	usb_set_intfdata(ch343->control, NULL);
	usb_set_intfdata(ch343->data, NULL);
	mutex_unlock(&ch343->mutex);

	tty = tty_port_tty_get(&ch343->port);
	if (tty) {
		tty_vhangup(tty);
		tty_kref_put(tty);
	}

	stop_data_traffic(ch343);
	tty_unregister_device(ch343_tty_driver, ch343->minor);

	usb_free_urb(ch343->ctrlurb);
	for (i = 0; i < CH343_NW; i++)
		usb_free_urb(ch343->wb[i].urb);
	for (i = 0; i < ch343->rx_buflimit; i++)
		usb_free_urb(ch343->read_urbs[i]);
	ch343_write_buffers_free(ch343);
	usb_free_coherent(usb_dev, ch343->ctrlsize, ch343->ctrl_buffer,
			  ch343->ctrl_dma);
	ch343_read_buffers_free(ch343);

	usb_driver_release_interface(
		&ch343_driver,
		intf == ch343->control ? ch343->data : ch343->control);
	tty_port_put(&ch343->port);
	dev_info(&intf->dev, "%s\n", "ch343 usb device disconnect.");
}

#ifdef CONFIG_PM
static int ch343_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct ch343 *ch343 = usb_get_intfdata(intf);
	int cnt;

	spin_lock_irq(&ch343->write_lock);
	if (PMSG_IS_AUTO(message)) {
		if (ch343->transmitting) {
			spin_unlock_irq(&ch343->write_lock);
			return -EBUSY;
		}
	}
	cnt = ch343->susp_count++;
	spin_unlock_irq(&ch343->write_lock);
	if (cnt)
		return 0;
	stop_data_traffic(ch343);

	return 0;
}

static int ch343_resume(struct usb_interface *intf)
{
	struct ch343 *ch343 = usb_get_intfdata(intf);
	struct urb *urb;
	int rv = 0;

	spin_lock_irq(&ch343->write_lock);
	if (--ch343->susp_count)
		goto out;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0))
	if (tty_port_initialized(&ch343->port)) {
#else
	if (test_bit(ASYNCB_INITIALIZED, &ch343->port.flags)) {
#endif
		rv = usb_submit_urb(ch343->ctrlurb, GFP_ATOMIC);
		for (;;) {
			urb = usb_get_from_anchor(&ch343->delayed);
			if (!urb)
				break;

			ch343_start_wb(ch343, urb->context);
		}
		if (rv < 0)
			goto out;
		rv = ch343_submit_read_urbs(ch343, GFP_ATOMIC);
	}
out:
	spin_unlock_irq(&ch343->write_lock);
	return rv;
}

static int ch343_reset_resume(struct usb_interface *intf)
{
	struct ch343 *ch343 = usb_get_intfdata(intf);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0))
	if (tty_port_initialized(&ch343->port))
#else
	if (test_bit(ASYNCB_INITIALIZED, &ch343->port.flags))
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0))
		struct tty_struct *tty = tty_port_tty_get(&ch343->port);
	tty_hangup(tty);
#else
		tty_port_tty_hangup(&ch343->port, false);
#endif
	return ch343_resume(intf);
}
#endif /* CONFIG_PM */

static const struct usb_device_id ch343_ids[] = {
	/* ch342 chip */
	{ USB_DEVICE(0x1a86, 0x55d2) },
	/* ch343 chip */
	{ USB_DEVICE(0x1a86, 0x55d3) },
	/* ch344 chip */
	{ USB_DEVICE(0x1a86, 0x55d5) },
	/* ch9143 chip */
	{ USB_DEVICE(0x1a86, 0x55d6) },
	/* ch347t chip mode0*/
	{ USB_DEVICE(0x1a86, 0x55da) },
	/* ch347t chip mode1*/
	{ USB_DEVICE_INTERFACE_NUMBER(0x1a86, 0x55db, 0x00) },
	/* ch347t chip mode3*/
	{ USB_DEVICE_INTERFACE_NUMBER(0x1a86, 0x55dd, 0x00) },
	/* ch347f chip uart0*/
	{ USB_DEVICE_INTERFACE_NUMBER(0x1a86, 0x55de, 0x00) },
	/* ch347f chip uart1*/
	{ USB_DEVICE_INTERFACE_NUMBER(0x1a86, 0x55de, 0x02) },
	/* ch339w chip */
	{ USB_DEVICE_INTERFACE_NUMBER(0x1a86, 0x55e7, 0x00) },
	/* ch9101 chip */
	{ USB_DEVICE(0x1a86, 0x55d8) },
	/* ch9102 chip */
	{ USB_DEVICE(0x1a86, 0x55d4) },
	/* ch9103 chip */
	{ USB_DEVICE(0x1a86, 0x55d7) },
	/* ch9104 chip */
	{ USB_DEVICE(0x1a86, 0x55df) },
	/* ch9111l chip mode0*/
	{ USB_DEVICE(0x1a86, 0x55e9) },
	/* ch9111l chip mode1*/
	{ USB_DEVICE(0x1a86, 0x55ea) },
	/* ch9114 chip */
	{ USB_DEVICE(0x1a86, 0x55e8) },
	/* ch346c chip mode0/1*/
	{ USB_DEVICE_INTERFACE_NUMBER(0x1a86, 0x55eb, 0x00) },
	/* ch346c chip mode2*/
	{ USB_DEVICE(0x1a86, 0x55ec) },
	{}
};

MODULE_DEVICE_TABLE(usb, ch343_ids);

static struct usb_driver ch343_driver = {
	.name = "usb_ch343",
	.probe = ch343_probe,
	.disconnect = ch343_disconnect,
#ifdef CONFIG_PM
	.suspend = ch343_suspend,
	.resume = ch343_resume,
	.reset_resume = ch343_reset_resume,
#endif
	.id_table = ch343_ids,
#ifdef CONFIG_PM
	.supports_autosuspend = 1,
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	.disable_hub_initiated_lpm = 1,
#endif
};

/*
 * TTY driver structures.
 */
static const struct tty_operations ch343_ops = {
	.install = ch343_tty_install,
	.open = ch343_tty_open,
	.close = ch343_tty_close,
	.cleanup = ch343_tty_cleanup,
	.hangup = ch343_tty_hangup,
	.write = ch343_tty_write,
	.write_room = ch343_tty_write_room,
	.ioctl = ch343_tty_ioctl,
	.chars_in_buffer = ch343_tty_chars_in_buffer,
	.break_ctl = ch343_tty_break_ctl,
	.set_termios = ch343_tty_set_termios,
	.tiocmget = ch343_tty_tiocmget,
	.tiocmset = ch343_tty_tiocmset,
	.get_icount = ch343_get_icount,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0))
	.proc_fops = &ch343_proc_fops,
#else
	.proc_show = ch343_proc_show,
#endif
};

static int __init ch343_init(void)
{
	int retval;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
	ch343_tty_driver = tty_alloc_driver(
		CH343_TTY_MINORS,
		TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(ch343_tty_driver))
		return PTR_ERR(ch343_tty_driver);
#else
	ch343_tty_driver = alloc_tty_driver(CH343_TTY_MINORS);
	if (!ch343_tty_driver)
		return -ENOMEM;
#endif
	ch343_tty_driver->driver_name = "usbch343",
	ch343_tty_driver->name = "ttyCH343USB",
	ch343_tty_driver->major = CH343_TTY_MAJOR,
	ch343_tty_driver->minor_start = 0,
	ch343_tty_driver->type = TTY_DRIVER_TYPE_SERIAL,
	ch343_tty_driver->subtype = SERIAL_TYPE_NORMAL,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0))
	ch343_tty_driver->flags = TTY_DRIVER_REAL_RAW |
				  TTY_DRIVER_DYNAMIC_DEV;
#endif
	ch343_tty_driver->init_termios = tty_std_termios;
	ch343_tty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD |
						 HUPCL | CLOCAL;
	ch343_tty_driver->init_termios.c_lflag &=
		~(ECHO | ECHONL | ICANON | ISIG);
	ch343_tty_driver->init_termios.c_oflag &= ~OPOST;

	tty_set_operations(ch343_tty_driver, &ch343_ops);

	retval = tty_register_driver(ch343_tty_driver);
	if (retval) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
		tty_driver_kref_put(ch343_tty_driver);
#else
		put_tty_driver(ch343_tty_driver);
#endif
		return retval;
	}

	retval = usb_register(&ch343_driver);
	if (retval) {
		tty_unregister_driver(ch343_tty_driver);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
		tty_driver_kref_put(ch343_tty_driver);
#else
		put_tty_driver(ch343_tty_driver);
#endif
		return retval;
	}

	printk(KERN_INFO KBUILD_MODNAME ": " DRIVER_DESC "\n");
	printk(KERN_INFO KBUILD_MODNAME ": " VERSION_DESC "\n");

	return 0;
}

static void __exit ch343_exit(void)
{
	usb_deregister(&ch343_driver);
	tty_unregister_driver(ch343_tty_driver);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
	tty_driver_kref_put(ch343_tty_driver);
#else
	put_tty_driver(ch343_tty_driver);
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	idr_destroy(&ch343_minors);
#endif
	printk(KERN_INFO KBUILD_MODNAME ": "
					"ch343 driver exit.\n");
}

fs_initcall(ch343_init);
module_exit(ch343_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(VERSION_DESC);
MODULE_LICENSE("GPL");
MODULE_ALIAS_CHARDEV_MAJOR(CH343_TTY_MAJOR);
