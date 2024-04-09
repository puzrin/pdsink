#!/bin/sh

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
EC_DIR=$SCRIPT_DIR/ec_tmp
ZEPHYR_DIR=$SCRIPT_DIR/zephyr_tmp
PATCHES_DIR=$SCRIPT_DIR/patches
SRC_DIR=$SCRIPT_DIR/../src
INCLUDE_DIR=$SCRIPT_DIR/../include
CP_OPTS=""
LIB_CONFIG=$SRC_DIR/pd_config.h
UNIFDEF_OPTS="-m -f $LIB_CONFIG"
EVAL_MACRO=$SCRIPT_DIR/patches/eval_IS_ENABLED_macro.py

[ -d $EC_DIR/.git ] || git clone --depth 1 https://chromium.googlesource.com/chromiumos/platform/ec $EC_DIR
[ -d $ZEPHYR_DIR/.git ] || git clone --depth 1 https://github.com/zephyrproject-rtos/zephyr.git $ZEPHYR_DIR

mkdir -p $SCRIPT_DIR/../src
mkdir -p $SCRIPT_DIR/../src/driver
mkdir -p $SCRIPT_DIR/../include

# Core

cp $CP_OPTS $EC_DIR/common/usbc/usb_pd_timer.c $SRC_DIR
cp $CP_OPTS $EC_DIR/include/usb_pd_timer.h $INCLUDE_DIR

cp $CP_OPTS $EC_DIR/common/usbc/usb_prl_sm.c $SRC_DIR
cp $CP_OPTS $EC_DIR/include/usb_prl_sm.h $INCLUDE_DIR

cp $CP_OPTS $EC_DIR/common/usbc/usb_pd_dpm.c $SRC_DIR

cp $CP_OPTS $EC_DIR/common/usbc/usb_pe_private.h $SRC_DIR
cp $CP_OPTS $EC_DIR/common/usbc/usb_pe_drp_sm.c $SRC_DIR

cp $CP_OPTS $EC_DIR/include/compiler.h $SRC_DIR

cp $CP_OPTS $EC_DIR/common/usbc/usb_sm.c $SRC_DIR
cp $CP_OPTS $EC_DIR/include/usb_sm.h $INCLUDE_DIR

cp $CP_OPTS $EC_DIR/include/usb_pe_sm.h $INCLUDE_DIR
cp $CP_OPTS $EC_DIR/include/usb_pd.h $INCLUDE_DIR
cp $CP_OPTS $EC_DIR/include/usb_pd_dpm_sm.h $INCLUDE_DIR
cp $CP_OPTS $EC_DIR/include/usb_pd_pdo.h $INCLUDE_DIR
cp $CP_OPTS $EC_DIR/include/usb_pd_policy.h $INCLUDE_DIR
cp $CP_OPTS $EC_DIR/include/usb_tc_sm.h $INCLUDE_DIR

# Drivers

cp $CP_OPTS $EC_DIR/driver/tcpm/fusb302.c $SRC_DIR/driver
cp $CP_OPTS $EC_DIR/driver/tcpm/fusb302.h $SRC_DIR/driver

cp $CP_OPTS $EC_DIR/driver/tcpm/stm32gx.c $SRC_DIR/driver
cp $CP_OPTS $EC_DIR/driver/tcpm/stm32gx.h $SRC_DIR/driver

cp $CP_OPTS $EC_DIR/driver/tcpm/tcpci.c $SRC_DIR/driver

cp $CP_OPTS $ZEPHYR_DIR/drivers/usb_c/tcpc/ucpd_stm32.c $SRC_DIR/driver
cp $CP_OPTS $ZEPHYR_DIR/drivers/usb_c/tcpc/ucpd_stm32_priv.h $SRC_DIR/driver

#
# Core patches
#
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_prl_sm.c
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_pd_dpm.c
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_pe_drp_sm.c

#spatch --sp-file $PATCHES_DIR/is_enabled_expand.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
$EVAL_MACRO $SRC_DIR/usb_pe_drp_sm.c $LIB_CONFIG
spatch --sp-file $PATCHES_DIR/remove_pe_states.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
#spatch --sp-file $PATCHES_DIR/remove_dead_branches.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place

#
# Driver patches
#
unifdef $UNIFDEF_OPTS $SRC_DIR/driver/fusb302.c
unifdef $UNIFDEF_OPTS $SRC_DIR/driver/stm32gx.c
unifdef $UNIFDEF_OPTS $SRC_DIR/driver/tcpci.c

$EVAL_MACRO $SRC_DIR/driver/fusb302.c $LIB_CONFIG
$EVAL_MACRO $SRC_DIR/driver/stm32gx.c $LIB_CONFIG
$EVAL_MACRO $SRC_DIR/driver/tcpci.c $LIB_CONFIG

spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/driver/fusb302.c --in-place
spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/driver/fusb302.c --in-place
spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/driver/stm32gx.c --in-place
spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/driver/stm32gx.c --in-place
spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/driver/tcpci.c --in-place
spatch --sp-file $PATCHES_DIR/remove_unused_static.cocci $SRC_DIR/driver/tcpci.c --in-place
