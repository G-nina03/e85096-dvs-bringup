
	if (!ischain) {	/* bypass: no ISP chain; only the sensor group needs its frame-start trigger */
		group_sensor = &device->group_sensor;
		if (list_empty(&group_sensor->smp_trigger.wait_list)) {
			atomic_set(&group_sensor->sensor_fcount, fcount + group_sensor->skip_shots);
		} else {
			backup_fcount = atomic_read(&group_sensor->backup_fcount);
			if (fcount + group_sensor->skip_shots > backup_fcount) {
				atomic_set(&group_sensor->sensor_fcount, fcount + group_sensor->skip_shots);
				up(&group_sensor->smp_trigger);
			}
		}
		goto trigger_skip;
	}
