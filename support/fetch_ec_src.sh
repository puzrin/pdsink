#!/bin/sh

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
EC_DIR=$SCRIPT_DIR/ec_tmp
HELPERS_DIR=$SCRIPT_DIR/helpers
SRC_DIR=$SCRIPT_DIR/../src
INCLUDE_DIR=$SCRIPT_DIR/../include
CP_OPTS=""
LIB_CONFIG=$SRC_DIR/pd_config.h
UNIFDEF_OPTS="-m -f $LIB_CONFIG"
REMOVE_STATES=$SCRIPT_DIR/remove_states.py
EVAL_MACRO=$SCRIPT_DIR/helpers/eval_IS_ENABLED_macro.py

[ -d $EC_DIR/.git ] || git clone --depth 1 https://chromium.googlesource.com/chromiumos/platform/ec $EC_DIR

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

#
# Apply patches
#
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_prl_sm.c
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_pd_dpm.c
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_pe_drp_sm.c

$EVAL_MACRO $SRC_DIR/usb_pe_drp_sm.c $LIB_CONFIG
spatch --sp-file $HELPERS_DIR/remove_pe_states.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
spatch --sp-file $HELPERS_DIR/remove_unused_static.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
spatch --sp-file $HELPERS_DIR/remove_unused_static.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
#spatch --sp-file $HELPERS_DIR/remove_dead_branches.cocci $SRC_DIR/usb_pe_drp_sm.c --in-place
