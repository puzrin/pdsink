#!/bin/sh

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
EC_DIR=$SCRIPT_DIR/ec_tmp
SRC_DIR=$SCRIPT_DIR/../src
INCLUDE_DIR=$SCRIPT_DIR/../include
CP_OPTS=""
UNIFDEF_OPTS="-m -f $SCRIPT_DIR/unifdef_opts"
REMOVE_FUNCTIONS=$SCRIPT_DIR/remove_functions.py
REMOVE_STATES=$SCRIPT_DIR/remove_states.py

[ -d $EC_DIR/.git ] || git clone --depth 1 https://chromium.googlesource.com/chromiumos/platform/ec $EC_DIR

mkdir -p $SCRIPT_DIR/../src
mkdir -p $SCRIPT_DIR/../src/driver
mkdir -p $SCRIPT_DIR/../include

# Core

cp $CP_OPTS $EC_DIR/common/usbc/usb_pd_timer.c $SRC_DIR
cp $CP_OPTS $EC_DIR/include/usb_pd_timer.h $INCLUDE_DIR

cp $CP_OPTS $EC_DIR/common/usbc/usb_prl_sm.c $SRC_DIR
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_prl_sm.c
cp $CP_OPTS $EC_DIR/include/usb_prl_sm.h $INCLUDE_DIR

cp $CP_OPTS $EC_DIR/common/usbc/usb_pd_dpm.c $SRC_DIR
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_pd_dpm.c

cp $CP_OPTS $EC_DIR/common/usbc/usb_pe_private.h $SRC_DIR

cp $CP_OPTS $EC_DIR/common/usbc/usb_pe_drp_sm.c $SRC_DIR
unifdef $UNIFDEF_OPTS $SRC_DIR/usb_pe_drp_sm.c
$REMOVE_FUNCTIONS $SRC_DIR/usb_pe_drp_sm.c
$REMOVE_STATES $SRC_DIR/usb_pe_drp_sm.c

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
