#ifndef MINIPUBSUB_SERVER_H
#define MINIPUBSUB_SERVER_H
#include "minipubsub/types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Server configuration is snapshotted by mps_server_create(). Port zero asks
 * the OS to choose a port, retrievable after start with mps_server_bound_port(). */
MPS_API mps_status_t MPS_CALL mps_server_config_create(mps_server_config_t** out_config, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_bind_address(mps_server_config_t* config, const char* address,
                                                                 mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_port(mps_server_config_t* config, uint16_t port,
                                                         mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_max_connections(mps_server_config_t* config, uint32_t limit,
                                                                    mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_max_channel_size(mps_server_config_t* config, uint32_t bytes,
                                                                     mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_max_message_size(mps_server_config_t* config, uint32_t bytes,
                                                                     mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_output_queue_limit(mps_server_config_t* config, uint32_t bytes,
                                                                       mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_command_queue_limit(mps_server_config_t* config, uint32_t commands,
                                                                        mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_shutdown_timeout_ms(mps_server_config_t* config,
                                                                        uint32_t timeout_ms,
                                                                        mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_config_set_log_callback(mps_server_config_t* config,
                                                                 mps_log_callback_t callback, void* user_data,
                                                                 mps_diagnostic_t* diagnostic);
MPS_API void MPS_CALL mps_server_config_destroy(mps_server_config_t* config);

/* start completes bind/listen before returning. request_stop is idempotent and
 * non-blocking; wait joins all library-owned server threads. */
MPS_API mps_status_t MPS_CALL mps_server_create(const mps_server_config_t* config, mps_server_t** out_server,
                                                mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_start(mps_server_t* server, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_bound_port(const mps_server_t* server, uint16_t* out_port,
                                                    mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_request_stop(mps_server_t* server, mps_diagnostic_t* diagnostic);
MPS_API mps_status_t MPS_CALL mps_server_wait(mps_server_t* server, mps_diagnostic_t* diagnostic);
MPS_API void MPS_CALL mps_server_destroy(mps_server_t* server);
#ifdef __cplusplus
}
#endif
#endif