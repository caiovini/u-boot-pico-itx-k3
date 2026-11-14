#include <command.h>
#include <exports.h>
#include <malloc.h>
#include <spi.h>
#include <i2c.h>
#include <asm/global_data.h>
#if CONFIG_IS_ENABLED(BLK)
#include <blk.h>
#include <dm.h>
#if CONFIG_IS_ENABLED(NVME)
#include <nvme.h>
#endif
#endif

DECLARE_GLOBAL_DATA_PTR;

#if CONFIG_IS_ENABLED(BLK)
static struct udevice *export_get_nvme_blk_dev(void)
{
	static struct udevice *cached_bdev;

	if (cached_bdev && device_active(cached_bdev))
		return cached_bdev;

#if CONFIG_IS_ENABLED(NVME)
	if (blk_first_device(UCLASS_NVME, &cached_bdev) != 0 || !cached_bdev) {
		nvme_scan_namespace();
		blk_first_device(UCLASS_NVME, &cached_bdev);
	}
#endif
	return cached_bdev;
}

unsigned long blk_get_capacity(void)
{
	struct udevice *bdev = export_get_nvme_blk_dev();
	struct blk_desc *desc;

	if (!bdev)
		return 0;
	desc = dev_get_uclass_plat(bdev);
	return desc ? (unsigned long)desc->lba : 0;
}

unsigned long blk_read_lba(unsigned long start, unsigned long blkcnt, void *buffer)
{
	struct udevice *bdev = export_get_nvme_blk_dev();
	long ret;

	if (!bdev || !buffer)
		return 0;
	ret = blk_read(bdev, (lbaint_t)start, (lbaint_t)blkcnt, buffer);
	return ret > 0 ? (unsigned long)ret : 0;
}

unsigned long blk_write_lba(unsigned long start, unsigned long blkcnt, const void *buffer)
{
	struct udevice *bdev = export_get_nvme_blk_dev();
	long ret;

	if (!bdev || !buffer)
		return 0;
	ret = blk_write(bdev, (lbaint_t)start, (lbaint_t)blkcnt, buffer);
	return ret > 0 ? (unsigned long)ret : 0;
}
#endif

unsigned long get_version(void)
{
	return XF_VERSION;
}

#define EXPORT_FUNC(f, a, x, ...)  gd->jt->x = f;

int jumptable_init(void)
{
	gd->jt = malloc(sizeof(struct jt_funcs));
#include <_exports.h>

	return 0;
}
