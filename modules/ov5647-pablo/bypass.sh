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

# 6. VC DMA done (csi_dma_tag): a VC0 frame armed at frame start by csis_check_vc_dma_buf()
#    was never tagged by a leader shot, so frame->subdev is NULL (WARN at "WARN_ON(!f_subdev)"
#    then an oops). Use the channel's DMA subdev, and touch the leader frame only if known.
F=$C/is-device-csi_v4.c
grep -q 'bypass: untagged VC frame' $F || sed -i '/^static void csi_dma_tag(/,/^}/ {
    s|^\t\t\tf_subdev = frame->subdev;$|\t\t\tf_subdev = frame->subdev ? frame->subdev : dma_subdev;\t/* bypass: untagged VC frame */|
    s|^\t\t\tfindex = frame->stream->findex;$|\t\t\tldr_frame = frame;\n\t\t\tif (frame->stream \&\& ldr_framemgr \&\& frame->subdev) {\n\t\t\tfindex = frame->stream->findex;|
    s|^\t\t\tclear_bit(f_subdev->id, \&ldr_frame->out_flag);$|\t\t\tclear_bit(f_subdev->id, \&ldr_frame->out_flag);\n\t\t\t}|
    s|^\t\t\tWARN_ON(!ldr_framemgr);$|\t\t\tWARN_ON(!ldr_framemgr \&\& frame->subdev);\t/* untagged: expected, no backtrace storm */|
}' $F
sed -n '/^static void csi_dma_tag(/,/CALL_VOPS(vctx, done/p' $F | grep -n 'bypass\|ldr_frame = \|findex\|clear_bit\|^\t\t\t}$'

# (7. Completing the leader frame at frame end when there is no ischain - removing the
#  "test_bit(IS_SENSOR_OTF_OUTPUT) -> skip" in is_sensor_notify_by_fend() - hung the SoC right
#  after sensor start; not applied. The capture tool queues enough leader buffers instead.)
