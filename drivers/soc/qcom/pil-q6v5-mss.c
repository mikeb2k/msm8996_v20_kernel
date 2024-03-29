/*
 * Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/of_gpio.h>
#include <linux/clk/msm-clk.h>
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/ramdump.h>
#include <soc/qcom/smem.h>
#include <soc/qcom/smsm.h>

#include "peripheral-loader.h"
#include "pil-q6v5.h"
#include "pil-msa.h"

#include <linux/time.h>

#include "dirtysanta_fixup.h"


/* Added getting MSM chip version info, 2014-12-21, secheol.pyo@lge.com*/
#define FEATURE_LGE_MODEM_LDB_EXCEPTION
#define FEATURE_LGE_MODEM_CHIPVER_INFO

#ifdef FEATURE_LGE_MODEM_LDB_EXCEPTION
#define LEN_EXCEP_MSG	100
#define EXCEPTION_STR   19
#endif

#ifdef FEATURE_LGE_MODEM_CHIPVER_INFO
typedef struct modem_chip_info {
	uint32_t chip_version;
	uint32_t chip_id;
	uint32_t chip_family;
} lg_chip_info;
#endif

/* FEATURE_LGE_MODEM_DEBUG_INFO, 2014-08-18, jin.park@lge.com */
#define FEATURE_LGE_MODEM_DEBUG_INFO
#ifdef FEATURE_LGE_MODEM_DEBUG_INFO
#include <asm/uaccess.h>
#include <linux/syscalls.h>

#define MAX_WRITE_SSR   2
#endif
#define MAX_VDD_MSS_UV		1150000
#define PROXY_TIMEOUT_MS	10000
#define MAX_SSR_REASON_LEN	255U
#define STOP_ACK_TIMEOUT_MS	1000

struct lge_hw_smem_id2_type {
			uint32_t sbl_log_meta_info;
			uint32_t sbl_delta_time;
			uint32_t lcd_maker;
			uint32_t secure_auth;
			uint32_t build_info;			   /* build type user:0 userdebug:1 eng:2 */
			int modem_reset;
#ifdef FEATURE_LGE_MODEM_CHIPVER_INFO
			uint32_t mChipVersion;
			uint32_t mChipId;
			uint32_t mChipFamily;

#endif
#ifdef FEATURE_LGE_MODEM_LDB_EXCEPTION
    char lge_excep_msg[LEN_EXCEP_MSG];
#endif
};
	
#define subsys_to_drv(d) container_of(d, struct modem_data, subsys_desc)

#ifdef FEATURE_LGE_MODEM_DEBUG_INFO
enum modem_ssr_event {
    MODEM_SSR_ERR_FATAL = 0x01,
    MODEM_SSR_WATCHDOG_BITE,
    MODEM_SSR_UNEXPECTED_RESET1,
    MODEM_SSR_UNEXPECTED_RESET2,
};

struct modem_debug_info {
    char save_ssr_reason[MAX_SSR_REASON_LEN];
#ifdef FEATURE_LGE_MODEM_CHIPVER_INFO
    char save_msm_chip_info[MAX_SSR_REASON_LEN];
#endif
#ifdef FEATURE_LGE_MODEM_LDB_EXCEPTION
    char save_excep_reason[LEN_EXCEP_MSG];
#endif
    int modem_ssr_event;
    int modem_ssr_level;
    struct workqueue_struct *modem_ssr_queue;
    struct work_struct modem_ssr_report_work;
};
struct modem_debug_info modem_debug;

static void modem_ssr_report_work_fn(struct work_struct *work)
{

    int fd, ret = 0;
    char report_index = 0;
    char index_to_write = 0;
    char path_prefix[]="/data/logger/modem_ssr_report";
    char index_file_path[]="/data/logger/modem_ssr_index";
    char report_file_path[128];
#if defined (FEATURE_LGE_MODEM_CHIPVER_INFO) && defined(FEATURE_LGE_MODEM_LDB_EXCEPTION)
    int i = 0;
    char chip_info_path[]="/data/logger/modem_chip_info";
    struct timespec time;
    struct tm tmresult;
    char time_stamp[128];
#endif
    char watchdog_bite[]="Watchdog bite received from modem software!";
    char unexpected_reset1[]="unexpected reset external modem";
    char unexpected_reset2[]="MDM2AP_STATUS did not go high";

    mm_segment_t oldfs;

    oldfs = get_fs();
    set_fs(get_ds());

    fd = sys_open(index_file_path, O_RDONLY, 0664);
    if (fd < 0) {
        printk("%s : can't open the index file\n", __func__);
        report_index = '0';
    } else {
        ret = sys_read(fd, &report_index, 1);
        if (ret < 0) {
            printk("%s : can't read the index file\n", __func__);
            report_index = '0';
        }
        sys_close(fd);
    }

    if (report_index == '9') {
        index_to_write = '0';
    } else if (report_index >= '0' && report_index <= '8') {
        index_to_write = report_index + 1;
    } else {
        index_to_write = '1';
        report_index = '0';
    }

    fd = sys_open(index_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        printk("%s : can't open the index file\n", __func__);
        return;
    }

    ret = sys_write(fd, &index_to_write, 1);
    if (ret < 0) {
        printk("%s : can't write the index file\n", __func__);
        return;
    }

    ret = sys_close(fd);
    if (ret < 0) {
        printk("%s : can't close the index file\n", __func__);
        return;
    }

#if defined(FEATURE_LGE_MODEM_CHIPVER_INFO) && defined(FEATURE_LGE_MODEM_LDB_EXCEPTION)
	time = __current_kernel_time();
	time_to_tm(time.tv_sec, sys_tz.tz_minuteswest * 60 * (-1),
				&tmresult);

	sprintf(time_stamp, "[%02d-%02d %02d:%02d:%02d.%03lu] ",
		tmresult.tm_mon+1,tmresult.tm_mday,tmresult.tm_hour,
		tmresult.tm_min,tmresult.tm_sec,(unsigned long) time.tv_nsec/1000000);
#endif
    for (i = 0; i < MAX_WRITE_SSR; i++){
        if (i == 0){
			sprintf(report_file_path, "%s%c", path_prefix, report_index);
			fd = sys_open(report_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
        }
        else if (i == 1){
			fd = sys_open(path_prefix, O_WRONLY | O_CREAT | O_TRUNC, 0664);
        }

		if (fd < 0) {
			printk("%s : can't open the report file\n", __func__);
			return;
		}

#if defined (FEATURE_LGE_MODEM_CHIPVER_INFO) && defined(FEATURE_LGE_MODEM_LDB_EXCEPTION)
		ret = sys_write(fd, time_stamp, strlen(time_stamp));

		if (ret < 0) {
			printk("%s : can't write the report file\n", __func__);
			return;
		}
#endif
		switch (modem_debug.modem_ssr_event) {
			case MODEM_SSR_ERR_FATAL:
				ret = sys_write(fd, modem_debug.save_ssr_reason, strlen(modem_debug.save_ssr_reason));
                                ret = sys_write(fd, "\nException Cause:  ", EXCEPTION_STR);
                                if (modem_debug.save_excep_reason != NULL)
                                    ret = sys_write(fd, modem_debug.save_excep_reason, strlen(modem_debug.save_excep_reason));
				break;
			case MODEM_SSR_WATCHDOG_BITE:
				ret = sys_write(fd, watchdog_bite, sizeof(watchdog_bite) - 1);
				break;
			case MODEM_SSR_UNEXPECTED_RESET1:
				ret = sys_write(fd, unexpected_reset1, sizeof(unexpected_reset1) - 1);
				break;
			case MODEM_SSR_UNEXPECTED_RESET2:
				ret = sys_write(fd, unexpected_reset2, sizeof(unexpected_reset2) - 1);
				break;
			default:
				printk("%s : modem_ssr_event error %d\n", __func__, modem_debug.modem_ssr_event);
				break;
		}

		if (ret < 0) {
			printk("%s : can't write the report file\n", __func__);
			return;
		}

		ret = sys_close(fd);

		if (ret < 0) {
			printk("%s : can't close the report file\n", __func__);
			return;
		}
    }

#ifdef FEATURE_LGE_MODEM_CHIPVER_INFO
	fd = sys_open(chip_info_path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
	if (fd < 0) {
		pr_err("can't open the report file\n");
		return;
	}

	ret = sys_write(fd, modem_debug.save_msm_chip_info, strlen(modem_debug.save_msm_chip_info));
	if (ret < 0) {
		pr_err("can't write the report file\n");
		return;
    }

    ret = sys_close(fd);
    if (ret < 0) {
        printk("%s : can't close the report file\n", __func__);
        return;
    }
#endif

    sys_sync();
    set_fs(oldfs);
}
#endif

static void log_modem_sfr(void)
{
	u32 size;
	char *smem_reason, reason[MAX_SSR_REASON_LEN];

#if defined(FEATURE_LGE_MODEM_CHIPVER_INFO) && defined(FEATURE_LGE_MODEM_LDB_EXCEPTION)
	u32 chip_info_size;
	struct lge_hw_smem_id2_type *chip_info_str;
#endif

	smem_reason = smem_get_entry_no_rlock(SMEM_SSR_REASON_MSS0, &size, 0,
							SMEM_ANY_HOST_FLAG);
	if (!smem_reason || !size) {
		pr_err("modem subsystem failure reason: (unknown, smem_get_entry_no_rlock failed).\n");
		return;
	}
	if (!smem_reason[0]) {
		pr_err("modem subsystem failure reason: (unknown, empty string found).\n");
		return;
	}

	strlcpy(reason, smem_reason, min(size, MAX_SSR_REASON_LEN));
	pr_err("modem subsystem failure reason: %s.\n", reason);

#if defined(FEATURE_LGE_MODEM_CHIPVER_INFO) && defined(FEATURE_LGE_MODEM_LDB_EXCEPTION)
        chip_info_str = smem_get_entry_no_rlock(SMEM_ID_VENDOR2, &chip_info_size, 0, SMEM_ANY_HOST_FLAG);
        if (!chip_info_str || !chip_info_size) {
            pr_err("modem subsystem failure reason: (unknown, smem_get_entry_no_rlock failed).\n");
	    return;
	}
        pr_err("[LGE] MSM Chip version : %d, Family : %d, Id : %d\n", chip_info_str->mChipVersion,chip_info_str->mChipFamily, chip_info_str->mChipId);
#endif

#if defined(FEATURE_LGE_MODEM_DEBUG_INFO) && defined(FEATURE_LGE_MODEM_LDB_EXCEPTION) && defined(FEATURE_LGE_MODEM_CHIPVER_INFO)
    if (modem_debug.modem_ssr_level != RESET_SOC) {
        strlcpy(modem_debug.save_ssr_reason, smem_reason, min(size, MAX_SSR_REASON_LEN));

        snprintf(modem_debug.save_msm_chip_info,
                  MAX_SSR_REASON_LEN,
                  "[LGE] MSM Chip version : %d, Family : %d, Id : %d\n",
                  chip_info_str->mChipVersion,
                  chip_info_str->mChipFamily,
                  chip_info_str->mChipId);
        if (chip_info_str->lge_excep_msg != NULL)
        {
            strlcpy(modem_debug.save_excep_reason,
                 chip_info_str->lge_excep_msg,
                 LEN_EXCEP_MSG);
        }
        modem_debug.modem_ssr_event = MODEM_SSR_ERR_FATAL;
        queue_work(modem_debug.modem_ssr_queue, &modem_debug.modem_ssr_report_work);
    }

    chip_info_str->lge_excep_msg[0] = '\0';
#endif
    smem_reason[0] = '\0';
    wmb();
}

static void restart_modem(struct modem_data *drv)
{

#ifdef FEATURE_LGE_MODEM_DEBUG_INFO
    modem_debug.modem_ssr_level = subsys_get_restart_level(drv->subsys);
#endif

	log_modem_sfr();
	drv->ignore_errors = true;
	subsystem_restart_dev(drv->subsys);
}

static int check_modem_reset(struct modem_data *drv)
{
	u32 size;
	int ret = -EPERM;
	struct lge_hw_smem_id2_type *smem_id2;

	smem_id2 = smem_get_entry(SMEM_ID_VENDOR2, &size, 0, SMEM_ANY_HOST_FLAG);

	if (smem_id2 == NULL) {
		pr_err("%s: smem_id2 is NULL.\n", __func__);
		return ret;
	}

	if(smem_id2->modem_reset != 1) {
		ret = 1;
	} else {
		wmb();
		drv->ignore_errors = true;
		subsys_modem_restart();
		ret = 0;
	}
	return ret;
}

static irqreturn_t modem_err_fatal_intr_handler(int irq, void *dev_id)
{
	struct modem_data *drv = subsys_to_drv(dev_id);

	/* Ignore if we're the one that set the force stop GPIO */
	if (drv->crash_shutdown)
		return IRQ_HANDLED;

	pr_err("Fatal error on the modem.\n");
	subsys_set_crash_status(drv->subsys, true);

	if (check_modem_reset(drv) == 0)
		return IRQ_HANDLED;

	restart_modem(drv);
	return IRQ_HANDLED;
}

static irqreturn_t modem_stop_ack_intr_handler(int irq, void *dev_id)
{
	struct modem_data *drv = subsys_to_drv(dev_id);
	pr_info("Received stop ack interrupt from modem\n");
	complete(&drv->stop_ack);
	return IRQ_HANDLED;
}

static int modem_shutdown(const struct subsys_desc *subsys, bool force_stop)
{
	struct modem_data *drv = subsys_to_drv(subsys);
	unsigned long ret;

	if (subsys->is_not_loadable)
		return 0;

	if (!subsys_get_crash_status(drv->subsys) && force_stop &&
	    subsys->force_stop_gpio) {
		gpio_set_value(subsys->force_stop_gpio, 1);
		ret = wait_for_completion_timeout(&drv->stop_ack,
				msecs_to_jiffies(STOP_ACK_TIMEOUT_MS));
		if (!ret)
			pr_warn("Timed out on stop ack from modem.\n");
		gpio_set_value(subsys->force_stop_gpio, 0);
	}

	if (drv->subsys_desc.ramdump_disable_gpio) {
		drv->subsys_desc.ramdump_disable = gpio_get_value(
					drv->subsys_desc.ramdump_disable_gpio);
		 pr_warn("Ramdump disable gpio value is %d\n",
			drv->subsys_desc.ramdump_disable);
	}

	pil_shutdown(&drv->q6->desc);

	return 0;
}

static int modem_powerup(const struct subsys_desc *subsys)
{
	struct modem_data *drv = subsys_to_drv(subsys);

	if (subsys->is_not_loadable)
		return 0;
	/*
	 * At this time, the modem is shutdown. Therefore this function cannot
	 * run concurrently with the watchdog bite error handler, making it safe
	 * to unset the flag below.
	 */
	reinit_completion(&drv->stop_ack);
	drv->subsys_desc.ramdump_disable = 0;
	drv->ignore_errors = false;
	drv->q6->desc.fw_name = subsys->fw_name;

	pr_info("%s is powering up ", subsys->fw_name);
	return pil_boot(&drv->q6->desc);
}

static void modem_crash_shutdown(const struct subsys_desc *subsys)
{
	struct modem_data *drv = subsys_to_drv(subsys);
	drv->crash_shutdown = true;
	if (!subsys_get_crash_status(drv->subsys) &&
		subsys->force_stop_gpio) {
		gpio_set_value(subsys->force_stop_gpio, 1);
		mdelay(STOP_ACK_TIMEOUT_MS);
	}
}

static int modem_ramdump(int enable, const struct subsys_desc *subsys)
{
	struct modem_data *drv = subsys_to_drv(subsys);
	int ret;

	if (!enable)
		return 0;

	ret = pil_mss_make_proxy_votes(&drv->q6->desc);
	if (ret)
		return ret;

	ret = pil_mss_reset_load_mba(&drv->q6->desc);
	if (ret)
		return ret;

	ret = pil_do_ramdump(&drv->q6->desc, drv->ramdump_dev);
	if (ret < 0)
		pr_err("Unable to dump modem fw memory (rc = %d).\n", ret);

	ret = __pil_mss_deinit_image(&drv->q6->desc, false);
	if (ret < 0)
		pr_err("Unable to free up resources (rc = %d).\n", ret);

	pil_mss_remove_proxy_votes(&drv->q6->desc);
	return ret;
}

static irqreturn_t modem_wdog_bite_intr_handler(int irq, void *dev_id)
{
	struct modem_data *drv = subsys_to_drv(dev_id);
	if (drv->ignore_errors)
		return IRQ_HANDLED;

	pr_err("Watchdog bite received from modem software!\n");
	if (drv->subsys_desc.system_debug &&
			!gpio_get_value(drv->subsys_desc.err_fatal_gpio))
		panic("%s: System ramdump requested. Triggering device restart!\n",
							__func__);
	subsys_set_crash_status(drv->subsys, true);
	restart_modem(drv);

#ifdef FEATURE_LGE_MODEM_DEBUG_INFO
    modem_debug.modem_ssr_level = subsys_get_restart_level(drv->subsys);
    if (modem_debug.modem_ssr_level != RESET_SOC) {
        modem_debug.modem_ssr_event = MODEM_SSR_WATCHDOG_BITE;
        queue_work(modem_debug.modem_ssr_queue, &modem_debug.modem_ssr_report_work);
    }
#endif

	return IRQ_HANDLED;
}

static int pil_subsys_init(struct modem_data *drv,
					struct platform_device *pdev)
{
	int ret;

	drv->subsys_desc.name = "modem";
	drv->subsys_desc.dev = &pdev->dev;
	drv->subsys_desc.owner = THIS_MODULE;
	drv->subsys_desc.shutdown = modem_shutdown;
	drv->subsys_desc.powerup = modem_powerup;
	drv->subsys_desc.ramdump = modem_ramdump;
	drv->subsys_desc.crash_shutdown = modem_crash_shutdown;
	drv->subsys_desc.err_fatal_handler = modem_err_fatal_intr_handler;
	drv->subsys_desc.stop_ack_handler = modem_stop_ack_intr_handler;
	drv->subsys_desc.wdog_bite_handler = modem_wdog_bite_intr_handler;

	drv->q6->desc.modem_ssr = false;
	drv->subsys = subsys_register(&drv->subsys_desc);
	if (IS_ERR(drv->subsys)) {
		ret = PTR_ERR(drv->subsys);
		goto err_subsys;
	}

	drv->q6->desc.subsys_dev = drv->subsys;
	drv->ramdump_dev = create_ramdump_device("modem", &pdev->dev);
	if (!drv->ramdump_dev) {
		pr_err("%s: Unable to create a modem ramdump device.\n",
			__func__);
		ret = -ENOMEM;
		goto err_ramdump;
	}

	dirtysanta_attach(&pdev->dev);

	return 0;

err_ramdump:
	subsys_unregister(drv->subsys);
err_subsys:
	return ret;
}

static int pil_mss_loadable_init(struct modem_data *drv,
					struct platform_device *pdev)
{
	struct q6v5_data *q6;
	struct pil_desc *q6_desc;
	struct resource *res;
	struct property *prop;
	int ret;

	q6 = pil_q6v5_init(pdev);
	if (IS_ERR_OR_NULL(q6))
		return PTR_ERR(q6);
	drv->q6 = q6;
	drv->xo = q6->xo;

	q6_desc = &q6->desc;
	q6_desc->owner = THIS_MODULE;
	q6_desc->proxy_timeout = PROXY_TIMEOUT_MS;

	q6_desc->ops = &pil_msa_mss_ops;

	q6->self_auth = of_property_read_bool(pdev->dev.of_node,
							"qcom,pil-self-auth");
	if (q6->self_auth) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						    "rmb_base");
		q6->rmb_base = devm_ioremap_resource(&pdev->dev, res);
		if (!q6->rmb_base)
			return -ENOMEM;
		drv->rmb_base = q6->rmb_base;
		q6_desc->ops = &pil_msa_mss_ops_selfauth;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "restart_reg");
	if (!res) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"restart_reg_sec");
		q6->restart_reg_sec = true;
	}

	q6->restart_reg = devm_ioremap_resource(&pdev->dev, res);
	if (!q6->restart_reg)
		return -ENOMEM;

	q6->vreg = NULL;

	prop = of_find_property(pdev->dev.of_node, "vdd_mss-supply", NULL);
	if (prop) {
		q6->vreg = devm_regulator_get(&pdev->dev, "vdd_mss");
		if (IS_ERR(q6->vreg))
			return PTR_ERR(q6->vreg);

		ret = regulator_set_voltage(q6->vreg, VDD_MSS_UV,
						MAX_VDD_MSS_UV);
		if (ret)
			dev_err(&pdev->dev, "Failed to set vreg voltage.\n");

		ret = regulator_set_optimum_mode(q6->vreg, 100000);
		if (ret < 0) {
			dev_err(&pdev->dev, "Failed to set vreg mode.\n");
			return ret;
		}
	}

	q6->vreg_mx = devm_regulator_get(&pdev->dev, "vdd_mx");
	if (IS_ERR(q6->vreg_mx))
		return PTR_ERR(q6->vreg_mx);
	prop = of_find_property(pdev->dev.of_node, "vdd_mx-uV", NULL);
	if (!prop) {
		dev_err(&pdev->dev, "Missing vdd_mx-uV property\n");
		return -EINVAL;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
		"cxrail_bhs_reg");
	if (res)
		q6->cxrail_bhs = devm_ioremap(&pdev->dev, res->start,
					  resource_size(res));

	q6->ahb_clk = devm_clk_get(&pdev->dev, "iface_clk");
	if (IS_ERR(q6->ahb_clk))
		return PTR_ERR(q6->ahb_clk);

	q6->axi_clk = devm_clk_get(&pdev->dev, "bus_clk");
	if (IS_ERR(q6->axi_clk))
		return PTR_ERR(q6->axi_clk);

	q6->rom_clk = devm_clk_get(&pdev->dev, "mem_clk");
	if (IS_ERR(q6->rom_clk))
		return PTR_ERR(q6->rom_clk);

	ret = of_property_read_u32(pdev->dev.of_node,
					"qcom,pas-id", &drv->pas_id);
	if (ret)
		dev_warn(&pdev->dev, "Failed to find the pas_id.\n");

	drv->subsys_desc.pil_mss_memsetup =
	of_property_read_bool(pdev->dev.of_node, "qcom,pil-mss-memsetup");

	/* Optional. */
	if (of_property_match_string(pdev->dev.of_node,
			"qcom,active-clock-names", "gpll0_mss_clk") >= 0)
		q6->gpll0_mss_clk = devm_clk_get(&pdev->dev, "gpll0_mss_clk");

	if (of_property_match_string(pdev->dev.of_node,
			"qcom,active-clock-names", "snoc_axi_clk") >= 0)
		q6->snoc_axi_clk = devm_clk_get(&pdev->dev, "snoc_axi_clk");

	if (of_property_match_string(pdev->dev.of_node,
			"qcom,active-clock-names", "mnoc_axi_clk") >= 0)
		q6->mnoc_axi_clk = devm_clk_get(&pdev->dev, "mnoc_axi_clk");

	ret = pil_desc_init(q6_desc);

	return ret;
}

static int pil_mss_driver_probe(struct platform_device *pdev)
{
	struct modem_data *drv;
	int ret, is_not_loadable;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;
	platform_set_drvdata(pdev, drv);

	is_not_loadable = of_property_read_bool(pdev->dev.of_node,
							"qcom,is-not-loadable");
	if (is_not_loadable) {
		drv->subsys_desc.is_not_loadable = 1;
	} else {
		ret = pil_mss_loadable_init(drv, pdev);
		if (ret)
			return ret;
	}
	init_completion(&drv->stop_ack);

	/* Probe the MBA mem device if present */
	ret = of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
	if (ret)
		return ret;

#ifdef FEATURE_LGE_MODEM_DEBUG_INFO
	modem_debug.modem_ssr_queue = alloc_workqueue("modem_ssr_queue", 0, 0);
	if (!modem_debug.modem_ssr_queue) {
		printk("could not create modem_ssr_queue\n");
	}

	modem_debug.modem_ssr_event = 0;
	modem_debug.modem_ssr_level = RESET_SOC;
	INIT_WORK(&modem_debug.modem_ssr_report_work, modem_ssr_report_work_fn);
#endif

	return pil_subsys_init(drv, pdev);
}

static int pil_mss_driver_exit(struct platform_device *pdev)
{
	struct modem_data *drv = platform_get_drvdata(pdev);

	dirtysanta_detach(&pdev->dev);

	subsys_unregister(drv->subsys);
	destroy_ramdump_device(drv->ramdump_dev);
	pil_desc_release(&drv->q6->desc);

#ifdef FEATURE_LGE_MODEM_DEBUG_INFO
    if (modem_debug.modem_ssr_queue) {
        destroy_workqueue(modem_debug.modem_ssr_queue);
        modem_debug.modem_ssr_queue = NULL;
    }
#endif

	return 0;
}

static int pil_mba_mem_driver_probe(struct platform_device *pdev)
{
	struct modem_data *drv;

	if (!pdev->dev.parent) {
		pr_err("No parent found.\n");
		return -EINVAL;
	}
	drv = dev_get_drvdata(pdev->dev.parent);
	drv->mba_mem_dev_fixed = &pdev->dev;
	return 0;
}

static struct of_device_id mba_mem_match_table[] = {
	{ .compatible = "qcom,pil-mba-mem" },
	{}
};

static struct platform_driver pil_mba_mem_driver = {
	.probe = pil_mba_mem_driver_probe,
	.driver = {
		.name = "pil-mba-mem",
		.of_match_table = mba_mem_match_table,
		.owner = THIS_MODULE,
	},
};

static struct of_device_id mss_match_table[] = {
	{ .compatible = "qcom,pil-q6v5-mss" },
	{ .compatible = "qcom,pil-q6v55-mss" },
	{ .compatible = "qcom,pil-q6v56-mss" },
	{}
};

static struct platform_driver pil_mss_driver = {
	.probe = pil_mss_driver_probe,
	.remove = pil_mss_driver_exit,
	.driver = {
		.name = "pil-q6v5-mss",
		.of_match_table = mss_match_table,
		.owner = THIS_MODULE,
	},
};

static int __init pil_mss_init(void)
{
	int ret;

	ret = platform_driver_register(&pil_mba_mem_driver);
	if (!ret)
		ret = platform_driver_register(&pil_mss_driver);
	return ret;
}
module_init(pil_mss_init);

static void __exit pil_mss_exit(void)
{
	platform_driver_unregister(&pil_mss_driver);
}
module_exit(pil_mss_exit);

MODULE_DESCRIPTION("Support for booting modem subsystems with QDSP6v5 Hexagon processors");
MODULE_LICENSE("GPL v2");
