/* bypass: find_frame() match (0) for a leader frame whose VC0 buffer is not armed yet */
static ulong bypass_vc0_armed(struct is_frame *frame, void *data)
{
	return test_bit((ulong)data, &frame->out_flag);
}

