/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * 6.12 deleted the 4.19 subsystem_get() "look up a remoteproc by name"
 * registry that the a5xx/a6xx zap-shader paths used. Stock gets the zap
 * shader through PIL (soc:qcom,kgsl-hyp, qcom,pas-id = 13,
 * firmware-name "a506_zap"); mainline loads the same image itself from
 * the GPU node's zap-shader child, which msm8953.dtsi already carries.
 * This follows drivers/gpu/drm/msm/adreno/adreno_gpu.c
 * zap_shader_load_mdt().
 *
 * The A5xx comes out of reset in secure mode. Only an authenticated zap
 * shader lets the CP leave it (CP_SET_SECURE_MODE 0); the fallback write
 * of RBBM_SECVID_TRUST_CNTL from the CPU is blocked on content-protected
 * parts (see a5xx_hw_init() in drm/msm).
 *
 * The callers only test the result for an error, so a non-error pointer
 * means "loaded".
 */

#ifndef _KGSL_ZAP_COMPAT_H
#define _KGSL_ZAP_COMPAT_H

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/soc/qcom/mdt_loader.h>

#define ADRENO_ZAP_PAS_ID	13

static inline void *adreno_zap_subsystem_get(struct device *dev,
		const char *name)
{
	struct device_node *np, *mem_np;
	const struct firmware *fw;
	struct resource r;
	char *fwname;
	void *mem;
	ssize_t size;
	int ret;

	np = of_get_child_by_name(dev->of_node, "zap-shader");
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}

	mem_np = of_parse_phandle(np, "memory-region", 0);
	of_node_put(np);
	if (!mem_np)
		return ERR_PTR(-EINVAL);

	ret = of_address_to_resource(mem_np, 0, &r);
	of_node_put(mem_np);
	if (ret)
		return ERR_PTR(ret);

	if (!qcom_scm_is_available() ||
	    !qcom_scm_pas_supported(ADRENO_ZAP_PAS_ID)) {
		dev_err(dev, "zap: SCM PAS %d not supported\n",
			ADRENO_ZAP_PAS_ID);
		return ERR_PTR(-EOPNOTSUPP);
	}


	fwname = kasprintf(GFP_KERNEL, "%s.mdt", name);
	if (!fwname)
		return ERR_PTR(-ENOMEM);

	ret = request_firmware(&fw, fwname, dev);
	if (ret) {
		dev_err(dev, "zap: request %s failed: %d\n", fwname, ret);
		goto out_name;
	}

	size = qcom_mdt_get_size(fw);
	if (size < 0) {
		ret = size;
		goto out_fw;
	}
	if (size > resource_size(&r)) {
		dev_err(dev, "zap: %zd bytes do not fit %pR\n", size, &r);
		ret = -E2BIG;
		goto out_fw;
	}

	mem = memremap(r.start, size, MEMREMAP_WC);
	if (!mem) {
		ret = -ENOMEM;
		goto out_fw;
	}

	/*
	 * qcom_mdt_load() split in two so each SCM step gets its own stage
	 * record. The pair is exactly qcom_mdt_load(); the .bNN segment names
	 * are derived from fwname.
	 */
	ret = qcom_mdt_pas_init(dev, fw, fwname, ADRENO_ZAP_PAS_ID, r.start,
			NULL);
	if (!ret) {
		ret = qcom_mdt_load_no_init(dev, fw, fwname,
				mem, r.start, size, NULL);
	}
	if (ret) {
		dev_err(dev, "zap: mdt load failed: %d\n", ret);
		goto out_mem;
	}

	/*
	 * Authenticate before dropping the CPU mapping. mainline
	 * adreno_gpu.c zap_shader_load_mdt() and OSS kgsl both run
	 * qcom_scm_pas_auth_and_reset() first and memunmap() at their out:
	 * label; the image only has to be in physical memory.
	 */
	ret = qcom_scm_pas_auth_and_reset(ADRENO_ZAP_PAS_ID);

out_mem:
	memunmap(mem);
out_fw:
	release_firmware(fw);
out_name:
	kfree(fwname);
	return ret ? ERR_PTR(ret) : (void *)dev;
}

#endif /* _KGSL_ZAP_COMPAT_H */
