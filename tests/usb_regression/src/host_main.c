#include "../host_shim.h"
#include "test_captured.c"
#include "test_state.c"
#include "test_w1c.c"
int main(void)
{
	printf("USB regression tests\n");
	run_usb_captured_test_active_bus_sie_status();
	run_usb_captured_test_genuinely_suspended_sie_status();
	run_usb_captured_test_bus_reset_sie_status();
	run_usb_captured_test_buf_ctrl_bit_positions();
	run_usb_captured_test_bulk_in_buffer_held_by_controller();
	run_usb_captured_test_bulk_in_buffer_idle();
	run_usb_captured_test_bulk_out_armed_vs_unarmed();
	run_usb_captured_test_ep_error_bit_layout();
	run_usb_captured_test_captured_ep_errors_discriminate_seq_from_transaction();
	run_usb_captured_test_cdc_endpoint_addresses();
	run_usb_captured_test_pacing_inversion();
	run_usb_captured_test_sof_proves_bus_alive();
	run_usb_captured_test_sof_delta_handles_wrap();
	run_usb_captured_test_enumeration_stuck_before_fix();
	run_usb_captured_test_enumeration_completes_after_fix();
	run_usb_captured_test_struct_drift_produces_garbage();
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
