/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/memremap.h>
#include <linux/rculist.h>
#include <linux/export.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/pfn_t.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/mm.h>
#include "nfit_test.h"

static LIST_HEAD(iomap_head);

static struct iomap_ops {
	nfit_test_lookup_fn nfit_test_lookup;
	struct list_head list;
} iomap_ops = {
	.list = LIST_HEAD_INIT(iomap_ops.list),
};

void nfit_test_setup(nfit_test_lookup_fn lookup)
{
	iomap_ops.nfit_test_lookup = lookup;
	list_add_rcu(&iomap_ops.list, &iomap_head);
}
EXPORT_SYMBOL(nfit_test_setup);

void nfit_test_teardown(void)
{
	list_del_rcu(&iomap_ops.list);
	synchronize_rcu();
}
EXPORT_SYMBOL(nfit_test_teardown);

static struct nfit_test_resource *__get_nfit_res(resource_size_t resource)
{
	struct iomap_ops *ops;

	ops = list_first_or_null_rcu(&iomap_head, typeof(*ops), list);
	if (ops)
		return ops->nfit_test_lookup(resource);
	return NULL;
}

struct nfit_test_resource *get_nfit_res(resource_size_t resource)
{
	struct nfit_test_resource *res;

	rcu_read_lock();
	res = __get_nfit_res(resource);
	rcu_read_unlock();

	return res;
}
EXPORT_SYMBOL(get_nfit_res);

void __iomem *__nfit_test_ioremap(resource_size_t offset, unsigned long size,
		void __iomem *(*fallback_fn)(resource_size_t, unsigned long))
{
	struct nfit_test_resource *nfit_res = get_nfit_res(offset);

	if (nfit_res)
		return (void __iomem *) nfit_res->buf + offset
			- nfit_res->res->start;
	return fallback_fn(offset, size);
}

void __iomem *__wrap_devm_ioremap_nocache(struct device *dev,
		resource_size_t offset, unsigned long size)
{
	struct nfit_test_resource *nfit_res = get_nfit_res(offset);

	if (nfit_res)
		return (void __iomem *) nfit_res->buf + offset
			- nfit_res->res->start;
	return devm_ioremap_nocache(dev, offset, size);
}
EXPORT_SYMBOL(__wrap_devm_ioremap_nocache);

void *__wrap_devm_memremap(struct device *dev, resource_size_t offset,
		size_t size, unsigned long flags)
{
	struct nfit_test_resource *nfit_res = get_nfit_res(offset);

	if (nfit_res)
		return nfit_res->buf + offset - nfit_res->res->start;
	return devm_memremap(dev, offset, size, flags);
}
EXPORT_SYMBOL(__wrap_devm_memremap);

void *__wrap_devm_memremap_pages(struct device *dev, struct resource *res,
		struct percpu_ref *ref, struct vmem_altmap *altmap)
{
	resource_size_t offset = res->start;
	struct nfit_test_resource *nfit_res = get_nfit_res(offset);

	if (nfit_res)
		return nfit_res->buf + offset - nfit_res->res->start;
	return devm_memremap_pages(dev, res, ref, altmap);
}
EXPORT_SYMBOL(__wrap_devm_memremap_pages);

pfn_t __wrap_phys_to_pfn_t(phys_addr_t addr, unsigned long flags)
{
	struct nfit_test_resource *nfit_res = get_nfit_res(addr);

	if (nfit_res)
		flags &= ~PFN_MAP;
        return phys_to_pfn_t(addr, flags);
}
EXPORT_SYMBOL(__wrap_phys_to_pfn_t);

void *__wrap_memremap(resource_size_t offset, size_t size,
		unsigned long flags)
{
	struct nfit_test_resource *nfit_res = get_nfit_res(offset);

	if (nfit_res)
		return nfit_res->buf + offset - nfit_res->res->start;
	return memremap(offset, size, flags);
}
EXPORT_SYMBOL(__wrap_memremap);

void __wrap_devm_memunmap(struct device *dev, void *addr)
{
	struct nfit_test_resource *nfit_res = get_nfit_res((long) addr);

	if (nfit_res)
		return;
	return devm_memunmap(dev, addr);
}
EXPORT_SYMBOL(__wrap_devm_memunmap);

void __iomem *__wrap_ioremap_nocache(resource_size_t offset, unsigned long size)
{
	return __nfit_test_ioremap(offset, size, ioremap_nocache);
}
EXPORT_SYMBOL(__wrap_ioremap_nocache);

void __iomem *__wrap_ioremap_wc(resource_size_t offset, unsigned long size)
{
	return __nfit_test_ioremap(offset, size, ioremap_wc);
}
EXPORT_SYMBOL(__wrap_ioremap_wc);

void __wrap_iounmap(volatile void __iomem *addr)
{
	struct nfit_test_resource *nfit_res = get_nfit_res((long) addr);
	if (nfit_res)
		return;
	return iounmap(addr);
}
EXPORT_SYMBOL(__wrap_iounmap);

void __wrap_memunmap(void *addr)
{
	struct nfit_test_resource *nfit_res = get_nfit_res((long) addr);

	if (nfit_res)
		return;
	return memunmap(addr);
}
EXPORT_SYMBOL(__wrap_memunmap);

static struct resource *nfit_test_request_region(struct device *dev,
		struct resource *parent, resource_size_t start,
		resource_size_t n, const char *name, int flags)
{
	struct nfit_test_resource *nfit_res;

	if (parent == &iomem_resource) {
		nfit_res = get_nfit_res(start);
		if (nfit_res) {
			struct resource *res = nfit_res->res + 1;

			if (start + n > nfit_res->res->start
					+ resource_size(nfit_res->res)) {
				pr_debug("%s: start: %llx n: %llx overflow: %pr\n",
						__func__, start, n,
						nfit_res->res);
				return NULL;
			}

			res->start = start;
			res->end = start + n - 1;
			res->name = name;
			res->flags = resource_type(parent);
			res->flags |= IORESOURCE_BUSY | flags;
			pr_debug("%s: %pr\n", __func__, res);
			return res;
		}
	}
	if (dev)
		return __devm_request_region(dev, parent, start, n, name);
	return __request_region(parent, start, n, name, flags);
}

struct resource *__wrap___request_region(struct resource *parent,
		resource_size_t start, resource_size_t n, const char *name,
		int flags)
{
	return nfit_test_request_region(NULL, parent, start, n, name, flags);
}
EXPORT_SYMBOL(__wrap___request_region);

int __wrap_insert_resource(struct resource *parent, struct resource *res)
{
	if (get_nfit_res(res->start))
		return 0;
	return insert_resource(parent, res);
}
EXPORT_SYMBOL(__wrap_insert_resource);

int __wrap_remove_resource(struct resource *res)
{
	if (get_nfit_res(res->start))
		return 0;
	return remove_resource(res);
}
EXPORT_SYMBOL(__wrap_remove_resource);

struct resource *__wrap___devm_request_region(struct device *dev,
		struct resource *parent, resource_size_t start,
		resource_size_t n, const char *name)
{
	if (!dev)
		return NULL;
	return nfit_test_request_region(dev, parent, start, n, name, 0);
}
EXPORT_SYMBOL(__wrap___devm_request_region);

static bool nfit_test_release_region(struct resource *parent,
		resource_size_t start, resource_size_t n)
{
	if (parent == &iomem_resource) {
		struct nfit_test_resource *nfit_res = get_nfit_res(start);
		if (nfit_res) {
			struct resource *res = nfit_res->res + 1;

			if (start != res->start || resource_size(res) != n)
				pr_info("%s: start: %llx n: %llx mismatch: %pr\n",
						__func__, start, n, res);
			else
				memset(res, 0, sizeof(*res));
			return true;
		}
	}
	return false;
}

void __wrap___release_region(struct resource *parent, resource_size_t start,
		resource_size_t n)
{
	if (!nfit_test_release_region(parent, start, n))
		__release_region(parent, start, n);
}
EXPORT_SYMBOL(__wrap___release_region);

void __wrap___devm_release_region(struct device *dev, struct resource *parent,
		resource_size_t start, resource_size_t n)
{
	if (!nfit_test_release_region(parent, start, n))
		__devm_release_region(dev, parent, start, n);
}
EXPORT_SYMBOL(__wrap___devm_release_region);

acpi_status __wrap_acpi_evaluate_object(acpi_handle handle, acpi_string path,
		struct acpi_object_list *p, struct acpi_buffer *buf)
{
	struct nfit_test_resource *nfit_res = get_nfit_res((long) handle);
	union acpi_object **obj;

	if (!nfit_res || strcmp(path, "_FIT") || !buf)
		return acpi_evaluate_object(handle, path, p, buf);

	obj = nfit_res->buf;
	buf->length = sizeof(union acpi_object);
	buf->pointer = *obj;
	return AE_OK;
}
EXPORT_SYMBOL(__wrap_acpi_evaluate_object);

MODULE_LICENSE("GPL v2");
