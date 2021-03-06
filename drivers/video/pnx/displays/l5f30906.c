/*
 * linux/drivers/video/pnx/displays/l5f30906.c
 *
 * l5f30906 LCD driver
 * Copyright (c) ST-Ericsson 2009
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <video/pnx/lcdbus.h>
#include <video/pnx/lcdctrl.h>
#include <mach/pnxfb.h>
#include <mach/gpio.h>
#include <linux/dma-mapping.h>
#include <asm/uaccess.h>

#include "l5f30906.h"

/*
 * TODO Tunning for sleep/deep sleep mode
 * TODO x panning has not been tested (but problably less important
 *      than y panning)
 *
 * TODO Manage all ACTIVATE mode in l5f30906_set_par
 *
 * */

/* Specific LCD and FB configuration
 * explicit_refresh: 1 to activate the explicit refresh (based on panning calls,
 *                  usefull if the mmi uses always double buffering).
 *
 * boot_power_mode: FB_BLANK_UNBLANK = DISPLAY ON
 *                  FB_BLANK_VSYNC_SUSPEND = DISPLAY STANDBY (OR SLEEP)
 *                  FB_BLANK_HSYNC_SUSPEND = DISPLAY SUSPEND (OR DEEP STANDBY)
 *                  FB_BLANK_POWERDOWN = DISPLAY OFF
 *                  FB_BLANK_NORMAL = not used for the moment
 * */
static struct lcdfb_specific_config l5f30906_specific_config = {
	.explicit_refresh = 1,
	.boot_power_mode = FB_BLANK_UNBLANK,
};


/* Splash screen management
 * note: The splash screen could be png files or raw files */
#ifdef CONFIG_FB_LCDBUS_L5F30906_KERNEL_SPLASH_SCREEN
#include "l5f30906_splash.h"
static struct lcdfb_splash_info l5f30906_splash_info = {
	.images      = 1,    /* How many images */
	.loop        = 0,    /* 1 for animation loop, 0 for no animation */
	.speed_ms    = 0,    /* Animation speed in ms */
	.data        = l5f30906_splash_data, /* Image data, NULL for nothing */
	.data_size   = sizeof(l5f30906_splash_data),
};
#else
/* No animation parameters */
static struct lcdfb_splash_info l5f30906_splash_info = {
	.images      = 0,    /* How many images */
	.loop        = 0,    /* 1 for animation loop, 0 for no animation */
	.speed_ms    = 0,    /* Animation speed in ms */
	.data        = NULL, /* Image data, NULL for nothing */
	.data_size   = 0,
};
#endif

/* FB_FIX_SCREENINFO (see fb.h) */
static struct fb_fix_screeninfo l5f30906_fix = {
	.id          = L5F30906_NAME,
	.type        = FB_TYPE_PACKED_PIXELS,
	.visual      = FB_VISUAL_TRUECOLOR,
	.xpanstep    = 0,
#if (L5F30906_SCREEN_BUFFERS == 0)
	.ypanstep    = 0, /* no panning */
#else
	.ypanstep    = 1, /* y panning available */
#endif
	.ywrapstep   = 0,
	.accel       = FB_ACCEL_NONE,
	.line_length = L5F30906_SCREEN_WIDTH * (L5F30906_FB_BPP/8),
};

/* FB_VAR_SCREENINFO (see fb.h) */
static struct fb_var_screeninfo l5f30906_var = {
	.xres           = L5F30906_SCREEN_WIDTH,
	.yres           = L5F30906_SCREEN_HEIGHT,
	.xres_virtual   = L5F30906_SCREEN_WIDTH,
	.yres_virtual   = L5F30906_SCREEN_HEIGHT * (L5F30906_SCREEN_BUFFERS + 1),
	.xoffset        = 0,
	.yoffset        = 0,
	.bits_per_pixel = L5F30906_FB_BPP,

#if (L5F30906_FB_BPP == 32)
	.red            = {16, 8, 0},
	.green          = {8, 8, 0},
	.blue           = {0, 8, 0},

#elif (L5F30906_FB_BPP == 24)
	.red            = {16, 8, 0},
	.green          = {8, 8, 0},
	.blue           = {0, 8, 0},

#elif (L5F30906_FB_BPP == 16)
	.red            = {11, 5, 0},
	.green          = {5, 6, 0},
	.blue           = {0, 5, 0},

#else
	#error "Unsupported color depth (see driver doc)"
#endif

	.vmode          = FB_VMODE_NONINTERLACED,
	.height         = 69,
	.width          = 41,
	.rotate         = FB_ROTATE_UR,
};


/* Hw LCD timings */
static struct lcdbus_timing l5f30906_timing = {
	/* bus */
	.mode = 0, /* 16 bits // interface */
	.ser  = 0, /* serial mode is not used */
	.hold = 0, /* VDE_CONF_HOLD_A0_FOR_COMMAND */
	/* read */
	.rh = 7,
	.rc = 4,
	.rs = 0,
	/* write */
	.wh = 1,
	.wc = 2,
	.ws = 1,
	/* misc */
	.ch = 0,
	.cs = 0,
};

/* ----------------------------------------------------------------------- */

/* L5F30906 driver data */
/*
 * @dev :
 * @fb  :
 * @bus :
 * @cmds_list: commands list virtual address
 * @curr_cmd : current command pointer
 * @last_cmd : last command pointer
 * @lock :
 * @osd0 :
 * @osd1 :
 * @power_mode :
 * @zoom_mode  :
 */
struct l5f30906_drvdata {

	struct lcdfb_device fb;

	struct lcdbus_cmd *cmds_list;
	u32    cmds_list_phys;
	u32    cmds_list_max_size;
	struct lcdbus_cmd *curr_cmd;
	struct lcdbus_cmd *last_cmd;

	u16 power_mode;
	u16 zoom_mode;
};

/*
 =========================================================================
 =                                                                       =
 =          SYSFS  section                                               =
 =                                                                       =
 =========================================================================
*/
/* l5f30906_show_explicit_refresh
 *
 */
static ssize_t
l5f30906_show_explicit_refresh(struct device *device,
		struct device_attribute *attr,
		char *buf)
{
	return sprintf(buf, "%d\n", l5f30906_specific_config.explicit_refresh);
}


/* l5f30906_store_explicit_refresh
 *
 */
static ssize_t
l5f30906_store_explicit_refresh(struct device *device,
		struct device_attribute *attr,
		const char *buf,
		size_t count)
{
	if (strncasecmp(buf, "0", count - 1) == 0) {
		l5f30906_specific_config.explicit_refresh = 0;
	}
	else if (strncasecmp(buf, "1", count - 1) == 0) {
		l5f30906_specific_config.explicit_refresh = 1;
	}

	return count;
}


static struct device_attribute l5f30906_device_attrs[] = {
	__ATTR(explicit_refresh, S_IRUGO|S_IWUSR, l5f30906_show_explicit_refresh,
			l5f30906_store_explicit_refresh)
};

/*
 =========================================================================
 =                                                                       =
 =              Helper functions                                         =
 =                                                                       =
 =========================================================================
*/

static void l5f30906_set_cols_rows(struct device *dev,
					struct list_head *cmds,
					u16 x_start, u16 x_end,
					u16 y_start, u16 y_end);

/**
 * l5f30906_set_bus_config - Sets the bus (VDE) config
 *
 * Configure the VDE colors format according to the LCD
 * colors formats (conversion)
 */
static int
l5f30906_set_bus_config(struct device *dev)
{
	struct lcdbus_conf busconf;
	int ret = 0;

	/* Tearing management */
	busconf.eofi_del  = 0;
	busconf.eofi_skip = 0;
	busconf.eofi_pol  = 0;
	busconf.eofi_use_vsync = 1;

	/* CSKIP & BSWAP params */
	busconf.cskip = 0;
	busconf.bswap = 0;

	/* Align paremeter */
	busconf.align = 0;

	/* Data & cmd params */
	busconf.cmd_ifmt  = LCDBUS_INPUT_DATAFMT_TRANSP;
	busconf.cmd_ofmt  = LCDBUS_OUTPUT_DATAFMT_TRANSP_8_BITS;

	switch (l5f30906_var.bits_per_pixel) {

	// Case 1: 16BPP
	case 16:
		busconf.data_ifmt = LCDBUS_INPUT_DATAFMT_RGB565;
		busconf.data_ofmt = LCDBUS_OUTPUT_DATAFMT_RGB565;
		break;

	// Case 2: 24BPP
	case 24:
		busconf.data_ifmt  = LCDBUS_INPUT_DATAFMT_TRANSP;
		busconf.data_ofmt  = LCDBUS_OUTPUT_DATAFMT_TRANSP_8_BITS;
		break;

	// Case 3: 32BPP
	case 32:
		busconf.data_ifmt = LCDBUS_INPUT_DATAFMT_RGB888;
		busconf.data_ofmt = LCDBUS_OUTPUT_DATAFMT_RGB565;
		break;

	default:
		dev_err(dev, "(Invalid color depth value %d "
					"(Supported color depth => 16, 24 or 23))\n",
					l5f30906_var.bits_per_pixel);
		ret = -EINVAL;
		break;
	}

	/* Set the bus config */
	if (ret == 0) {
		struct lcdctrl_device *ldev = to_lcdctrl_device(dev);
		struct lcdbus_ops *bus = ldev->ops;

		ret = bus->set_conf(dev, &busconf);
	}

	return ret;
}

/**
 * l5f30906_set_gpio_config - Sets the gpio config
 *
 */
static int
l5f30906_set_gpio_config(struct device *dev)
{
	int ret=0;

	/* EOFI pin is connected to the GPIOB7 */
	gpio_request(GPIO_B7, (char*)dev->init_name);
	pnx_gpio_set_mode(GPIO_B7, GPIO_MODE_MUX0);
	gpio_direction_input(GPIO_B7);
	// TODO: Check the Pull Up/Down mode

	/* Return the error code */
	return ret;
}


/**
 * l5f30906_check_cmd_list_overflow
 *
 */
#define l5f30906_check_cmd_list_overflow()                                      \
    if (drvdata->curr_cmd > drvdata->last_cmd) {                               \
        printk(KERN_ERR "%s(***********************************************\n",\
                __FUNCTION__);                                                 \
        printk(KERN_ERR "%s(* Too many cmds, please try to:      \n",          \
                __FUNCTION__);                                                 \
        printk(KERN_ERR "%s(* 1- increase LCDBUS_CMDS_LIST_LENGTH (%d) \n",    \
                __FUNCTION__, LCDBUS_CMDS_LIST_LENGTH);                        \
        printk(KERN_ERR "%s(* 2- Split the commands list\n",                   \
                __FUNCTION__);                                                 \
        printk(KERN_ERR "%s(***********************************************\n",\
                __FUNCTION__);                                                 \
        /* Return the error code */                                            \
        return -ENOMEM;                                                        \
    }


/**
 * l5f30906_free_cmd_list
 * @dev: device which has been used to call this function
 * @cmds: a LLI list of lcdbus_cmd's
 *
 * This function removes and free's all lcdbus_cmd's from cmds.
 */
static void
l5f30906_free_cmd_list(struct device *dev, struct list_head *cmds)
{
	struct lcdbus_cmd *cmd, *tmp;
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev);

	pr_debug("%s()\n", __FUNCTION__);

	list_for_each_entry_safe(cmd, tmp, cmds, link) {
		list_del(&cmd->link);
	}

	/* Reset the first command pointer (position) */
	drvdata->curr_cmd = drvdata->cmds_list;
}

/**
 * l5f30906_execute_cmds - execute the internal command chain
 * @dev: device which has been used to call this function
 * @cmds: a LLI list of lcdbus_cmd's
 * @delay: wait duration is msecs
 *
 * This function executes the given commands list by sending it
 * to the lcdbus layer. Afterwards it cleans the given command list.
 * This function waits the given amount of msecs after sending the
 * commands list
 */
static int
l5f30906_execute_cmds(struct device *dev, struct list_head *cmds, u16 delay)
{
	struct lcdctrl_device *ldev = to_lcdctrl_device(dev);
	struct lcdbus_ops *bus = ldev->ops;
	int ret;

	pr_debug("%s()\n", __FUNCTION__);

	/* Write data to the bus */
	ret = bus->write(dev, cmds);
	l5f30906_free_cmd_list(dev, cmds);

	/* Need to wait ? */
	if (delay != 0)
		mdelay(delay);

	/* Return the error code */
	return ret;
}

/**
 * l5f30906_add_ctrl_cmd
 * @dev: device which has been used to call this function
 * @cmds: commandes list
 * @reg: the register that is addressed
 * @reg_size: the register data size
 * @reg_data: the register data value
 *
 * This function adds the given command to the internal command chain
 * of the LCD driver.
 */
static int
l5f30906_add_ctrl_cmd(struct device *dev,
		struct list_head *cmds,
		u16 reg,
		u16 reg_size,
		u8 *reg_data)
{
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev);

	pr_debug("%s()\n", __FUNCTION__);

	l5f30906_check_cmd_list_overflow();

	drvdata->curr_cmd->type = LCDBUS_CMDTYPE_CMD;
	drvdata->curr_cmd->cmd  = reg;
	drvdata->curr_cmd->len  = reg_size;
	drvdata->curr_cmd->data_phys = drvdata->curr_cmd->data_int_phys;

	if ((reg_data) && (reg_size))
		memcpy(&drvdata->curr_cmd->data_int, reg_data, reg_size);

	list_add_tail(&drvdata->curr_cmd->link, cmds);
	drvdata->curr_cmd ++; /* Next command */

	/* Return the error code */
	return 0;
}

/**
 * l5f30906_add_data_cmd - add a request to the internal command chain
 * @dev: device which has been used to call this function
 * @cmds: commandes list
 * @data: the data to send
 * @len: the length of the data to send
 *
 * This function adds the given command and its assigned data to the internal
 * command chain of the LCD driver. Note that this function can only be used
 * for write cmds and that the data buffer must retain intact until
 * transfer is finished.
 */
static inline int
l5f30906_add_data_cmd(struct device *dev,
		struct list_head *cmds,
		struct lcdfb_transfer *transfer)
{
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev);

	pr_debug("%s()\n", __FUNCTION__);

	l5f30906_check_cmd_list_overflow();

	drvdata->curr_cmd->type = LCDBUS_CMDTYPE_DATA;
	drvdata->curr_cmd->cmd  = L5F30906_RAMWR_REG;
	drvdata->curr_cmd->w = transfer->w;
	drvdata->curr_cmd->h = transfer->h;
	drvdata->curr_cmd->bytes_per_pixel = l5f30906_var.bits_per_pixel >> 3;
	drvdata->curr_cmd->stride = l5f30906_fix.line_length;
	drvdata->curr_cmd->data_phys = transfer->addr_phys +
			transfer->x * drvdata->curr_cmd->bytes_per_pixel +
			transfer->y * drvdata->curr_cmd->stride;

	list_add_tail(&drvdata->curr_cmd->link, cmds);
	drvdata->curr_cmd ++; /* Next command */

	/* Return the error code */
	return 0;
}

/**
 * l5f30906_add_transfer_cmd - adds lcdbus_cmd's from a transfer to list
 * @dev: device which has been used to call this function
 * @cmds: a LLI list of lcdbus_cmd's
 * @transfer: the lcdfb_transfer to be converted
 *
 * This function creates a command list for a given transfer. Therefore it
 * selects the x- and y-start position in the display ram, issues the write-ram
 * command and sends the data.
 */
static int
l5f30906_add_transfer_cmd(struct device *dev,
	                     struct list_head *cmds,
	                     struct lcdfb_transfer *transfer)
{
	int ret = 0;
	u16 x_start, x_end, y_start, y_end;

	pr_debug("%s()\n", __FUNCTION__);

	x_start = transfer->x;
	x_end = x_start + transfer->w - 1;

	y_start = transfer->y;
	y_end = y_start + transfer->h - 1;

	/* Configure display window (columns/rows) */
	l5f30906_set_cols_rows(dev, cmds, x_start, x_end, y_start, y_end);

	/* Prepare Data */
	ret = l5f30906_add_data_cmd(dev, cmds, transfer);

	/* Return the error code */
	return ret;
}


/**
 * @brief
 *
 * @param dev
 * @param cmds
 * @param x_start
 * @param x_end
 * @param y_start
 * @param y_end
 */
/*
 * FB_ROTATE_UR   0 - normal orientation (0 degree)
 * FB_ROTATE_CW   1 - clockwise orientation (90 degrees)
 * FB_ROTATE_UD   2 - upside down orientation (180 degrees)
 * FB_ROTATE_CCW  3 - counterclockwise orientation (270 degrees)
 *
 **/
static void
l5f30906_set_cols_rows(struct device *dev,
					struct list_head *cmds,
					u16 x_start, u16 x_end,
					u16 y_start, u16 y_end)
{
	u8 vl_CmdParam[L5F30906_REG_SIZE_MAX];

    /* Colomn start and end */
	vl_CmdParam[0] = L5F30906_HIBYTE(x_start);
	vl_CmdParam[1] = L5F30906_LOBYTE(x_start);
	vl_CmdParam[2] = L5F30906_HIBYTE(x_end);
	vl_CmdParam[3] = L5F30906_LOBYTE(x_end);

	switch(l5f30906_var.rotate) {
	case FB_ROTATE_CW:   /* 90  */
	case FB_ROTATE_CCW:  /* 270 */
		l5f30906_add_ctrl_cmd(dev, cmds,
			L5F30906_PASET_REG,
			L5F30906_PASET_SIZE,
			vl_CmdParam);
		break;

	default: /* 0 or 180 */
		l5f30906_add_ctrl_cmd(dev, cmds,
			L5F30906_CASET_REG,
			L5F30906_CASET_SIZE,
			vl_CmdParam);
		break;
	}

	/* Row start and end */
	vl_CmdParam[0] = L5F30906_HIBYTE(y_start);
	vl_CmdParam[1] = L5F30906_LOBYTE(y_start);
	vl_CmdParam[2] = L5F30906_HIBYTE(y_end);
	vl_CmdParam[3] = L5F30906_LOBYTE(y_end);

	switch(l5f30906_var.rotate) {
	case FB_ROTATE_CW:   /* 90  */
	case FB_ROTATE_CCW:  /* 270 */
		l5f30906_add_ctrl_cmd(dev, cmds,
			L5F30906_CASET_REG,
			L5F30906_CASET_SIZE,
			vl_CmdParam);
		break;

	default: /* 0 or 180 */
		l5f30906_add_ctrl_cmd(dev, cmds,
			L5F30906_PASET_REG,
			L5F30906_PASET_SIZE,
			vl_CmdParam);
		break;
	}
}


/*
 * l5f30906_bootstrap - execute the bootstrap commands list
 * @dev: device which has been used to call this function
 *
 */
static int
l5f30906_bootstrap(struct device *dev)
{
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev);
	int ret = 0;
	struct list_head cmds;
	u16 x_start, x_end, y_start, y_end;
	u8 vl_CmdParam[L5F30906_REG_SIZE_MAX];

	pr_debug("%s()\n", __FUNCTION__);

	/* ------------------------------------------------------------------------
	 * Initialize the LCD HW
	 * --------------------------------------------------------------------- */
	INIT_LIST_HEAD(&cmds);

	/* Access Level 2 command */
	vl_CmdParam[0] = 0x5A;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_EXTCMMOD1_REG,
			L5F30906_EXTCMMOD1_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Access Level 2 command */
	vl_CmdParam[0] = 0x5A;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_EXTCMMOD2_REG,
			L5F30906_EXTCMMOD2_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set Timings for Display */
	vl_CmdParam[0] = 0x23;
	vl_CmdParam[1] = 0x23;
	vl_CmdParam[2] = 0x23;
	vl_CmdParam[3] = 0x23;
	vl_CmdParam[4] = 0x2A;
	vl_CmdParam[5] = 0x2A;
	vl_CmdParam[6] = 0x2A;
	vl_CmdParam[7] = 0x2A;
	vl_CmdParam[8] = 0x26;
	vl_CmdParam[9] = 0x26;
	vl_CmdParam[10] = 0x26;
	vl_CmdParam[11] = 0x26;
	vl_CmdParam[12] = 0xB2;
	vl_CmdParam[13] = 0xBF;
	vl_CmdParam[14] = 0x55;
	vl_CmdParam[15] = 0x02;
	vl_CmdParam[16] = 0x01;
	vl_CmdParam[17] = 0x0C;
	vl_CmdParam[18] = 0xCC;
	vl_CmdParam[19] = 0xCC;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_DISCTL_REG,
			L5F30906_DISCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set Output Voltages */
	vl_CmdParam[0] = 0x42;
	vl_CmdParam[1] = 0x32;
	vl_CmdParam[2] = 0x45;
	vl_CmdParam[3] = 0x32;
	vl_CmdParam[4] = 0x0E;
	vl_CmdParam[5] = 0x02;
	vl_CmdParam[6] = 0xCD;
	vl_CmdParam[7] = 0x02;
	vl_CmdParam[8] = 0x00;
	vl_CmdParam[9] = 0x02;
	vl_CmdParam[10] = 0x0B;
	vl_CmdParam[11] = 0x00;
	vl_CmdParam[12] = 0x00;
	vl_CmdParam[13] = 0x02;
	vl_CmdParam[14] = 0x00;
	vl_CmdParam[15] = 0x00;
	vl_CmdParam[16] = 0x02;
	vl_CmdParam[17] = 0x00;
	vl_CmdParam[18] = 0x00;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_PWRCTL_REG,
			L5F30906_PWRCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set AMP of VCI1 */
	vl_CmdParam[0] = 0x00;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_AMPCTL_REG,
			L5F30906_AMPCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* MTPCTL */
	vl_CmdParam[0] = 0x02;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_MTPCTL_REG,
			L5F30906_MTPCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* RGB Timing Settings */
	vl_CmdParam[0] = 0x00;
	vl_CmdParam[1] = 0x00;
	vl_CmdParam[2] = 0x2A;
	vl_CmdParam[3] = 0x26;
	vl_CmdParam[4] = 0x10;
	vl_CmdParam[5] = 0x08;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_RGBIF_REG,
			L5F30906_RGBIF_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);


	/* Set Color mode */
	vl_CmdParam[0] = 0x05;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_COLMOD_REG,
			L5F30906_COLMOD_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set Gamma Curve */
	vl_CmdParam[0] = 0x01;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_GAMSET_REG,
			L5F30906_GAMSET_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* GAMMSETP0 */
	vl_CmdParam[0] = 0x03;
	vl_CmdParam[1] = 0x03;
	vl_CmdParam[2] = 0x08;
	vl_CmdParam[3] = 0x1D;
	vl_CmdParam[4] = 0x27;
	vl_CmdParam[5] = 0x2D;
	vl_CmdParam[6] = 0x2C;
	vl_CmdParam[7] = 0x31;
	vl_CmdParam[8] = 0x37;
	vl_CmdParam[9] = 0x3A;
	vl_CmdParam[10] = 0x42;
	vl_CmdParam[11] = 0x4B;
	vl_CmdParam[12] = 0x35;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_GAMMSETP0_REG,
			L5F30906_GAMMSETP0_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* GAMMSETN0 */
	vl_CmdParam[0] = 0x03;
	vl_CmdParam[1] = 0x03;
	vl_CmdParam[2] = 0x08;
	vl_CmdParam[3] = 0x1D;
	vl_CmdParam[4] = 0x27;
	vl_CmdParam[5] = 0x2D;
	vl_CmdParam[6] = 0x2C;
	vl_CmdParam[7] = 0x31;
	vl_CmdParam[8] = 0x37;
	vl_CmdParam[9] = 0x3A;
	vl_CmdParam[10] = 0x42;
	vl_CmdParam[11] = 0x4B;
	vl_CmdParam[12] = 0x35;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_GAMMSETN0_REG,
			L5F30906_GAMMSETN0_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* PWMENB */
	vl_CmdParam[0] = 0x01;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_PWMENB_REG,
			L5F30906_PWMENB_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set Image Enhancement */
	vl_CmdParam[0] = 0x00;
	vl_CmdParam[1] = 0x80;
	vl_CmdParam[2] = 0x80;
	vl_CmdParam[3] = 0x49;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_MIECTL_REG,
			L5F30906_MIECTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set Image Enhancement */
	vl_CmdParam[0] = 0x26;
	vl_CmdParam[1] = 0x00;
	vl_CmdParam[2] = 0x00;
	vl_CmdParam[3] = 0x01;
	vl_CmdParam[4] = 0x8F;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_MIESIZCTL_REG,
			L5F30906_MIESIZCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set Output Voltages */
	vl_CmdParam[0] = 0x02;
	vl_CmdParam[1] = 0x00;
	vl_CmdParam[2] = 0x02;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_PWMCTL_REG,
			L5F30906_PWMCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* SSLCTL */
	vl_CmdParam[0] = 0x00;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_SSLCTL_REG,
			L5F30906_SSLCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* ADCCTL */
	vl_CmdParam[0] = 0xCC;
	vl_CmdParam[1] = 0x02;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_ADCCTL_REG,
			L5F30906_ADCCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Tearing ON */
	vl_CmdParam[0] = 0x00;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_TEON_REG,
			L5F30906_TEON_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* MADCTL */
	vl_CmdParam[0] = 0x48;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_MADCTL_REG,
			L5F30906_MADCTL_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Pre-Calculation of MADCTL & Setting of Sync Signal Polarity */
	vl_CmdParam[0] = 0x00;
	vl_CmdParam[1] = 0x00;
	vl_CmdParam[2] = 0x00;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_MADDEF_REG,
			L5F30906_MADDEF_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Configure display window (columns/rows) */
	x_start = 0;
	x_end = l5f30906_var.xres - 1;
	y_start = 0;
	y_end = l5f30906_var.yres - 1;

	l5f30906_set_cols_rows(dev, &cmds, x_start, x_end, y_start, y_end);

	/* Exit Sleep Mode */
	vl_CmdParam[0] = 0x5A;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_SLPOUT_REG,
			L5F30906_SLPOUT_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 250);

	/* DLS */
	vl_CmdParam[0] = 0x03;
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_DLS_REG,
			L5F30906_DLS_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 250);

	/* Display ON */
	l5f30906_add_ctrl_cmd(dev, &cmds,
			L5F30906_DISPON_REG,
			L5F30906_DISPON_SIZE, vl_CmdParam);
	l5f30906_execute_cmds(dev, &cmds, 0);

	/* Set power mode */
	drvdata->power_mode = FB_BLANK_UNBLANK;

	/* Return the error code */
	return ret;
}

/*
 =========================================================================
 =                                                                       =
 =              device detection and bootstrapping                       =
 =                                                                       =
 =========================================================================
*/

/**
 * l5f30906_device_supported - perform hardware detection check
 * @dev:	pointer to the device which should be checked for support
 *
 */
static int __devinit
l5f30906_device_supported(struct device *dev)
{
	int ret = 0;

	/*
	 * Hardware detection of the display does not seem to be supported
	 * by the hardware, thus we assume the display is there!
	 */
	if (strcmp(dev->bus_id, "pnx-vde-lcd0"))
		ret = -ENODEV; // (pnx-vde-lcd0 not detected (pb during VDE init)

	/* Return the error code */
	return ret;
}

/*
 =========================================================================
 =                                                                       =
 =              lcdfb_ops implementations                                =
 =                                                                       =
 =========================================================================
*/

/**
 * l5f30906_write - implementation of the write function call
 * @dev:	device which has been used to call this function
 * @transfers:	list of lcdfb_transfer's
 *
 * This function converts the list of lcdfb_transfer into a list of resulting
 * lcdbus_cmd's which then gets sent to the display controller using the
 * underlying bus driver.
 */
static int
l5f30906_write(const struct device *dev, const struct list_head *transfers)
{
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev->parent);
	struct lcdctrl_device *ldev = to_lcdctrl_device(dev->parent);
	struct lcdbus_ops *bus = ldev->ops;
	struct list_head cmds;
	struct lcdfb_transfer *transfer;
	int ret = 0;

	pr_debug("%s()\n", __FUNCTION__);

	if (drvdata->power_mode != FB_BLANK_UNBLANK) {
		dev_warn((struct device *)dev,
				"NOT allowed (refresh while power off)\n");
		return 0;
	}

	if (list_empty(transfers)) {
		dev_warn((struct device *)dev,
				"Got an empty transfer list\n");
		return 0;
	}

	/* now get on with the real stuff */
	INIT_LIST_HEAD(&cmds);

	list_for_each_entry(transfer, transfers, link) {
		ret |= l5f30906_add_transfer_cmd(dev->parent, &cmds, transfer);
	}

	if (ret >= 0) {
		/* execute the cmd list we build */
		ret = bus->write(dev->parent, &cmds);
	}

	/* Now buffers may be freed */
	l5f30906_free_cmd_list(dev->parent, &cmds);

	/* Return the error code */
	return ret;
}

/**
 * l5f30906_get_fscreeninfo - copies the fix screeninfo into fsi
 * @dev: device which has been used to call this function
 * @fsi: structure to which the fix screeninfo should be copied
 *
 * Get the fixed information of the screen.
 */
static int
l5f30906_get_fscreeninfo(const struct device *dev,
		struct fb_fix_screeninfo *fsi)
{
	pr_debug("%s()\n", __FUNCTION__);

	if (!fsi) {
		return -EINVAL;
	}

	*fsi = l5f30906_fix;

	/* Return the error code */
	return 0;
}

/**
 * l5f30906_get_vscreeninfo - copies the var screeninfo into vsi
 * @dev: device which has been used to call this function
 * @vsi: structure to which the var screeninfo should be copied
 *
 * Get the variable screen information.
 */
static int
l5f30906_get_vscreeninfo(const struct device *dev,
		struct fb_var_screeninfo *vsi)
{
	pr_debug("%s()\n", __FUNCTION__);

	if (!vsi) {
		return -EINVAL;
	}

	*vsi = l5f30906_var;

	/* Return the error code */
	return 0;
}


/**
 * l5f30906_display_on - execute "Display On" sequence
 * @dev: device which has been used to call this function
 *
 * This function switches the display on.
 */
static int
l5f30906_display_on(struct device *dev)
{
	int ret = 0;
	struct device *devp = dev->parent;
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(devp);

	/* Switch on the display if needed */
	if (drvdata->power_mode != FB_BLANK_UNBLANK) {
		struct list_head cmds;
		u8 vl_CmdParam[L5F30906_REG_SIZE_MAX];

		INIT_LIST_HEAD(&cmds);

		/* Exit Sleep Mode */
		vl_CmdParam[0] = 0x5A;
		l5f30906_add_ctrl_cmd(devp, &cmds,
				L5F30906_SLPOUT_REG,
				L5F30906_SLPOUT_SIZE, vl_CmdParam);
		l5f30906_execute_cmds(devp, &cmds, 250);

		/* DLS */
		vl_CmdParam[0] = 0x03;
		l5f30906_add_ctrl_cmd(devp, &cmds,
				L5F30906_DLS_REG,
				L5F30906_DLS_SIZE, vl_CmdParam);
		l5f30906_execute_cmds(devp, &cmds, 250);

		/* Display ON */
		l5f30906_add_ctrl_cmd(devp, &cmds,
				L5F30906_DISPON_REG,
				L5F30906_DISPON_SIZE, vl_CmdParam);
		l5f30906_execute_cmds(devp, &cmds, 0);

		/* Set power mode */
		drvdata->power_mode = FB_BLANK_UNBLANK;
	}
	else {
		dev_err(dev, "Display already in FB_BLANK_UNBLANK mode\n");
		ret = -EPERM; /* Operation not permitted */
	}

	/* Return the error code */
	return ret;
}

/**
 * l5f30906_display_off - execute "Display Off" sequence
 * @dev: device which has been used to call this function
 *
 * This function switches the display off.
 */
static int
l5f30906_display_off(struct device *dev)
{
	int ret = 0;
	struct device *devp = dev->parent;
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(devp);

	pr_debug("%s()\n", __FUNCTION__);

	/* Switch off the display if needed */
	if (drvdata->power_mode != FB_BLANK_POWERDOWN) {
		struct list_head cmds;
		u8 vl_CmdParam[L5F30906_REG_SIZE_MAX];

		INIT_LIST_HEAD(&cmds);

		/* Display OFF */
		l5f30906_add_ctrl_cmd(devp, &cmds,
				L5F30906_DISPOFF_REG,
				L5F30906_DISPOFF_SIZE, vl_CmdParam);
		l5f30906_execute_cmds(devp, &cmds, 45);

		/* DLS */
		vl_CmdParam[0] = 0x00;
		l5f30906_add_ctrl_cmd(devp, &cmds,
				L5F30906_DLS_REG,
				L5F30906_DLS_SIZE, vl_CmdParam);
		l5f30906_execute_cmds(devp, &cmds, 0);

		/* Enter Sleep Mode */
		l5f30906_add_ctrl_cmd(devp, &cmds,
				L5F30906_SLPIN_REG,
				L5F30906_SLPIN_SIZE, vl_CmdParam);
		l5f30906_execute_cmds(devp, &cmds, 250);

		/* Set power mode */
		drvdata->power_mode = FB_BLANK_POWERDOWN;
	}
	else {
		dev_err(dev, "Display already in FB_BLANK_POWERDOWN mode\n");
		ret = -EPERM; /* Operation not permitted */
	}

	/* Return the error code */
	return ret;
}


/**
 * l5f30906_display_standby - enter standby mode
 * @dev: device which has been used to call this function
 *
 * This function switches the display from normal mode
 * to standby mode.
 */
static int
l5f30906_display_standby(struct device *dev)
{
	int ret = 0;
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev->parent);

	pr_debug("%s()\n", __FUNCTION__);

	if (drvdata->power_mode != FB_BLANK_VSYNC_SUSPEND) {

		/* Set power mode */
		drvdata->power_mode = FB_BLANK_VSYNC_SUSPEND;
	}
	else {
		dev_err(dev, "Display already in FB_BLANK_VSYNC_SUSPEND mode\n");
		ret = -EPERM; /* Operation not permitted */
	}

	/* Return the error code */
	return  ret;
}


/**
 * l5f30906_display_deep_standby - enter deep standby mode
 * @dev: device which has been used to call this function
 *
 * This function switches the display from standby mode to
 * deep standby mode.
 */
static int
l5f30906_display_deep_standby(struct device *dev)
{
	int ret = 0;
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev->parent);

	pr_debug("%s()\n", __FUNCTION__);

	if (drvdata->power_mode != FB_BLANK_VSYNC_SUSPEND) {

		/* Set power mode */
		drvdata->power_mode = FB_BLANK_VSYNC_SUSPEND;
	}
	else {
		dev_err(dev, "Display already in FB_BLANK_HSYNC_SUSPEND mode\n");
		ret = -EPERM; /* Operation not permitted */
	}

	/* Return the error code */
	return  ret;
}

/**
 * l5f30906_get_splash_info - copies the splash screen info into si
 * @dev: device which has been used to call this function
 * @si:  structure to which the splash screen info should be copied
 *
 * Get the splash screen info.
 * */
static int
l5f30906_get_splash_info(struct device *dev,
		struct lcdfb_splash_info *si)
{
	pr_debug("%s()\n", __FUNCTION__);

	if (!si) {
		return -EINVAL;
	}
	
	*si = l5f30906_splash_info;

	/* Return the error code */
	return 0;
}


/**
 * l5f30906_get_specific_config - return the explicit_refresh configuration
 * @dev:	device which has been used to call this function
 *
 * */
static int
l5f30906_get_specific_config(struct device *dev,
		struct lcdfb_specific_config **sc)
{
	pr_debug("%s()\n", __FUNCTION__);

	if (!sc) {
		return -EINVAL;
	}

	(*sc) = &l5f30906_specific_config;

	/* Return the error code */
	return 0;
}


/**
 * l5f30906_get_device_attrs
 * @dev: device which has been used to call this function
 * @device_attrs: device attributes to be returned
 * @attrs_nbr: the number of device attributes
 *
 *
 * Returns the device attributes to be added to the SYSFS
 * entries (/sys/class/graphics/fbX/....
 * */
static int
l5f30906_get_device_attrs(struct device *dev,
		struct device_attribute **device_attrs,
		unsigned int *attrs_nbr)
{
	pr_debug("%s()\n", __FUNCTION__);

	if (!device_attrs) {
		return -EINVAL;
	}

	(*device_attrs) = l5f30906_device_attrs;

	(*attrs_nbr) = sizeof(l5f30906_device_attrs)/sizeof(l5f30906_device_attrs[0]);

	/* Return the error code */
	return 0;
}


/*
 * l5f30906_check_var
 * @dev:	device which has been used to call this function
 * @vsi:	structure  var screeninfo to check
 *
 * TODO check more parameters and cross cases (virtual xyres vs. rotation,
 *      panning...
 *
 * */
static int l5f30906_check_var(const struct device *dev,
		 struct fb_var_screeninfo *vsi)
{
	int ret = -EINVAL;

	pr_debug("%s()\n", __FUNCTION__);

	if (!vsi) {
		return -EINVAL;
	}

	/* check xyres */
	if ((vsi->xres != l5f30906_var.xres) ||
		(vsi->yres != l5f30906_var.yres)) {
		ret = -EPERM;
		goto ko;
	}

	/* check xyres virtual */
	if ((vsi->xres_virtual != l5f30906_var.xres_virtual) ||
		(vsi->yres_virtual != l5f30906_var.yres_virtual)) {
		ret = -EPERM;
		goto ko;
	}

	/* check xoffset */
	if (vsi->xoffset != l5f30906_var.xoffset) {
		ret = -EPERM;
		goto ko;
	}

	/* check bpp */
	if ((vsi->bits_per_pixel != 16) &&
		(vsi->bits_per_pixel != 24) &&
		(vsi->bits_per_pixel != 32))
		goto ko;

	/* Check pixel format */
	if (vsi->nonstd) {
		/* Non-standard pixel format not supported by LCD HW */
		ret = -EPERM;
		goto ko;
	}

	/* check rotation */
/*
 * FB_ROTATE_UR      0 - normal orientation (0 degree)
 * FB_ROTATE_CW      1 - clockwise orientation (90 degrees)
 * FB_ROTATE_UD      2 - upside down orientation (180 degrees)
 * FB_ROTATE_CCW     3 - counterclockwise orientation (270 degrees)
 *
 * */
	if (vsi->rotate > FB_ROTATE_CCW)
		goto ko;

	/* Everything is ok */
	return 0;

ko:
	/* Return the error code */
	return ret;
}

/*
 * l5f30906_set_par
 * @dev: device which has been used to call this function
 * @vsi: structure  var screeninfo to set
 *
 * TODO check more parameters and cross cases (virtual xyres vs. rotation,
 *      panning...
 *
 * */
static inline void l5f30906_init_color(struct fb_bitfield *color,
		__u32 offset, __u32 length, __u32 msb_right)
{
	color->offset     = offset;
	color->length     = length;
	color->msb_right  = msb_right;
}

static int l5f30906_set_par(const struct device *dev,
		struct fb_info *info)
{
	int ret = -EINVAL;
	int set_params = 0;
	struct fb_var_screeninfo *vsi = &info->var;

	pr_debug("%s()\n", __FUNCTION__);

	if (!vsi) {
		return -EINVAL;
	}

	/* Check the activation mode (see fb.h) */
	switch(vsi->activate)
	{
	case FB_ACTIVATE_NOW:		/* set values immediately (or vbl)*/
		set_params = 1;
		break;

	case FB_ACTIVATE_NXTOPEN:	/* activate on next open */
		break;

	case FB_ACTIVATE_TEST:		/* don't set, round up impossible */
		break;

	case FB_ACTIVATE_MASK: 		/* values */
		break;

	case FB_ACTIVATE_VBL:		/* activate values on next vbl  */
		break;

	case FB_CHANGE_CMAP_VBL:	/* change colormap on vbl */
		break;

	case FB_ACTIVATE_ALL:		/* change all VCs on this fb */
		set_params = 1;
		break;

	case FB_ACTIVATE_FORCE:		/* force apply even when no change*/
		set_params = 1;
		break;

	case FB_ACTIVATE_INV_MODE:	/* invalidate videomode */
		break;
	}

	if (set_params) {
		if (! memcmp(vsi, &l5f30906_var, sizeof(struct fb_var_screeninfo))) {
			ret = 0;
			goto quit;
		}

		/* Check rotation */
		if (l5f30906_var.rotate != vsi->rotate) {
			l5f30906_var.rotate = vsi->rotate;
		}

		/* Check bpp */
		if (l5f30906_var.bits_per_pixel != vsi->bits_per_pixel) {

			u32 bytes_per_pixel;

			l5f30906_var.bits_per_pixel = vsi->bits_per_pixel;

			switch(l5f30906_var.bits_per_pixel) {
			case 16:
				l5f30906_init_color(&l5f30906_var.red,  11, 5, 0);
				l5f30906_init_color(&l5f30906_var.green, 5, 6, 0);
				l5f30906_init_color(&l5f30906_var.blue,  0, 5, 0);
				break;

			case 24:
			case 32:
				l5f30906_init_color(&l5f30906_var.red,   16, 8, 0);
				l5f30906_init_color(&l5f30906_var.green,  8, 8, 0);
				l5f30906_init_color(&l5f30906_var.blue,   0, 8, 0);
				break;
			}

			bytes_per_pixel = l5f30906_var.bits_per_pixel/8;

			/* Set the new line length (BPP or resolution changed) */
			l5f30906_fix.line_length = l5f30906_var.xres_virtual*bytes_per_pixel;

			/* Switch off the LCD HW */
			l5f30906_display_off((struct device *)dev);

			/* Switch on the LCD HW */
			ret = l5f30906_display_on((struct device *)dev);
		}

		/* TODO: Check other parameters */

		ret = 0;
	}

quit:
	/* Return the error code */
	return ret;
}


/**
 * l5f30906_ioctl
 * @dev: device which has been used to call this function
 * @cmd: requested command
 * @arg: command argument
 * */
static int
l5f30906_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned int value;
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev->parent);

	pr_debug("%s()\n", __FUNCTION__);

	switch (cmd) {

	/* */
	case PNXFB_GET_ZOOM_MODE:
		if (put_user(drvdata->zoom_mode, (int __user *)arg)) {
			ret = -EFAULT;
		}
		break;

	/* */
	case PNXFB_SET_ZOOM_MODE:
		if (get_user(value, (int __user *)arg)) {
			ret = -EFAULT;
		}
		else {
			if ((value >= PNXFB_ZOOM_MAX) || (value < PNXFB_ZOOM_NONE)) {
				ret = -EINVAL;
			}
			else {
				/* NOT allowed to change zoom mode */
				ret = -EPERM;
			}
		}
		break;

	/* */
	default:
		dev_err(dev, "Unknwon IOCTL command (%d)\n", cmd);
		ret = -ENOIOCTLCMD;
	}

	return ret;
}

/*
 * LCD operations supported by this driver
 */
struct lcdfb_ops l5f30906_ops = {
	.write           = l5f30906_write,
	.get_fscreeninfo = l5f30906_get_fscreeninfo,
	.get_vscreeninfo = l5f30906_get_vscreeninfo,
	.get_splash_info = l5f30906_get_splash_info,
	.get_specific_config = l5f30906_get_specific_config,
	.get_device_attrs= l5f30906_get_device_attrs,
	.display_on      = l5f30906_display_on,
	.display_off     = l5f30906_display_off,
	.display_standby = l5f30906_display_standby,
	.display_deep_standby = l5f30906_display_deep_standby,
	.check_var       = l5f30906_check_var,
	.set_par         = l5f30906_set_par,
	.ioctl           = l5f30906_ioctl,
};


/*
 =========================================================================
 =                                                                       =
 =              module stuff (init, exit, probe, release)                =
 =                                                                       =
 =========================================================================
*/
static int __devinit
l5f30906_probe(struct device *dev)
{
	struct l5f30906_drvdata *drvdata;
	struct lcdctrl_device *ldev = to_lcdctrl_device(dev);
	struct lcdbus_ops *bus = ldev->ops;
	u32 cmds_list_phys;
	struct lcdbus_cmd *cmd;
	int ret, i;

	pr_debug("%s()\n", __FUNCTION__);

	if (!bus || !bus->read || !bus->write || !bus->get_conf ||
	       !bus->set_conf || !bus->set_timing) {
		return -EINVAL;
	}

	/* ------------------------------------------------------------------------
	 * Hardware detection
	 * ---- */
	ret = l5f30906_device_supported(dev);
	if (ret < 0) {
		goto out;
	}

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL) {
		dev_err(dev, "No more memory (drvdata)\n");
		ret = -ENOMEM;
		goto out;
	}

	/* ------------------------------------------------------------------------
	 *  Prepare the lcd cmds list
	 * ---- */
	drvdata->cmds_list_max_size = LCDBUS_CMDS_LIST_LENGTH *
			sizeof(struct lcdbus_cmd);

	drvdata->cmds_list = (struct lcdbus_cmd *)dma_alloc_coherent(NULL,
			drvdata->cmds_list_max_size, &(drvdata->cmds_list_phys),
			GFP_KERNEL | GFP_DMA);

	if ((!drvdata->cmds_list) || (!drvdata->cmds_list_phys)) {
		printk(KERN_ERR "%s Failed ! (No more memory (drvdata->cmds_list))\n",
				__FUNCTION__);
		ret = -ENOMEM;
		goto err_free_drvdata;
	}

	/* Calculates the physical address */
	cmd = drvdata->cmds_list;
	cmds_list_phys = drvdata->cmds_list_phys;
	i = 0;
	do {
		cmd->data_int_phys = cmds_list_phys;
		i++;
		cmd++;
		cmds_list_phys += sizeof(struct lcdbus_cmd);
	} while (i < LCDBUS_CMDS_LIST_LENGTH);

	/* Set the first & last command pointer */
	drvdata->curr_cmd = drvdata->cmds_list;
	drvdata->last_cmd = cmd--;

	pr_debug("%s (cmd %p, curr %p, last %p)\n",
			__FUNCTION__, drvdata->cmds_list, drvdata->curr_cmd,
			drvdata->last_cmd);

	/* Set drvdata */
	dev_set_drvdata(dev, drvdata);

	drvdata->power_mode = l5f30906_specific_config.boot_power_mode;
	drvdata->fb.ops = &l5f30906_ops;
	drvdata->fb.dev.parent = dev;
	snprintf(drvdata->fb.dev.bus_id, BUS_ID_SIZE, "%s-fb", dev->bus_id);

	/* ------------------------------------------------------------------------
	 * GPIO configuration is specific for each LCD
	 * (Chech the HW datasheet (VDE_EOFI, VDE_EOFI_copy)
	 * ---- */
	ret = l5f30906_set_gpio_config(dev);
	if (ret < 0) {
		printk(KERN_ERR "%s Failed ! (Could not set gpio config)\n",
				__FUNCTION__);
		ret = -EBUSY;
		goto err_free_cmds_list;
	}

	/* ------------------------------------------------------------------------
	 * Set the bus timings for this display device
	 * ---- */
	bus->set_timing(dev, &l5f30906_timing);

	/* ------------------------------------------------------------------------
	 * Set the bus configuration for this display device
	 * ---- */
	ret = l5f30906_set_bus_config(dev);
	if (ret < 0) {
		printk(KERN_ERR "%s Failed ! (Could not set bus config)\n",
				__FUNCTION__);
		ret = -EBUSY;
		goto err_free_cmds_list;
	}

	/* -------------------------------------------------------------------------
	 * Initialize the LCD if the initial state is ON (FB_BLANK_UNBLANK)
	 * ---- */
	if (drvdata->power_mode == FB_BLANK_UNBLANK) {
		ret = l5f30906_bootstrap(dev);
		if (ret < 0) {
			ret = -EBUSY;
			goto err_free_cmds_list;
		}
	}

	/* ------------------------------------------------------------------------
	 * Register the lcdfb device
	 * ---- */
	ret = lcdfb_device_register(&drvdata->fb);
	if (ret < 0) {
		goto err_free_cmds_list;
	}

	/* Everything is OK, so return */
	goto out;

err_free_cmds_list:

	dma_free_coherent(NULL, drvdata->cmds_list_max_size, drvdata->cmds_list,
			(dma_addr_t)drvdata->cmds_list_phys);

err_free_drvdata:
	kfree(drvdata);

out:
	return ret;
}

static int __devexit
l5f30906_remove(struct device *dev)
{
	struct l5f30906_drvdata *drvdata = dev_get_drvdata(dev);

	pr_debug("%s()\n", __FUNCTION__);

	lcdfb_device_unregister(&drvdata->fb);

	if (drvdata->cmds_list) {
		dma_free_coherent(NULL,
				drvdata->cmds_list_max_size,
				drvdata->cmds_list,
				(dma_addr_t)drvdata->cmds_list_phys);
	}

	kfree(drvdata);

	return 0;
}

static struct device_driver l5f30906_driver = {
	.owner  = THIS_MODULE,
	.name   = L5F30906_NAME,
	.bus    = &lcdctrl_bustype,
	.probe  = l5f30906_probe,
	.remove = l5f30906_remove,
};

static int __init
l5f30906_init(void)
{
	pr_debug("%s()\n", __FUNCTION__);

	return driver_register(&l5f30906_driver);
}

static void __exit
l5f30906_exit(void)
{
	pr_debug("%s()\n", __FUNCTION__);

	driver_unregister(&l5f30906_driver);
}

module_init(l5f30906_init);
module_exit(l5f30906_exit);

MODULE_AUTHOR("Faouaz TENOUTIT, ST-Ericsson");
MODULE_DESCRIPTION("LCDBUS driver for the l5f30906 LCD");
MODULE_LICENSE("GPL");
