#!/bin/sh
# Root, on the board: cpumap-cap.sh [SECONDS] [OUTDIR]. ftrace sched_switch + hard/soft irq for SECONDS, with
# /proc/stat, /proc/interrupts, /proc/softirqs and cpufreq time_in_state before/after, and the thread list.
# 8 MB of trace buffer per CPU while it runs; read it on the Mac with tools/host/cpumap.py (11.30).
T=${1:-5}; O=${2:-/tmp/cpumap}
TR=/sys/kernel/tracing
EV="sched/sched_switch irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit"
rm -rf $O; mkdir -p $O
[ -d $TR/events ] || mount -t tracefs nodev $TR
echo 0 > $TR/tracing_on
echo nop > $TR/current_tracer
echo 1 > $TR/options/record-tgid
old=$(cat $TR/buffer_size_kb)
echo 8192 > $TR/buffer_size_kb
echo > $TR/trace
for e in $EV; do echo 1 > $TR/events/$e/enable; done
snap() {
	cat /proc/stat > $O/stat$1; cat /proc/interrupts > $O/irq$1; cat /proc/softirqs > $O/sirq$1
	for p in /sys/devices/system/cpu/cpufreq/policy*; do
		echo "$(cat $p/related_cpus | tr ' ' ,) $(cat $p/scaling_governor) $(tr '\n' ' ' < $p/stats/time_in_state)"
	done > $O/freq$1
}
snap 0
echo 1 > $TR/tracing_on
sleep $T
echo 0 > $TR/tracing_on
snap 1
for e in $EV; do echo 0 > $TR/events/$e/enable; done
grep -H overrun $TR/per_cpu/cpu*/stats > $O/overrun
ps -eLo pid,tid,psr,pcpu,comm,args > $O/ps.txt
cat $TR/trace > $O/trace.txt
echo > $TR/trace
echo $old > $TR/buffer_size_kb
echo 0 > $TR/options/record-tgid
chmod -R a+rX $O
wc -l $O/trace.txt
