#include "../host_shim.h"
#include "test_state.c"
#include "test_w1c.c"
int main(void)
{
	printf("USB regression tests\n");
	run_usb_state_test_rx_busy_released_on_alloc_failure();
	run_usb_state_test_rx_busy_released_on_enqueue_failure();
	run_usb_state_test_resume_must_not_double_arm();
	run_usb_state_test_skew_looks_like_a_lost_completion();
	run_usb_state_test_enable_clears_stale_suspend();
	run_usb_state_test_tx_enqueue_failure_reschedules();
	run_usb_state_test_set_configuration_reasserts_state();
	run_usb_state_test_bus_reset_always_reaches_default();
	run_usb_state_test_resume_after_reset_is_delivered();
	run_usb_w1c_test_clr_alias_cannot_clear_w1c();
	run_usb_w1c_test_direct_write_clears_w1c();
	run_usb_w1c_test_ro_bit_is_never_writable();
	run_usb_w1c_test_buff_status_accumulates_with_clr_alias();
	run_usb_w1c_test_buff_status_snapshot_ack_is_correct();
	run_usb_w1c_test_snapshot_ack_preserves_late_completion();
	printf("\n%d run, %d failed\n", host_tests_run, host_tests_failed);
	return host_tests_failed ? 1 : 0;
}
