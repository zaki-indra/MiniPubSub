#ifndef MINIPUBSUB_CLIENT_H
#define MINIPUBSUB_CLIENT_H

#include "minipubsub/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration values are copied into the client by mps_client_create().
 * A successfully consumed configuration cannot be mutated and may be destroyed.
 * Polling is the default; setting a callback selects callback mode permanently. */

MPS_API mps_status_t MPS_CALL mps_client_config_create(mps_client_config_t** out_config, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_config_set_host(mps_client_config_t* config, const char* host,
                                                         mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_config_set_port(mps_client_config_t* config, uint16_t port,
                                                         mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_config_set_connect_timeout_ms(mps_client_config_t* config, uint32_t timeout_ms,
                                                                       mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_config_set_shutdown_timeout_ms(mps_client_config_t* config,
                                                                        uint32_t timeout_ms,
                                                                        mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_config_set_command_queue_limit(mps_client_config_t* config, uint32_t limit,
                                                                        mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_config_set_event_queue_limit(mps_client_config_t* config, uint32_t limit,
                                                                      mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_config_set_event_callback(mps_client_config_t* config,
                                                                   mps_event_callback_t callback, void* user_data,
                                                                   mps_diagnostic_t* diagnostic);
MPS_API void MPS_CALL mps_client_config_destroy(mps_client_config_t* config);

/* connect is bounded by connect_timeout_ms. Command calls are concurrent-safe,
 * copy their input before returning OK, and report completion through events. */
MPS_API mps_status_t MPS_CALL mps_client_create(const mps_client_config_t* config, mps_client_t** out_client,
                                                mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_connect(mps_client_t* client, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_publish(mps_client_t* client, const mps_bytes_view_t* channel,
                                                 const mps_bytes_view_t* message, mps_request_id_t* out_request_id,
                                                 mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_subscribe(mps_client_t* client, const mps_bytes_view_t* channel,
                                                   mps_request_id_t* out_request_id, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_unsubscribe(mps_client_t* client, const mps_bytes_view_t* channel,
                                                     mps_request_id_t* out_request_id, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_ping(mps_client_t* client, mps_request_id_t* out_request_id,
                                              mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_poll_event(mps_client_t* client, uint32_t timeout_ms, mps_event_t** out_event,
                                                    mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_request_stop(mps_client_t* client, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_client_wait(mps_client_t* client, mps_diagnostic_t* diagnostic);
MPS_API void MPS_CALL mps_client_destroy(mps_client_t* client);

/* Poll-mode events are owned and require mps_event_destroy(). Callback events
 * are borrowed. Accessor views live until poll-event destruction or callback
 * return, respectively. wait/destroy must not run on the callback thread. */
MPS_API mps_event_kind_t MPS_CALL mps_event_kind(const mps_event_t* event);
MPS_API mps_request_id_t MPS_CALL mps_event_request_id(const mps_event_t* event);
MPS_API mps_status_t MPS_CALL mps_event_channel(const mps_event_t* event, mps_bytes_view_t* out_channel);
MPS_API mps_status_t MPS_CALL mps_event_message(const mps_event_t* event, mps_bytes_view_t* out_message);
MPS_API mps_protocol_error_t MPS_CALL mps_event_protocol_error(const mps_event_t* event);
MPS_API const char*MPS_CALL mps_event_error_message(const mps_event_t* event);
MPS_API mps_disconnect_cause_t MPS_CALL mps_event_disconnect_cause(const mps_event_t* event);
MPS_API void MPS_CALL mps_event_destroy(mps_event_t* event);
#ifdef __cplusplus
}
#endif

#endif