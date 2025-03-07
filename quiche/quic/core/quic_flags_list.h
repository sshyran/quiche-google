// Copyright (c) 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is autogenerated by the QUICHE Copybara export script.

#ifdef QUIC_FLAG

QUIC_FLAG(quic_restart_flag_quic_offload_pacing_to_usps2, false)
// A testonly reloadable flag that will always default to false.
QUIC_FLAG(quic_reloadable_flag_quic_testonly_default_false, false)
// A testonly reloadable flag that will always default to true.
QUIC_FLAG(quic_reloadable_flag_quic_testonly_default_true, true)
// A testonly restart flag that will always default to false.
QUIC_FLAG(quic_restart_flag_quic_testonly_default_false, false)
// A testonly restart flag that will always default to true.
QUIC_FLAG(quic_restart_flag_quic_testonly_default_true, true)
// If bytes in flight has dipped below 1.25*MaxBW in the last round, do not exit PROBE_UP due to excess queue buildup.
QUIC_FLAG(quic_reloadable_flag_quic_bbr2_no_probe_up_exit_if_no_queue, true)
// If true, QUIC will default enable MTU discovery at server, with a target of 1450 bytes.
QUIC_FLAG(quic_reloadable_flag_quic_enable_mtu_discovery_at_server, false)
// If true, QuicGsoBatchWriter will support release time if it is available and the process has the permission to do so.
QUIC_FLAG(quic_restart_flag_quic_support_release_time_for_gso, false)
// If true, TlsHandshaker::AdvanceHandshake will check if connection is closed after SSL_do_handshake.
QUIC_FLAG(quic_reloadable_flag_quic_tls_handshaker_check_connection_closed, true)
// If true, abort async QPACK header decompression in QuicSpdyStream::Reset() and in QuicSpdyStream::OnStreamReset().
QUIC_FLAG(quic_reloadable_flag_quic_abort_qpack_on_stream_reset, true)
// If true, ack frequency frame can be sent from server to client.
QUIC_FLAG(quic_reloadable_flag_quic_can_send_ack_frequency, true)
// If true, allow client to enable BBRv2 on server via connection option \'B2ON\'.
QUIC_FLAG(quic_reloadable_flag_quic_allow_client_enabled_bbr_v2, true)
// If true, close read side but not write side in QuicSpdyStream::OnStreamReset().
QUIC_FLAG(quic_reloadable_flag_quic_fix_on_stream_reset, true)
// If true, close the connection if a crypto send buffer exceeds its size limit.
QUIC_FLAG(quic_reloadable_flag_quic_bounded_crypto_send_buffer, false)
// If true, consider original connection ID as active before handshake completes.
QUIC_FLAG(quic_reloadable_flag_quic_consider_original_connection_id_as_active_pre_handshake, true)
// If true, default-enable 5RTO blachole detection.
QUIC_FLAG(quic_reloadable_flag_quic_default_enable_5rto_blackhole_detection2, true)
// If true, deliver INITIAL packets before other types of packets in QuicBufferedPacketStore.
QUIC_FLAG(quic_reloadable_flag_quic_deliver_initial_packets_first, true)
// If true, disable QUIC version Q043.
QUIC_FLAG(quic_reloadable_flag_quic_disable_version_q043, false)
// If true, disable QUIC version Q046.
QUIC_FLAG(quic_reloadable_flag_quic_disable_version_q046, false)
// If true, disable QUIC version Q050.
QUIC_FLAG(quic_reloadable_flag_quic_disable_version_q050, false)
// If true, disable QUIC version h3 (RFCv1).
QUIC_FLAG(quic_reloadable_flag_quic_disable_version_rfcv1, false)
// If true, disable QUIC version h3-29.
QUIC_FLAG(quic_reloadable_flag_quic_disable_version_draft_29, false)
// If true, disable blackhole detection on server side.
QUIC_FLAG(quic_reloadable_flag_quic_disable_server_blackhole_detection, false)
// If true, disable resumption when receiving NRES connection option.
QUIC_FLAG(quic_reloadable_flag_quic_enable_disable_resumption, true)
// If true, discard INITIAL packet if the key has been dropped.
QUIC_FLAG(quic_reloadable_flag_quic_discard_initial_packet_with_key_dropped, true)
// If true, do not bundle ACK while sending PATH_CHALLENGE on alternative path.
QUIC_FLAG(quic_reloadable_flag_quic_not_bundle_ack_on_alternative_path, true)
// If true, enable server retransmittable on wire PING.
QUIC_FLAG(quic_reloadable_flag_quic_enable_server_on_wire_ping, true)
// If true, flush pending frames as well as pending padding bytes on connection migration.
QUIC_FLAG(quic_reloadable_flag_quic_flush_pending_frames_and_padding_bytes_on_migration, true)
// If true, ietf connection migration is no longer conditioned on connection option RVCM.
QUIC_FLAG(quic_reloadable_flag_quic_remove_connection_migration_connection_option_v2, false)
// If true, include offset in QUIC STREAM_DATA_BLOCKED and DATA_BLOCKED frames.
QUIC_FLAG(quic_reloadable_flag_quic_include_offset_in_blocked_frames, false)
// If true, include stream information in idle timeout connection close detail.
QUIC_FLAG(quic_reloadable_flag_quic_add_stream_info_to_idle_close_detail, true)
// If true, quic server will send ENABLE_CONNECT_PROTOCOL setting and and endpoint will validate required request/response headers and extended CONNECT mechanism and update code counts of valid/invalid headers.
QUIC_FLAG(quic_reloadable_flag_quic_verify_request_headers_2, true)
// If true, reject or send error response code upon receiving invalid request or response headers. This flag depends on --gfe2_reloadable_flag_quic_verify_request_headers_2.
QUIC_FLAG(quic_reloadable_flag_quic_act_upon_invalid_header, false)
// If true, require handshake confirmation for QUIC connections, functionally disabling 0-rtt handshakes.
QUIC_FLAG(quic_reloadable_flag_quic_require_handshake_confirmation, false)
// If true, server proactively retires client issued connection ID on reverse path validation failure. 
QUIC_FLAG(quic_reloadable_flag_quic_retire_cid_on_reverse_path_validation_failure, true)
// If true, server sends bandwidth eastimate when network is idle for a while.
QUIC_FLAG(quic_restart_flag_quic_enable_sending_bandwidth_estimate_when_network_idle, true)
// If true, set burst token to 2 in cwnd bootstrapping experiment.
QUIC_FLAG(quic_reloadable_flag_quic_conservative_bursts, false)
// If true, stop resetting ideal_next_packet_send_time_ in pacing sender.
QUIC_FLAG(quic_reloadable_flag_quic_donot_reset_ideal_next_packet_send_time, false)
// If true, use BBRv2 as the default congestion controller. Takes precedence over --quic_default_to_bbr.
QUIC_FLAG(quic_reloadable_flag_quic_default_to_bbr_v2, false)
// If true, use PING manager to manage the PING alarm.
QUIC_FLAG(quic_reloadable_flag_quic_use_ping_manager2, true)
// If true, use new connection ID in connection migration.
QUIC_FLAG(quic_reloadable_flag_quic_connection_migration_use_new_cid_v2, true)
// If true, uses conservative cwnd gain and pacing gain when cwnd gets bootstrapped.
QUIC_FLAG(quic_reloadable_flag_quic_conservative_cwnd_and_pacing_gains, false)
// When the flag is true, exit STARTUP after the same number of loss events as PROBE_UP.
QUIC_FLAG(quic_reloadable_flag_quic_bbr2_startup_probe_up_loss_events, true)
// When true, defaults to BBR congestion control instead of Cubic.
QUIC_FLAG(quic_reloadable_flag_quic_default_to_bbr, false)
// When true, prevents QUIC\'s PacingSender from generating bursts when the congestion controller is CWND limited and not pacing limited.
QUIC_FLAG(quic_reloadable_flag_quic_fix_pacing_sender_bursts, true)
// When true, set the initial congestion control window from connection options in QuicSentPacketManager rather than TcpCubicSenderBytes.
QUIC_FLAG(quic_reloadable_flag_quic_unified_iw_options, true)
// When true, support draft-ietf-quic-v2-01
QUIC_FLAG(quic_reloadable_flag_quic_enable_version_2_draft_01, false)
// When true, the B203 connection option causes the Bbr2Sender to ignore inflight_hi during PROBE_UP and increase it when the bytes delivered without loss are higher.
QUIC_FLAG(quic_reloadable_flag_quic_bbr2_ignore_inflight_hi_in_probe_up, true)
// When true, the B205 connection option enables extra acked in STARTUP, and B204 adds new logic to decrease it whenever max bandwidth increases.
QUIC_FLAG(quic_reloadable_flag_quic_bbr2_startup_extra_acked, true)
// When true, the B207 connection option causes BBR2 to exit STARTUP if a persistent queue of 2*BDP has existed for the entire round.
QUIC_FLAG(quic_reloadable_flag_quic_bbr2_exit_startup_on_persistent_queue2, true)
// When true, the BBQ0 connection option causes QUIC BBR2 to add bytes_acked to probe_up_acked if the connection hasn\'t been app-limited since inflight_hi was utilized.
QUIC_FLAG(quic_reloadable_flag_quic_bbr2_add_bytes_acked_after_inflight_hi_limited, true)
// When true, the BBR4 copt sets the extra_acked window to 20 RTTs and BBR5 sets it to 40 RTTs.
QUIC_FLAG(quic_reloadable_flag_quic_bbr2_extra_acked_window, true)

#endif

