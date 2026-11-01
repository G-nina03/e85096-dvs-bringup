	if (!frame && !device->ischain) {	/* bypass: leader fcount != line_fcount without ISP */
		framemgr_e_barrier_irqs(framemgr, 0, flags);
		if (framemgr->queued_count[FS_PROCESS])
			frame = find_frame(framemgr, FS_PROCESS, bypass_vc0_armed,
				(void *)(ulong)device->ssvc0.id);
		framemgr_x_barrier_irqr(framemgr, 0, flags);
	}
