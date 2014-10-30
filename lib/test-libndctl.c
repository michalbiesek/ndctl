/*
 * libndctl: helper library for the nd (nvdimm, nfit-defined, persistent
 *           memory, ...) sub-system.
 *
 * Copyright (c) 2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <syslog.h>
#include <libkmod.h>
#include <uuid/uuid.h>

#include <ccan/array_size/array_size.h>
#include <ndctl/libndctl.h>
#include <linux/ndctl.h>

/*
 * Kernel provider "nfit_test.0" produces an NFIT with the following attributes:
 *
 *                              (a)               (b)           DIMM   BLK-REGION
 *           +-------------------+--------+--------+--------+
 * +------+  |       pm0.0       | blk2.0 | pm1.0  | blk2.1 |    0      region2
 * | imc0 +--+- - - region0- - - +--------+        +--------+
 * +--+---+  |       pm0.0       | blk3.0 | pm1.0  | blk3.1 |    1      region3
 *    |      +-------------------+--------v        v--------+
 * +--+---+                               |                 |
 * | cpu0 |                                     region1
 * +--+---+                               |                 |
 *    |      +----------------------------^        ^--------+
 * +--+---+  |           blk4.0           | pm1.0  | blk4.0 |    2      region4
 * | imc1 +--+----------------------------|        +--------+
 * +------+  |           blk5.0           | pm1.0  | blk5.0 |    3      region5
 *           +----------------------------+--------+--------+
 *
 * *) In this layout we have four dimms and two memory controllers in one
 *    socket.  Each unique interface ("block" or "pmem") to DPA space
 *    is identified by a region device with a dynamically assigned id.
 *
 * *) The first portion of dimm0 and dimm1 are interleaved as REGION0.
 *    A single "pmem" namespace is created in the REGION0-"spa"-range
 *    that spans dimm0 and dimm1 with a user-specified name of "pm0.0".
 *    Some of that interleaved "spa" range is reclaimed as "bdw"
 *    accessed space starting at offset (a) into each dimm.  In that
 *    reclaimed space we create two "bdw" "namespaces" from REGION2 and
 *    REGION3 where "blk2.0" and "blk3.0" are just human readable names
 *    that could be set to any user-desired name in the label.
 *
 * *) In the last portion of dimm0 and dimm1 we have an interleaved
 *    "spa" range, REGION1, that spans those two dimms as well as dimm2
 *    and dimm3.  Some of REGION1 allocated to a "pmem" namespace named
 *    "pm1.0" the rest is reclaimed in 4 "bdw" namespaces (for each
 *    dimm in the interleave set), "blk2.1", "blk3.1", "blk4.0", and
 *    "blk5.0".
 *
 * *) The portion of dimm2 and dimm3 that do not participate in the
 *    REGION1 interleaved "spa" range (i.e. the DPA address below
 *    offset (b) are also included in the "blk4.0" and "blk5.0"
 *    namespaces.  Note, that this example shows that "bdw" namespaces
 *    don't need to be contiguous in DPA-space.
 *
 * Kernel provider "nfit_test.1" produces an NFIT with the following attributes:
 *
 * region2
 * +---------------------+
 * |---------------------|
 * ||       pm2.0       ||
 * |---------------------|
 * +---------------------+
 *
 * *) Describes a simple system-physical-address range with no backing
 *    dimm or interleave description.
 */

static const char *NFIT_TEST_MODULE = "nfit_test";
static const char *NFIT_PROVIDER0 = "nfit_test.0";
static const char *NFIT_PROVIDER1 = "nfit_test.1";

struct dimm {
	unsigned int handle;
	unsigned int phys_id;
};

#define DIMM_HANDLE(n, s, i, c, d) \
	(((n & 0xfff) << 16) | ((s & 0xf) << 12) | ((i & 0xf) << 8) \
	 | ((c & 0xf) << 4) | (d & 0xf))
static struct dimm dimms[] = {
	{ DIMM_HANDLE(0, 0, 0, 0, 0), 0, },
	{ DIMM_HANDLE(0, 0, 0, 0, 1), 1, },
	{ DIMM_HANDLE(0, 0, 1, 0, 0), 2, },
	{ DIMM_HANDLE(0, 0, 1, 0, 1), 3, },
};

struct region {
	unsigned int id;
	unsigned int spa_index;
	unsigned int interleave_ways;
	int enabled;
	char *type;
};

struct btt;
struct namespace {
	unsigned int id;
	char *type;
	char *bdev;
	struct btt *create_btt;
};

struct btt {
	int id;
	int enabled;
	uuid_t uuid;
	char *bdev;
	char *backing_dev;
	int num_sector_sizes;
	unsigned int sector_size;
	unsigned int sector_sizes[4];
};

static struct region regions0[] = {
	{ 0, 1, 2, 0, "pmem" },
	{ 1, 2, 4, 0, "pmem" },
	{ 2, -1, 1, 0, "block" },
	{ 3, -1, 1, 0, "block" },
	{ 4, -1, 1, 0, "block" },
	{ 5, -1, 1, 0, "block" },
};

static struct region regions1[] = {
	{ 6, 1, 0, 1, "pmem" },
};

static struct btt btts0[] = {
	{ 0, 0, { 0, }, "", "", 1, UINT_MAX, { 512, }, },
};

static struct btt btts1[] = {
	{ 1, 0, { 0, }, "", "", 1, UINT_MAX, { 512, }, }
};

static struct btt create_btt1 = {
	.id = 1,
	.enabled = 1,
	.uuid = {  0,  1,  2,  3,  4,  5,  6,  7,
		   8, 9,  10, 11, 12, 13, 14, 15
	},
	.bdev = "btt1",
	.backing_dev = "/dev/pmem0",
	.num_sector_sizes = 1,
	.sector_size = 512,
	.sector_sizes =  { 512, },
};

static struct namespace namespaces1[] = {
	{ 0, "namespace_io", "pmem0", &create_btt1, },
};

static unsigned long commands0 = 1UL << NFIT_CMD_GET_CONFIG_SIZE
		| 1UL << NFIT_CMD_GET_CONFIG_DATA
		| 1UL << NFIT_CMD_SET_CONFIG_DATA;

static struct ndctl_bus *get_bus_by_provider(struct ndctl_ctx *ctx,
		const char *provider)
{
	struct ndctl_bus *bus;

        ndctl_bus_foreach(ctx, bus)
		if (strcmp(provider, ndctl_bus_get_provider(bus)) == 0)
			return bus;

	return NULL;
}

static struct ndctl_dimm *get_dimm_by_handle(struct ndctl_bus *bus, unsigned int handle)
{
	struct ndctl_dimm *dimm;

	ndctl_dimm_foreach(bus, dimm)
		if (ndctl_dimm_get_handle(dimm) == handle)
			return dimm;

	return NULL;
}

static struct ndctl_region *get_region_by_id(struct ndctl_bus *bus,
		unsigned int id)
{
	struct ndctl_region *region;

	ndctl_region_foreach(bus, region)
		if (ndctl_region_get_id(region) == id)
			return region;

	return NULL;
}

static struct ndctl_btt *get_btt_by_id(struct ndctl_bus *bus,
		unsigned int id)
{
	struct ndctl_btt *btt;

	ndctl_btt_foreach(bus, btt)
		if (ndctl_btt_get_id(btt) == id)
			return btt;

	return NULL;
}

static struct ndctl_namespace *get_namespace_by_id(struct ndctl_region *region,
		unsigned int id)
{
	struct ndctl_namespace *ndns;

	ndctl_namespace_foreach(region, ndns)
		if (ndctl_namespace_get_id(ndns) == id)
			return ndns;

	return NULL;
}

static int check_regions(struct ndctl_bus *bus, struct region *regions, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		struct ndctl_region *region;
		char devname[50];

		snprintf(devname, sizeof(devname), "region%d", regions[i].id);
		region = get_region_by_id(bus, regions[i].id);
		if (!region) {
			fprintf(stderr, "%s: failed to find region\n", devname);
			return -ENXIO;
		}
		if (strcmp(ndctl_region_get_type_name(region), regions[i].type) != 0) {
			fprintf(stderr, "%s: expected type: %s got: %s\n",
					devname, regions[i].type,
					ndctl_region_get_type_name(region));
			return -ENXIO;
		}
		if (ndctl_region_get_interleave_ways(region) != regions[i].interleave_ways) {
			fprintf(stderr, "%s: expected interleave_ways: %d got: %d\n",
					devname, regions[i].interleave_ways,
					ndctl_region_get_interleave_ways(region));
			return -ENXIO;
		}
		if (ndctl_region_get_spa_index(region) != regions[i].spa_index) {
			fprintf(stderr, "%s: expected spa_index: %d got: %d\n",
					devname, regions[i].spa_index,
					ndctl_region_get_spa_index(region));
			return -ENXIO;
		}
		if (regions[i].enabled && !ndctl_region_is_enabled(region)) {
			fprintf(stderr, "%s: expected enabled by default\n",
					devname);
			return -ENXIO;
		}
		if (ndctl_region_disable(region, 1) < 0) {
			fprintf(stderr, "%s: failed to disable\n", devname);
			return -ENXIO;
		}
		if (regions[i].enabled && ndctl_region_enable(region) < 0) {
			fprintf(stderr, "%s: failed to enable\n", devname);
			return -ENXIO;
		}
	}

	return 0;
}

static int check_btt_create(struct ndctl_bus *bus, struct btt *create_btt)
{
	struct ndctl_btt *btt;
	void *buf = NULL;
	int i;

	if (!create_btt)
		return 0;

	btt = get_btt_by_id(bus, create_btt->id);
	if (!btt)
		return -ENXIO;

	if (posix_memalign(&buf, 4096, 4096) != 0)
		return -ENXIO;

	ndctl_btt_set_uuid(btt, create_btt->uuid);
	ndctl_btt_set_sector_size(btt, create_btt->sector_size);
	ndctl_btt_set_backing_dev(btt, create_btt->backing_dev);
	ndctl_btt_enable(btt);

	for (i = 0; i < 1; i++) {
		const char *devname = ndctl_btt_get_devname(btt);
		char bdevpath[50];
		int fd;

		sprintf(bdevpath, "/dev/%s", ndctl_btt_get_block_device(btt));
		fd = open(bdevpath, O_RDWR|O_DIRECT);
		if (fd < 0) {
			fprintf(stderr, "%s: failed to open %s\n",
					devname, bdevpath);
			break;
		}
		if (read(fd, buf, 4096) < 4096) {
			fprintf(stderr, "%s: failed to read %s\n",
					devname, bdevpath);
			close(fd);
			break;
		}
		if (write(fd, buf, 4096) < 4096) {
			fprintf(stderr, "%s: failed to write %s\n",
					devname, bdevpath);
			close(fd);
			break;
		}
		close(fd);
	}
	free(buf);
	if (i < 1)
		return -ENXIO;
	return 0;
}

static int check_namespaces(struct ndctl_region *region,
		struct namespace *namespaces, int n)
{
	struct ndctl_bus *bus = ndctl_region_get_bus(region);
	struct ndctl_namespace **ndns_save;
	void *buf = NULL;
	int i, rc = -ENXIO;

	if (!region)
		return -ENXIO;

	ndns_save = calloc(n, sizeof(struct ndctl_namespace *));
	if (!ndns_save)
		return -ENXIO;

	if (posix_memalign(&buf, 4096, 4096) != 0)
		goto out;

	for (i = 0; i < n; i++) {
		struct ndctl_namespace *ndns;
		char devname[50];
		char bdevpath[50];
		int fd;

		snprintf(devname, sizeof(devname), "namespace%d.%d",
				ndctl_region_get_id(region), namespaces[i].id);
		ndns = get_namespace_by_id(region, namespaces[i].id);
		if (!ndns) {
			fprintf(stderr, "%s: failed to find namespace\n",
					devname);
			break;
		}

		if (strcmp(ndctl_namespace_get_type_name(ndns),
					namespaces[i].type) != 0) {
			fprintf(stderr, "%s: expected type: %s got: %s\n",
					devname,
					ndctl_namespace_get_type_name(ndns),
					namespaces[i].type);
			break;
		}

		if (!ndctl_namespace_is_enabled(ndns)) {
			fprintf(stderr, "%s: expected enabled by default\n",
					devname);
			break;
		}

		if (strcmp(ndctl_namespace_get_block_device(ndns),
					namespaces[i].bdev) != 0) {
			fprintf(stderr, "%s: expected block_device: %s got %s\n",
					devname, namespaces[i].bdev,
					ndctl_namespace_get_block_device(ndns));
			break;
		}

		sprintf(bdevpath, "/dev/%s", ndctl_namespace_get_block_device(ndns));
		fd = open(bdevpath, O_RDWR|O_DIRECT);
		if (fd < 0) {
			fprintf(stderr, "%s: failed to open %s\n",
					devname, bdevpath);
			break;
		}
		if (read(fd, buf, 4096) < 4096) {
			fprintf(stderr, "%s: failed to read %s\n",
					devname, bdevpath);
			close(fd);
			break;
		}
		if (write(fd, buf, 4096) < 4096) {
			fprintf(stderr, "%s: failed to write %s\n",
					devname, bdevpath);
			close(fd);
			break;
		}
		close(fd);

		if (check_btt_create(bus, namespaces[i].create_btt) < 0) {
			fprintf(stderr, "failed to create btt%d\n",
					namespaces[i].create_btt->id);
			break;
		}

		if (ndctl_namespace_disable(ndns) < 0) {
			fprintf(stderr, "%s: failed to disable\n", devname);
			break;
		}

		if (ndctl_namespace_enable(ndns) < 0) {
			fprintf(stderr, "%s: failed to enable\n", devname);
			break;
		}
		ndns_save[i] = ndns;
	}
	if (i < n || ndctl_region_disable(region, 0) != 0) {
		if (i >= n)
			fprintf(stderr, "failed to disable region%d\n",
					ndctl_region_get_id(region));
		goto out;
	}

	rc = 0;
	for (i = 0; i < n; i++) {
		char devname[50];

		snprintf(devname, sizeof(devname), "namespace%d.%d",
				ndctl_region_get_id(region), namespaces[i].id);
		if (ndctl_namespace_is_valid(ndns_save[i])) {
			fprintf(stderr, "%s: failed to invalidate\n", devname);
			rc = -ENXIO;
			break;
		}
	}
	ndctl_region_cleanup(region);
 out:
	free(ndns_save);
	free(buf);

	return rc;
}

static int check_btt_supported_sectors(struct ndctl_btt *btt, struct btt *expect_btt)
{
	int s, t;
	char devname[50];

	snprintf(devname, sizeof(devname), "btt%d", expect_btt->id);
	for (s = 0; s < expect_btt->num_sector_sizes; s++) {
		for (t = 0; t < expect_btt->num_sector_sizes; t++) {
			if (ndctl_btt_get_supported_sector_size(btt, t)
					== expect_btt->sector_sizes[s])
				break;
		}
		if (t >= expect_btt->num_sector_sizes) {
			fprintf(stderr, "%s: expected sector_size: %d to be supported\n",
					devname, expect_btt->sector_sizes[s]);
			return -ENXIO;
		}
	}

	return 0;
}

static int check_btts(struct ndctl_bus *bus, struct btt *btts, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		struct ndctl_btt *btt;
		char devname[50];
		uuid_t btt_uuid;
		int rc;

		snprintf(devname, sizeof(devname), "btt%d", btts[i].id);
		btt = get_btt_by_id(bus, btts[i].id);
		if (!btt) {
			fprintf(stderr, "%s: failed to find btt\n", devname);
			return -ENXIO;
		}
		if (strcmp(ndctl_btt_get_backing_dev(btt), btts[i].backing_dev) != 0) {
			fprintf(stderr, "%s: expected backing_dev: %s got: %s\n",
					devname, btts[i].backing_dev,
					ndctl_btt_get_backing_dev(btt));
			return -ENXIO;
		}
		if (ndctl_btt_get_sector_size(btt) != btts[i].sector_size) {
			fprintf(stderr, "%s: expected sector_size: %d got: %d\n",
					devname, btts[i].sector_size,
					ndctl_btt_get_sector_size(btt));
			return -ENXIO;
		}
		ndctl_btt_get_uuid(btt, btt_uuid);
		if (uuid_compare(btt_uuid, btts[i].uuid) != 0) {
			char expect[40], actual[40];

			uuid_unparse(btt_uuid, actual);
			uuid_unparse(btts[i].uuid, expect);
			fprintf(stderr, "%s: expected uuid: %s got: %s\n",
					devname, expect, actual);
			return -ENXIO;
		}
		if (ndctl_btt_get_num_sector_sizes(btt) != btts[i].num_sector_sizes) {
			fprintf(stderr, "%s: expected num_sector_sizes: %d got: %d\n",
					devname, btts[i].num_sector_sizes,
					ndctl_btt_get_num_sector_sizes(btt));
		}
		rc = check_btt_supported_sectors(btt, &btts[i]);
		if (rc)
			return rc;
		if (btts[i].enabled && ndctl_btt_is_enabled(btt)) {
			fprintf(stderr, "%s: expected disabled by default\n",
					devname);
			return -ENXIO;
		}

		if (strcmp(ndctl_btt_get_block_device(btt), btts[i].bdev) != 0) {
			fprintf(stderr, "%s: expected block_device: %s got %s\n",
					devname, btts[i].bdev,
					ndctl_btt_get_block_device(btt));
			return -ENXIO;
		}
	}

	return 0;
}

#define BITS_PER_LONG 32
static int check_commands(struct ndctl_bus *bus, unsigned long commands)
{
	int i;

	for (i = 0; i < BITS_PER_LONG; i++) {
		if ((commands & (1UL << i)) == 0)
			continue;
		if (!ndctl_bus_is_cmd_supported(bus, i)) {
			fprintf(stderr, "bus: %s expected cmd: %d (%s) supported\n",
					ndctl_bus_get_provider(bus), i,
					ndctl_bus_get_cmd_name(bus, i));
			return -ENXIO;
		}
	}

	return 0;
}

static int do_test0(struct ndctl_ctx *ctx)
{
	struct ndctl_bus *bus = get_bus_by_provider(ctx, NFIT_PROVIDER0);
	unsigned int i;
	int rc;

	if (!bus)
		return -ENXIO;

	for (i = 0; i < ARRAY_SIZE(dimms); i++) {
		struct ndctl_dimm *dimm = get_dimm_by_handle(bus, dimms[i].handle);

		if (!dimm) {
			fprintf(stderr, "failed to find dimm: %d\n", dimms[i].phys_id);
			return -ENXIO;
		}

		if (ndctl_dimm_get_phys_id(dimm) != dimms[i].phys_id) {
			fprintf(stderr, "dimm%d expected phys_id: %d got: %d\n",
					i, dimms[i].phys_id,
					ndctl_dimm_get_phys_id(dimm));
			return -ENXIO;
		}
	}

	rc = check_commands(bus, commands0);
	if (rc)
		return rc;

	rc = check_regions(bus, regions0, ARRAY_SIZE(regions0));
	if (rc)
		return rc;

	return check_btts(bus, btts0, ARRAY_SIZE(btts0));
}

static int do_test1(struct ndctl_ctx *ctx)
{
	struct ndctl_bus *bus = get_bus_by_provider(ctx, NFIT_PROVIDER1);
	struct ndctl_region *region;
	int rc;

	if (!bus)
		return -ENXIO;

	rc = check_regions(bus, regions1, ARRAY_SIZE(regions1));
	if (rc)
		return rc;

	region = get_region_by_id(bus, regions1[0].id);

	rc = check_btts(bus, btts1, ARRAY_SIZE(btts1));
	if (rc)
		return rc;

	return check_namespaces(region, namespaces1, ARRAY_SIZE(namespaces1));
}

typedef int (*do_test_fn)(struct ndctl_ctx *ctx);
static do_test_fn do_test[] = {
	do_test0,
	do_test1,
};

int main(int argc, char *argv[])
{
	unsigned int i;
	struct ndctl_ctx *ctx;
	struct kmod_module *mod;
	struct kmod_ctx *kmod_ctx;
	int err, result = EXIT_FAILURE;
	const char *null_config = NULL;

	err = ndctl_new(&ctx);
	if (err < 0)
		exit(EXIT_FAILURE);

	ndctl_set_log_priority(ctx, LOG_DEBUG);

	kmod_ctx = kmod_new(NULL, &null_config);
	if (!kmod_ctx)
		goto err_kmod;

	err = kmod_module_new_from_name(kmod_ctx, NFIT_TEST_MODULE, &mod);
	if (err < 0)
		goto err_module;

	err = kmod_module_probe_insert_module(mod, KMOD_PROBE_APPLY_BLACKLIST,
			NULL, NULL, NULL, NULL);
	if (err < 0)
		goto err_module;

	for (i = 0; i < ARRAY_SIZE(do_test); i++) {
		err = do_test[i](ctx);
		if (err < 0) {
			fprintf(stderr, "ndctl-test%d failed: %d\n", i, err);
			break;
		}
	}

	if (i >= ARRAY_SIZE(do_test))
		result = EXIT_SUCCESS;
	kmod_module_remove_module(mod, 0);

 err_module:
	kmod_unref(kmod_ctx);
 err_kmod:
	ndctl_unref(ctx);
	return result;
}
