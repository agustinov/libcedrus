/*
 * Copyright (c) 2013-2016 Jens Kuske <jenskuske@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "cedrus.h"
#include "cedrus_mem.h"
#include "cedrus_regs.h"
#include "kernel-headers/cedardev_api.h"

#define DEVICE "/dev/cedar_dev"
#define EXPORT __attribute__ ((visibility ("default")))

static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;
static int open_count;

static struct cedrus
{
	int fd;
	void *regs;
	int version;
	int ioctl_offset;
	struct cedrus_allocator *allocator;
	pthread_mutex_t device_lock;
} ve = { .fd = -1, .device_lock = PTHREAD_MUTEX_INITIALIZER };

EXPORT cedrus_t *cedrus_open(void)
{
	pthread_mutex_lock(&open_lock);
	if (ve.fd != -1) {
		open_count++;
		pthread_mutex_unlock(&open_lock);
		return &ve;
	}

	struct cedarv_env_infomation info;

	ve.fd = open(DEVICE, O_RDWR);
	if (ve.fd == -1) {
		pthread_mutex_unlock(&open_lock);
		return NULL;
	}

	if (ioctl(ve.fd, IOCTL_GET_ENV_INFO, (void *)(&info)) == -1)
		goto close;

	ve.regs = mmap(NULL, 0x800, PROT_READ | PROT_WRITE, MAP_SHARED, ve.fd, info.address_macc);
	if (ve.regs == MAP_FAILED)
		goto close;

#ifdef USE_UMP
	ve.allocator = cedrus_allocator_ump_new();
	if (!ve.allocator)
#endif
	{
		ve.allocator = cedrus_allocator_ve_new(ve.fd, &info);
		if (!ve.allocator)
		{
			ve.allocator = cedrus_allocator_ion_new();
			if (!ve.allocator)
				goto unmap;
		}
	}

	ioctl(ve.fd, IOCTL_ENGINE_REQ, 0);

	ve.version = readl(ve.regs + VE_VERSION) >> 16;

	if (ve.version >= 0x1639)
		ve.ioctl_offset = 1;

	ioctl(ve.fd, IOCTL_ENABLE_VE + ve.ioctl_offset, 0);
	ioctl(ve.fd, IOCTL_SET_VE_FREQ + ve.ioctl_offset, 320);
	ioctl(ve.fd, IOCTL_RESET_VE + ve.ioctl_offset, 0);

	writel(0x00130000 | VE_CTRL_ENGINE_RESET, ve.regs + VE_CTRL);

	//printf("[libcedrus SUNXI] VE version 0x%04x opened\n", ve.version);
	open_count++;
	pthread_mutex_unlock(&open_lock);
	return &ve;

unmap:
	munmap(ve.regs, 0x800);

close:
	close(ve.fd);
	ve.fd = -1;
	pthread_mutex_unlock(&open_lock);
	return NULL;
}

EXPORT void cedrus_close(struct cedrus *dev)
{
	pthread_mutex_lock(&open_lock);
	if (dev->fd == -1) {
		pthread_mutex_unlock(&open_lock);
		return;
	}

	if (--open_count) {
		pthread_mutex_unlock(&open_lock);
		return;
	}

	ioctl(dev->fd, IOCTL_DISABLE_VE + dev->ioctl_offset, 0);
	ioctl(dev->fd, IOCTL_ENGINE_REL, 0);

	munmap(dev->regs, 0x800);
	dev->regs = NULL;

	dev->allocator->free(dev->allocator);

	close(dev->fd);
	dev->fd = -1;
	pthread_mutex_unlock(&open_lock);
}

EXPORT int cedrus_get_ve_version(struct cedrus *dev)
{
	if (!dev)
		return 0x0;

	return dev->version;
}

EXPORT int cedrus_ve_wait(struct cedrus *dev, int timeout)
{
	unsigned int engine_wait = IOCTL_WAIT_VE_DE;
	unsigned int reg = 0;
	if (!dev)
		return -1;

	if (((reg = readl(dev->regs + VE_CTRL)) & VE_CTRL_ENGINE_FIELD) == VE_CTRL_ENGINE_AVC) {
		engine_wait = IOCTL_WAIT_VE_DE + dev->ioctl_offset;
	}

	//printf("ve engine %04X, ve wait %d\n", reg, engine_wait);

	return ioctl(dev->fd, engine_wait, timeout);
}

EXPORT void *cedrus_ve_get(struct cedrus *dev, enum cedrus_engine engine, uint32_t flags)
{
	if (!dev || pthread_mutex_lock(&dev->device_lock))
		return NULL;

	writel(0x00130000 | (engine & VE_CTRL_ENGINE_FIELD) | (flags & ~VE_CTRL_ENGINE_FIELD), dev->regs + VE_CTRL);

	return dev->regs;
}

EXPORT void cedrus_ve_put(struct cedrus *dev)
{
	if (!dev)
		return;

	writel(0x00130000 | VE_CTRL_ENGINE_RESET, dev->regs + VE_CTRL);
	pthread_mutex_unlock(&dev->device_lock);
}

EXPORT struct cedrus_mem *cedrus_mem_alloc(struct cedrus *dev, size_t size)
{
	if (!dev || size == 0)
		return NULL;

	return dev->allocator->mem_alloc(dev->allocator, (size + 4096 - 1) & ~(4096 - 1));
}

EXPORT void cedrus_mem_free(struct cedrus_mem *mem)
{
	if (!mem)
		return;

	ve.allocator->mem_free(ve.allocator, mem);
}

EXPORT void cedrus_mem_flush_cache(struct cedrus_mem *mem)
{
	if (!mem)
		return;

	ve.allocator->mem_flush(ve.allocator, mem);
}

EXPORT void *cedrus_mem_get_pointer(const struct cedrus_mem *mem)
{
	if (!mem)
		return NULL;

	return mem->virt;
}

EXPORT uint32_t cedrus_mem_get_phys_addr(const struct cedrus_mem *mem)
{
	if (!mem)
		return 0x0;

	return mem->phys;
}

uint32_t phys2bus(uint32_t phys)
{
	if (ve.version == 0x1639)
		return phys - 0x20000000;
	else
		return phys - 0x40000000;
}

uint32_t bus2phys(uint32_t bus)
{
	if (ve.version == 0x1639)
		return bus + 0x20000000;
	else
		return bus + 0x40000000;
}

EXPORT uint32_t cedrus_mem_get_bus_addr(const struct cedrus_mem *mem)
{
	if (!mem)
		return 0x0;

	return phys2bus(mem->phys);
}

EXPORT void cedrus_ve_reset(struct cedrus *dev)
{
	if (!dev)
		return;

	writel(0x1, dev->regs + VE_RESET);
	usleep(1000);
	writel(0x0, dev->regs + VE_RESET);
}
