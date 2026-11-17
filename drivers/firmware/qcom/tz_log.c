// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Qualcomm TrustZone diagnostic area reader.
 *
 * TrustZone publishes the physical address of its diagnostic area in an
 * IMEM cookie, which the "qcom,tz-log" node describes; the size of the
 * node's register range is the size of the diagnostic area. The area
 * starts with a header of offsets to the VMID, boot, reset and interrupt
 * tables and to the TrustZone log ring buffer.
 *
 * Probe maps the cookie and touches no hardware. The cookie is read and
 * the diagnostic area is mapped on the first read of a debugfs file, and
 * every read copies the whole area before decoding it, so every offset
 * TrustZone reports is checked against the copy.
 */

#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <soc/qcom/scm.h>

/* TZ_INFO_GET_FEATURE_VERSION: SIP service INFO, command 3, one value */
#define TZ_INFO_GET_FEATURE_VERSION	SCM_SIP_FNID(SCM_SVC_INFO, 0x03)
#define QCOM_SCM_FEAT_LOG_ID		0x0a
#define QCOM_SCM_FEAT_DIAG_ID		0x06

/* Diagnostic area versions with the 16-bit or the 32-bit ring position */
#define TZBSP_DIAG_MAJOR_VERSION_LEGACY	2
#define TZBSP_FVER_MAJOR_MINOR_MASK	0x3ff
#define TZBSP_FVER_MAJOR_SHIFT		22
#define TZBSP_FVER_MINOR_SHIFT		12
#define TZBSP_DIAG_MAJOR_VERSION_V9	9
#define TZBSP_DIAG_MINOR_VERSION_V2	2
#define TZBSP_DIAG_MINOR_VERSION_V21	3

/* Log feature versions that changed the table layouts */
#define QSEE_VERSION_TZ_3_X		0x800000
#define QSEE_VERSION_TZ_4_X		0x1000000

#define TZBSP_MAX_CPU_COUNT		8
#define TZBSP_DIAG_VMID_DESC_LEN	7
#define TZBSP_MAX_INT_DESC		16

struct tzdbg_hdr {
	u32 magic_num;
	u32 version;
	u32 cpu_count;
	u32 vmid_info_off;
	u32 boot_info_off;
	u32 reset_info_off;
	u32 int_info_off;
	u32 ring_off;
	u32 ring_len;
	u32 wakeup_info_off;
};

struct tzdbg_vmid {
	u8 vmid;
	u8 desc[TZBSP_DIAG_VMID_DESC_LEN];
};

struct tzdbg_boot_info {
	u32 wb_entry_cnt;
	u32 wb_exit_cnt;
	u32 pc_entry_cnt;
	u32 pc_exit_cnt;
	u32 warm_jmp_addr;
	u32 spare;
};

/* Boot table of log feature version 3.x and later */
struct tzdbg_boot_info64 {
	u32 wb_entry_cnt;
	u32 wb_exit_cnt;
	u32 pc_entry_cnt;
	u32 pc_exit_cnt;
	u32 psci_entry_cnt;
	u32 psci_exit_cnt;
	u64 warm_jmp_addr;
	u32 warm_jmp_instr;
};

struct tzdbg_reset_info {
	u32 reset_type;
	u32 reset_cnt;
};

struct tzdbg_int {
	u16 int_info;
	u8 avail;
	u8 spare;
	u32 int_num;
	u8 int_desc[TZBSP_MAX_INT_DESC];
	u64 int_count[TZBSP_MAX_CPU_COUNT];
};

/* Interrupt table of log feature version 4.x and later */
struct tzdbg_int_tz40 {
	u16 int_info;
	u8 avail;
	u8 spare;
	u32 int_num;
	u8 int_desc[TZBSP_MAX_INT_DESC];
	u32 int_count[TZBSP_MAX_CPU_COUNT];
};

struct tzdbg_log_pos {
	u16 wrap;
	u16 offset;
};

struct tzdbg_log_pos_v2 {
	u32 wrap;
	u32 offset;
};

struct tzdbg {
	struct device *dev;
	struct mutex lock;		/* serialises the copy and its users */
	void __iomem *cookie;
	void __iomem *diag;
	phys_addr_t diag_phys;
	size_t diag_size;
	void *buf;
	bool versions_queried;
	u32 log_version;
	u32 diag_version;
	bool enlarged_buf;
};

static const void *tzdbg_ptr(const struct tzdbg *tz, u32 off, size_t len)
{
	if (off > tz->diag_size || len > tz->diag_size - off)
		return NULL;

	return (const u8 *)tz->buf + off;
}

/* An unknown log version (failed call, or 0) is taken as the 4.x layout. */
static bool tzdbg_layout_at_least(const struct tzdbg *tz, u32 version)
{
	return !tz->log_version || tz->log_version >= version;
}

static void tzdbg_query_versions(struct tzdbg *tz)
{
	struct scm_desc desc = { .arginfo = SCM_ARGS(1) };
	u32 major, minor;
	int ret;

	if (tz->versions_queried || !qcom_scm_is_available())
		return;

	tz->versions_queried = true;

	desc.args[0] = QCOM_SCM_FEAT_LOG_ID;
	ret = scm_call2(TZ_INFO_GET_FEATURE_VERSION, &desc);
	if (ret)
		dev_warn(tz->dev, "log feature version query failed: %d\n", ret);
	else
		tz->log_version = desc.ret[0];

	desc.args[0] = QCOM_SCM_FEAT_DIAG_ID;
	ret = scm_call2(TZ_INFO_GET_FEATURE_VERSION, &desc);
	if (ret) {
		dev_warn(tz->dev, "diag feature version query failed: %d\n", ret);
		return;
	}

	tz->diag_version = desc.ret[0];
	major = (tz->diag_version >> TZBSP_FVER_MAJOR_SHIFT) &
		TZBSP_FVER_MAJOR_MINOR_MASK;
	minor = (tz->diag_version >> TZBSP_FVER_MINOR_SHIFT) &
		TZBSP_FVER_MAJOR_MINOR_MASK;
	tz->enlarged_buf = major == TZBSP_DIAG_MAJOR_VERSION_V9 &&
			   (minor == TZBSP_DIAG_MINOR_VERSION_V2 ||
			    minor == TZBSP_DIAG_MINOR_VERSION_V21);
}

/* Map the diagnostic area on first use; called with tz->lock held. */
static int tzdbg_map(struct tzdbg *tz)
{
	phys_addr_t base;

	tzdbg_query_versions(tz);

	if (tz->diag)
		return 0;

	base = readl(tz->cookie);
	if (!base || !IS_ALIGNED(base, sizeof(u32))) {
		dev_err_ratelimited(tz->dev,
				    "invalid diagnostic area address %pa\n",
				    &base);
		return -ENXIO;
	}

	/* ioremap() refuses RAM in the linear map; say why instead. */
	if (page_is_ram(PHYS_PFN(base)) ||
	    page_is_ram(PHYS_PFN(base + tz->diag_size - 1))) {
		dev_err_ratelimited(tz->dev,
				    "diagnostic area %pa is in system RAM\n",
				    &base);
		return -ENXIO;
	}

	tz->diag = devm_ioremap(tz->dev, base, tz->diag_size);
	if (!tz->diag)
		return -ENOMEM;

	tz->diag_phys = base;
	dev_info(tz->dev, "diagnostic area at %pa, %zu bytes\n", &base,
		 tz->diag_size);

	return 0;
}

/* Copy the diagnostic area; called with tz->lock held. */
static const struct tzdbg_hdr *tzdbg_snapshot(struct tzdbg *tz)
{
	int ret;

	ret = tzdbg_map(tz);
	if (ret)
		return ERR_PTR(ret);

	memcpy_fromio(tz->buf, tz->diag, tz->diag_size);

	return tz->buf;
}

static u32 tzdbg_cpu_count(struct seq_file *s, const struct tzdbg_hdr *hdr)
{
	if (hdr->cpu_count <= TZBSP_MAX_CPU_COUNT)
		return hdr->cpu_count;

	seq_printf(s, "cpu_count %u, showing %u\n", hdr->cpu_count,
		   TZBSP_MAX_CPU_COUNT);

	return TZBSP_MAX_CPU_COUNT;
}

static int tzdbg_general_show(struct seq_file *s, void *unused)
{
	struct tzdbg *tz = s->private;
	const struct tzdbg_hdr *hdr;

	guard(mutex)(&tz->lock);

	hdr = tzdbg_snapshot(tz);
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	seq_printf(s, "   Version        : 0x%x\n", hdr->version);
	seq_printf(s, "   Magic Number   : 0x%x\n", hdr->magic_num);
	seq_printf(s, "   Number of CPU  : %u\n", hdr->cpu_count);
	seq_printf(s, "   Diag area      : %pa, %zu bytes\n", &tz->diag_phys,
		   tz->diag_size);
	seq_printf(s, "   Log feature    : 0x%x%s\n", tz->log_version,
		   tz->log_version ? "" : " (unknown, 4.x layout assumed)");
	seq_printf(s, "   Diag feature   : 0x%x\n", tz->diag_version);
	seq_printf(s, "   VMID offset    : 0x%x\n", hdr->vmid_info_off);
	seq_printf(s, "   Boot offset    : 0x%x\n", hdr->boot_info_off);
	seq_printf(s, "   Reset offset   : 0x%x\n", hdr->reset_info_off);
	seq_printf(s, "   Int offset     : 0x%x\n", hdr->int_info_off);
	seq_printf(s, "   Ring offset    : 0x%x\n", hdr->ring_off);
	seq_printf(s, "   Ring length    : 0x%x\n", hdr->ring_len);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tzdbg_general);

static int tzdbg_vmid_show(struct seq_file *s, void *unused)
{
	struct tzdbg *tz = s->private;
	const struct tzdbg_hdr *hdr;
	const struct tzdbg_vmid *vmid;
	u32 i, num;

	guard(mutex)(&tz->lock);

	hdr = tzdbg_snapshot(tz);
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	if (hdr->boot_info_off < hdr->vmid_info_off)
		return -EINVAL;

	num = (hdr->boot_info_off - hdr->vmid_info_off) / sizeof(*vmid);
	vmid = tzdbg_ptr(tz, hdr->vmid_info_off, num * sizeof(*vmid));
	if (!vmid)
		return -EINVAL;

	for (i = 0; i < num; i++, vmid++) {
		if (vmid->vmid == 0xff)
			continue;

		seq_printf(s, "   0x%x        %.*s\n", vmid->vmid,
			   (int)strnlen((const char *)vmid->desc,
					sizeof(vmid->desc)),
			   (const char *)vmid->desc);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tzdbg_vmid);

static int tzdbg_boot_show(struct seq_file *s, void *unused)
{
	struct tzdbg *tz = s->private;
	const struct tzdbg_boot_info64 *b64;
	const struct tzdbg_boot_info *b;
	const struct tzdbg_hdr *hdr;
	u32 i, cpus;

	guard(mutex)(&tz->lock);

	hdr = tzdbg_snapshot(tz);
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	cpus = tzdbg_cpu_count(s, hdr);

	if (!tzdbg_layout_at_least(tz, QSEE_VERSION_TZ_3_X)) {
		b = tzdbg_ptr(tz, hdr->boot_info_off, cpus * sizeof(*b));
		if (!b)
			return -EINVAL;

		for (i = 0; i < cpus; i++, b++)
			seq_printf(s,
				   "  CPU #: %u\n"
				   "     Warmboot jump address     : 0x%x\n"
				   "     Warmboot entry CPU counter: 0x%x\n"
				   "     Warmboot exit CPU counter : 0x%x\n"
				   "     Power Collapse entry CPU counter: 0x%x\n"
				   "     Power Collapse exit CPU counter : 0x%x\n",
				   i, b->warm_jmp_addr, b->wb_entry_cnt,
				   b->wb_exit_cnt, b->pc_entry_cnt,
				   b->pc_exit_cnt);
		return 0;
	}

	b64 = tzdbg_ptr(tz, hdr->boot_info_off, cpus * sizeof(*b64));
	if (!b64)
		return -EINVAL;

	for (i = 0; i < cpus; i++, b64++)
		seq_printf(s,
			   "  CPU #: %u\n"
			   "     Warmboot jump address : 0x%llx\n"
			   "     Warmboot entry CPU counter : 0x%x\n"
			   "     Warmboot exit CPU counter : 0x%x\n"
			   "     Power Collapse entry CPU counter : 0x%x\n"
			   "     Power Collapse exit CPU counter : 0x%x\n"
			   "     Psci entry CPU counter : 0x%x\n"
			   "     Psci exit CPU counter : 0x%x\n"
			   "     Warmboot Jump Address Instruction : 0x%x\n",
			   i, b64->warm_jmp_addr, b64->wb_entry_cnt,
			   b64->wb_exit_cnt, b64->pc_entry_cnt,
			   b64->pc_exit_cnt, b64->psci_entry_cnt,
			   b64->psci_exit_cnt, b64->warm_jmp_instr);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tzdbg_boot);

static int tzdbg_reset_show(struct seq_file *s, void *unused)
{
	struct tzdbg *tz = s->private;
	const struct tzdbg_reset_info *r;
	const struct tzdbg_hdr *hdr;
	u32 i, cpus;

	guard(mutex)(&tz->lock);

	hdr = tzdbg_snapshot(tz);
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	cpus = tzdbg_cpu_count(s, hdr);
	r = tzdbg_ptr(tz, hdr->reset_info_off, cpus * sizeof(*r));
	if (!r)
		return -EINVAL;

	for (i = 0; i < cpus; i++, r++)
		seq_printf(s,
			   "  CPU #: %u\n"
			   "     Reset Type (reason)       : 0x%x\n"
			   "     Reset counter             : 0x%x\n",
			   i, r->reset_type, r->reset_cnt);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tzdbg_reset);

static void tzdbg_int_entry(struct seq_file *s, u32 int_num, u16 int_info,
			    const u8 *desc)
{
	seq_printf(s,
		   "     Interrupt Number          : 0x%x\n"
		   "     Type of Interrupt         : 0x%x\n"
		   "     Description of interrupt  : %.*s\n",
		   int_num, int_info,
		   (int)strnlen((const char *)desc, TZBSP_MAX_INT_DESC),
		   (const char *)desc);
}

static int tzdbg_interrupt_show(struct seq_file *s, void *unused)
{
	struct tzdbg *tz = s->private;
	const struct tzdbg_int_tz40 *e40;
	const struct tzdbg_hdr *hdr;
	const struct tzdbg_int *e;
	const u32 *num_int;
	u32 i, j, cpus, num;

	guard(mutex)(&tz->lock);

	hdr = tzdbg_snapshot(tz);
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	if (hdr->int_info_off < sizeof(*num_int))
		return -EINVAL;

	num_int = tzdbg_ptr(tz, hdr->int_info_off - sizeof(*num_int),
			    sizeof(*num_int));
	if (!num_int)
		return -EINVAL;

	num = *num_int;
	cpus = tzdbg_cpu_count(s, hdr);

	if (!tzdbg_layout_at_least(tz, QSEE_VERSION_TZ_4_X)) {
		e = tzdbg_ptr(tz, hdr->int_info_off, (size_t)num * sizeof(*e));
		if (!e)
			return -EINVAL;

		for (i = 0; i < num; i++, e++) {
			tzdbg_int_entry(s, e->int_num, e->int_info,
					e->int_desc);
			for (j = 0; j < cpus; j++)
				seq_printf(s,
					   "     int_count on CPU # %u      : %llu\n",
					   j, e->int_count[j]);
			seq_putc(s, '\n');
		}
		return 0;
	}

	e40 = tzdbg_ptr(tz, hdr->int_info_off, (size_t)num * sizeof(*e40));
	if (!e40)
		return -EINVAL;

	for (i = 0; i < num; i++, e40++) {
		tzdbg_int_entry(s, e40->int_num, e40->int_info,
				e40->int_desc);
		for (j = 0; j < cpus; j++)
			seq_printf(s, "     int_count on CPU # %u      : %u\n",
				   j, e40->int_count[j]);
		seq_putc(s, '\n');
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tzdbg_interrupt);

/*
 * The ring position sits just before the ring. Print the ring oldest byte
 * first: after a wrap that is the byte at the write offset.
 */
static int tzdbg_log_show(struct seq_file *s, void *unused)
{
	struct tzdbg *tz = s->private;
	const struct tzdbg_log_pos_v2 *pos2;
	const struct tzdbg_log_pos *pos;
	const struct tzdbg_hdr *hdr;
	u32 wrap, offset, len;
	size_t pos_size;
	const u8 *ring;

	guard(mutex)(&tz->lock);

	hdr = tzdbg_snapshot(tz);
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	if ((hdr->version >> 16) <= TZBSP_DIAG_MAJOR_VERSION_LEGACY) {
		ring = tzdbg_ptr(tz, hdr->ring_off, 0);
		if (!ring)
			return -EINVAL;

		len = tz->diag_size - hdr->ring_off;
		seq_write(s, ring, strnlen((const char *)ring, len));
		seq_putc(s, '\n');
		return 0;
	}

	pos_size = tz->enlarged_buf ? sizeof(*pos2) : sizeof(*pos);
	len = hdr->ring_len;
	if (hdr->ring_off < pos_size || !len)
		return -EINVAL;

	ring = tzdbg_ptr(tz, hdr->ring_off - pos_size, pos_size + len);
	if (!ring)
		return -EINVAL;

	if (tz->enlarged_buf) {
		pos2 = (const void *)ring;
		wrap = pos2->wrap;
		offset = pos2->offset;
	} else {
		pos = (const void *)ring;
		wrap = pos->wrap;
		offset = pos->offset;
	}
	ring += pos_size;

	if (offset >= len)
		return -EINVAL;

	if (wrap)
		seq_write(s, ring + offset, len - offset);
	seq_write(s, ring, offset);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tzdbg_log);

/* Raw copy of the diagnostic area for offline decoding. */
static ssize_t tzdbg_diag_read(struct file *file, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct tzdbg *tz = file->private_data;
	const struct tzdbg_hdr *hdr;

	guard(mutex)(&tz->lock);

	if (!*ppos) {
		hdr = tzdbg_snapshot(tz);
		if (IS_ERR(hdr))
			return PTR_ERR(hdr);
	}

	return simple_read_from_buffer(ubuf, count, ppos, tz->buf,
				       tz->diag_size);
}

static const struct file_operations tzdbg_diag_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = tzdbg_diag_read,
	.llseek = default_llseek,
};

static void tzdbg_remove_debugfs(void *data)
{
	debugfs_remove_recursive(data);
}

static int tz_log_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct dentry *dir;
	struct tzdbg *tz;
	int ret;

	tz = devm_kzalloc(dev, sizeof(*tz), GFP_KERNEL);
	if (!tz)
		return -ENOMEM;

	tz->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -EINVAL, "missing cookie address\n");

	tz->diag_size = resource_size(res);
	if (tz->diag_size < sizeof(struct tzdbg_hdr))
		return dev_err_probe(dev, -EINVAL,
				     "diagnostic area size %zu too small\n",
				     tz->diag_size);

	/* Only the cookie lives at this address; the region size is not IMEM. */
	tz->cookie = devm_ioremap(dev, res->start, sizeof(u32));
	if (!tz->cookie)
		return -ENOMEM;

	tz->buf = devm_kzalloc(dev, tz->diag_size, GFP_KERNEL);
	if (!tz->buf)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &tz->lock);
	if (ret)
		return ret;

	dir = debugfs_create_dir("tzdbg", NULL);
	ret = devm_add_action_or_reset(dev, tzdbg_remove_debugfs, dir);
	if (ret)
		return ret;

	debugfs_create_file("general", 0400, dir, tz, &tzdbg_general_fops);
	debugfs_create_file("vmid", 0400, dir, tz, &tzdbg_vmid_fops);
	debugfs_create_file("boot", 0400, dir, tz, &tzdbg_boot_fops);
	debugfs_create_file("reset", 0400, dir, tz, &tzdbg_reset_fops);
	debugfs_create_file("interrupt", 0400, dir, tz,
			    &tzdbg_interrupt_fops);
	debugfs_create_file("log", 0400, dir, tz, &tzdbg_log_fops);
	debugfs_create_file("diag", 0400, dir, tz, &tzdbg_diag_fops);

	return 0;
}

static const struct of_device_id tz_log_of_match[] = {
	{ .compatible = "qcom,tz-log" },
	{ }
};
MODULE_DEVICE_TABLE(of, tz_log_of_match);

static struct platform_driver tz_log_driver = {
	.probe = tz_log_probe,
	.driver = {
		.name = "tz_log",
		.of_match_table = tz_log_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(tz_log_driver);

MODULE_DESCRIPTION("Qualcomm TrustZone diagnostic log reader");
MODULE_LICENSE("GPL");
