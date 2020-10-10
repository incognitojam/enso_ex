/* homebrew enabler
 *
 * Copyright (C) 2017 molecule, 2020 skgleba
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../hooking.h"
#include "../patches.h"

// sysstate patches
#define SBLAUTHMGR_OFFSET_PATCH_ARG (168)
#define SYSSTATE_IS_MANUFACTURING_MODE_OFFSET (0x1500)
#define SYSSTATE_IS_DEV_MODE_OFFSET (0xE28)
#define SYSSTATE_RET_CHECK_BUG (0xD92)
#define SYSSTATE_SD0_STRING_365 (0x2448)
#define SYSSTATE_SD0_PSP2CONFIG_STRING_365 (0x2396)
#define SYSSTATE_SD0_STRING_360 (0x2460)
#define SYSSTATE_SD0_PSP2CONFIG_STRING_360 (0x23AE)
#define SYSSTATE_FINAL_CALL (0x130)
#define SYSSTATE_FINAL (0x18C9)

static const uint8_t sysstate_ret_patch[] = {0x13, 0x22, 0xc8, 0xf2, 0x01, 0x02};

static const char ur0_path[] = "ur0:";
static const char ur0_psp2config_path[] = "ur0:tai/boot_config.txt";

static const char ux0_path[] = "ux0:";
static const char ux0_psp2config_path[] = "ux0:eex/boot_config.txt";

// sigpatch globals
static int g_sigpatch_disabled = 0;
static int g_homebrew_decrypt = 0;
static int (*sbl_parse_header)(uint32_t ctx, const void *header, int len, void *args) = NULL;
static int (*sbl_set_up_buffer)(uint32_t ctx, int segidx) = NULL;
static int (*sbl_decrypt)(uint32_t ctx, void *buf, int sz) = NULL;

// sysstate final function
static void __attribute__((noreturn)) (*sysstate_final)(void) = NULL;

static int is_safe_mode(kbl_param_struct *kblparam) {
    uint32_t v;
    if (kblparam->debug_flags[7] != 0xFF) {
        return 1;
    }
    v = kblparam->boot_type_indicator_2 & 0x7F;
    if (v == 0xB || (v == 4 && kblparam->resume_context_addr)) {
        v = ~kblparam->field_CC;
        if (((v >> 8) & 0x54) == 0x54 && (v & 0xC0) == 0) {
            return 1;
        } else {
            return 0;
        }
    } else if (v == 4) {
        return 0;
    }
    if (v == 0x1F || (uint32_t)(v - 0x18) <= 1) {
        return 1;
    } else {
        return 0;
    }
}

static int is_update_mode(kbl_param_struct *kblparam) {
    if (kblparam->debug_flags[4] != 0xFF) {
        return 1;
    } else {
        return 0;
    }
}

static inline int skip_patches(kbl_param_struct *kblparam) {
    return is_safe_mode(kblparam) || is_update_mode(kblparam);
}

// sigpatches for bootup
static int sbl_parse_header_patched(uint32_t ctx, const void *header, int len, void *args) {
    int ret = sbl_parse_header(ctx, header, len, args);
    if (unlikely(!g_sigpatch_disabled)) {
        DACR_OFF(
            g_homebrew_decrypt = (ret < 0);
        );
        if (g_homebrew_decrypt) {
            *(uint32_t *)(args + SBLAUTHMGR_OFFSET_PATCH_ARG) = 0x40;
            ret = 0;
        }
    }
    return ret;
}

static int sbl_set_up_buffer_patched(uint32_t ctx, int segidx) {
    if (unlikely(!g_sigpatch_disabled)) {
        if (g_homebrew_decrypt) {
            return 2; // always compressed!
        }
    }
    return sbl_set_up_buffer(ctx, segidx);
}

static int sbl_decrypt_patched(uint32_t ctx, void *buf, int sz) {
    if (unlikely(!g_sigpatch_disabled)) {
        if (g_homebrew_decrypt) {
            return 0;
        }
    }
    return sbl_decrypt(ctx, buf, sz);
}

static void __attribute__((noreturn)) sysstate_final_hook(void) {

    DACR_OFF(
        g_sigpatch_disabled = 1;
    );

    sysstate_final();
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(uint32_t argc, void *args) {
	
	patch_args_struct *patch_args = args;
	
	int nkblfw = (*(uint32_t *)(patch_args->kbl_param + 0x4) == 0x03650000);
	
	void *(*memcpy)(void *dst, const void *src, int sz) = (nkblfw) ? memcpy_365 : memcpy_360;
	void *(*get_obj_for_uid)(int uid) = (nkblfw) ? get_obj_for_uid_365 : get_obj_for_uid_360;
	
    SceObject *obj;
    SceModuleObject *mod;
	
    obj = get_obj_for_uid(patch_args->uids_a[9]);
    if (obj != NULL) {
		mod = (SceModuleObject *)&obj->data;
		HOOK_EXPORT(sbl_parse_header, 0x7ABF5135, 0xF3411881);
		HOOK_EXPORT(sbl_set_up_buffer, 0x7ABF5135, 0x89CCDA2C);
		HOOK_EXPORT(sbl_decrypt, 0x7ABF5135, 0xBC422443);
    }
	
    obj = get_obj_for_uid(patch_args->uids_b[14]);
    if (obj != NULL) {
		mod = (SceModuleObject *)&obj->data;
		int goodfw = (*(uint8_t *)(mod->segments[0].buf + 10) == 0xDA); // check if its 3.65, thats because kernel FW can be different than bl FW
		DACR_OFF(
			INSTALL_RET_THUMB(mod->segments[0].buf + SYSSTATE_IS_MANUFACTURING_MODE_OFFSET, 1);
			*(uint32_t *)(mod->segments[0].buf + SYSSTATE_IS_DEV_MODE_OFFSET) = 0x20012001;
			memcpy(mod->segments[0].buf + SYSSTATE_RET_CHECK_BUG, sysstate_ret_patch, sizeof(sysstate_ret_patch));
			uint32_t sd0stroff = (goodfw) ? SYSSTATE_SD0_STRING_365 : SYSSTATE_SD0_STRING_360;
			uint32_t sdcfgstroff = (goodfw) ? SYSSTATE_SD0_PSP2CONFIG_STRING_365 : SYSSTATE_SD0_PSP2CONFIG_STRING_360;
			if (CTRL_BUTTON_HELD(patch_args->ctrldata, E2X_USE_BBCONFIG)) {
				memcpy(mod->segments[0].buf + sd0stroff, ux0_path, sizeof(ux0_path));
				memcpy(mod->segments[0].buf + sdcfgstroff, ux0_psp2config_path, sizeof(ux0_psp2config_path));
			} else if (!skip_patches(patch_args->kbl_param)) {
				memcpy(mod->segments[0].buf + sd0stroff, ur0_path, sizeof(ur0_path));
				memcpy(mod->segments[0].buf + sdcfgstroff, ur0_psp2config_path, sizeof(ur0_psp2config_path));
			}
			// this patch actually corrupts two words of data, but they are only used in debug printing and seem to be fine
			INSTALL_HOOK_THUMB(sysstate_final_hook, mod->segments[0].buf + SYSSTATE_FINAL_CALL);
			sysstate_final = mod->segments[0].buf + SYSSTATE_FINAL;
		);
    }
	
	return 0;
}

void _stop() __attribute__ ((weak, alias ("module_stop")));
int module_stop(void) {
	return 0;
}