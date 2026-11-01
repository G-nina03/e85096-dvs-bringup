#!/bin/sh
# Let the Pablo (fimc-is) sensor path run without an ISP chain (bypass: CSIS -> WDMA -> memory).
# The OV5647 is a RAW sensor; the framework assumes a 3AA/ISP chain behind every sensor group.
#   sh modules/ov5647-pablo/bypass.sh /work/kernel
set -eu
K=$1
C=$K/drivers/media/platform/exynos/camera

# 1. is_group_buffer_queue(): sensor = device->sensor is NULL when no ischain was opened
#    ("BUG(!sensor)"); the sensor group also keeps its sensor in group->sensor.
F=$C/is-groupmgr.c
grep -q 'bypass: no ischain' $F ||
    sed -i '/^int is_group_buffer_queue(/,/^}/ s|^\tsensor = device->sensor;$|\tsensor = device->sensor;\n\tif (!sensor)\t/* bypass: no ischain bound to this sensor */\n\t\tsensor = group->sensor;|' $F
grep -n 'bypass: no ischain' -B1 -A1 $F

# 2. is_hw_camif_cfg(): only selects the CSIS -> 3AA (IPP) OTF input. Without an ischain there is
#    no OTF consumer, so skip it instead of failing back_start with -ENODEV.
F=$C/ischain/is-v8_20_0/is-hw-chain.c
grep -q 'bypass: RAW to memory' $F ||
    sed -i '/^int is_hw_camif_cfg(/,/^}/ s|^\t\tmerr("ischain is null\\n", sensor);$|\t\tminfo("no ischain: CSIS OTF path not set (bypass: RAW to memory)\\n", sensor);\n\t\treturn 0;|' $F
grep -n 'bypass: RAW to memory' -B3 -A3 $F

# 3. group->device (the ischain struct the sensor group is attached to) is checked for ->sensor in
#    several places (is_group_start: "BUG(!group->device->sensor)"). Link only that direction at
#    S_INPUT; sensor->ischain stays NULL, so ISP firmware calls (is_itf_stream_on, ...) are skipped.
F=$C/is-device-sensor_v2.c
grep -q 'bypass: link the sensor' $F ||
    sed -i '/^int is_sensor_s_input(/,/^}/ s|^\tif ((device->pdata->use_module_sel) \&\&$|\t/* bypass: link the sensor to its group device when no ischain was bound */\n\tif (group->device \&\& !group->device->sensor)\n\t\tgroup->device->sensor = device;\n\n&|' $F
grep -n 'bypass: link the sensor' -A3 $F

# 4. is_groupmgr_init() (run when an ischain is opened) makes the sensor group the head/tail of
#    its chain; without it group->head is NULL and the group worker oopses in is_sensor_shot().
#    The sensor group alone is its own chain.
grep -q 'bypass: sensor group alone' $F ||
    sed -i '/^int is_sensor_s_input(/,/^}/ s|^\t\tgroup->device->sensor = device;$|&\n\tif (!group->head) {\t/* bypass: sensor group alone is its own chain */\n\t\tgroup->head = group;\n\t\tgroup->tail = group;\n\t}|' $F
grep -n 'bypass: link the sensor' -A8 $F

# 5. CSIS frame-start tasklet (OTF variant, always used) dereferences device->ischain for the
#    3AA/ISP groups. Without an ischain only the sensor group trigger is needed.
F=$C/is-device-csi_v4.c
H=$(cd "$(dirname "$0")" && pwd)
grep -q 'bypass: no ISP chain; only the sensor group' $F ||
    sed -i "/^void tasklet_csis_str_otf(/,/^}/ { /^\tischain = device->ischain;$/r $H/bypass-csis-fstart.inc
}" $F
grep -n 'bypass: no ISP chain; only the sensor group' -B2 -A3 $F
