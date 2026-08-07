#include "../host_shim.h"
#include "test_attack_cdc.c"
#include "test_attack_udc.c"
#include "test_captured.c"
#include "test_state.c"
#include "test_w1c.c"
int main(void)
{
	printf("USB regression tests\n");
	run_usb_attack_cdc_test_stale_abort_must_not_release_new_claim();
	run_usb_attack_cdc_test_stale_abort_reproduces_captured_wedge();
	run_usb_attack_cdc_test_enable_arms_rx_without_irq_rx_enabled();
	run_usb_attack_cdc_test_no_rearm_path_survives_lost_completion();
	run_usb_attack_cdc_test_zlp_needed_latches_while_suspended();
	run_usb_attack_cdc_test_write_during_inflight_tx_is_not_lost();
	run_usb_attack_cdc_test_poll_out_during_inflight_is_covered_by_completion();
	run_usb_attack_cdc_test_notif_sem_reset_leaves_caller_blocked_forever();
	run_usb_attack_cdc_test_notif_sem_eagain_check_is_dead_code();
	run_usb_attack_cdc_test_disable_and_error_path_double_release();
	run_usb_attack_udc_test_dequeue_desyncs_data_toggle();
	run_usb_attack_udc_test_toggle_desync_is_permanent();
	run_usb_attack_udc_test_set_halt_out_burns_toggle_step();
	run_usb_attack_udc_test_clear_halt_reset_to_data0_is_correct();
	run_usb_attack_udc_test_setup_forced_pid_masks_ep0_cancel();
	run_usb_attack_udc_test_ep_enable_must_reset_toggle();
	run_usb_attack_udc_test_abort_timeout_must_not_touch_buf_ctrl();
	run_usb_attack_udc_test_abandoned_set_address_corrupts_later_transfer();
	run_usb_attack_udc_test_stale_post_status_applies_wrong_address();
	run_usb_attack_udc_test_enobufs_leaves_out_endpoint_dead();
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
