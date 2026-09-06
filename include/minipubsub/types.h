#ifndef MINIPUBSUB_TYPES_H
#define MINIPUBSUB_TYPES_H

#include <stdint.h>

#if defined(_WIN32)
#  define MPS_CALL __cdecl
#  if defined(MPS_BUILD_SHARED)
#    define MPS_API __declspec(dllexport)
#  elif defined(MPS_USE_SHARED)
#    define MPS_API __declspec(dllimport)
#  else
#    define MPS_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define MPS_CALL
#  define MPS_API __attribute__((visibility("default")))
#else
#  define MPS_CALL
#  define MPS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t mps_status_t;
typedef uint32_t mps_request_id_t;
typedef uint16_t mps_protocol_error_t;
typedef uint32_t mps_event_kind_t;
typedef uint32_t mps_event_delivery_mode_t;
typedef uint32_t mps_disconnect_cause_t;
typedef uint32_t mps_log_severity_t;

typedef struct mps_client mps_client_t;
typedef struct mps_client_config mps_client_config_t;
typedef struct mps_server mps_server_t;
typedef struct mps_server_config mps_server_config_t;
typedef struct mps_event mps_event_t;

typedef struct mps_bytes_view {
    /* Borrowed bytes. data may be null only when length is zero. */
    const uint8_t* data;
    uint32_t length;
} mps_bytes_view_t;

typedef struct mps_diagnostic {
    /* The library writes struct_size and always NUL-terminates message. */
    uint32_t struct_size;
    int32_t system_code;
    char message[256];
} mps_diagnostic_t;

#define MPS_STATUS_OK ((mps_status_t)0)
#define MPS_STATUS_INVALID_ARGUMENT ((mps_status_t)1)
#define MPS_STATUS_OUT_OF_MEMORY ((mps_status_t)2)
#define MPS_STATUS_INVALID_STATE ((mps_status_t)3)
#define MPS_STATUS_TIMEOUT ((mps_status_t)4)
#define MPS_STATUS_WOULD_BLOCK ((mps_status_t)5)
#define MPS_STATUS_IO_ERROR ((mps_status_t)6)
#define MPS_STATUS_CLOSED ((mps_status_t)7)
#define MPS_STATUS_WRONG_EVENT_MODE ((mps_status_t)8)
#define MPS_STATUS_INTERNAL_ERROR ((mps_status_t)9)

#define MPS_EVENT_OK ((mps_event_kind_t)1)
#define MPS_EVENT_PONG ((mps_event_kind_t)2)
#define MPS_EVENT_MESSAGE ((mps_event_kind_t)3)
#define MPS_EVENT_REMOTE_ERROR ((mps_event_kind_t)4)
#define MPS_EVENT_DISCONNECTED ((mps_event_kind_t)5)

#define MPS_EVENT_MODE_POLL ((mps_event_delivery_mode_t)1)
#define MPS_EVENT_MODE_CALLBACK ((mps_event_delivery_mode_t)2)

#define MPS_DISCONNECT_REQUESTED ((mps_disconnect_cause_t)1)
#define MPS_DISCONNECT_REMOTE_CLOSED ((mps_disconnect_cause_t)2)
#define MPS_DISCONNECT_IO_ERROR ((mps_disconnect_cause_t)3)
#define MPS_DISCONNECT_PROTOCOL_ERROR ((mps_disconnect_cause_t)4)
#define MPS_DISCONNECT_QUEUE_LIMIT ((mps_disconnect_cause_t)5)

#define MPS_PROTOCOL_UNKNOWN_OPCODE ((mps_protocol_error_t)0x0001)
#define MPS_PROTOCOL_MALFORMED_FRAME ((mps_protocol_error_t)0x0002)
#define MPS_PROTOCOL_CHANNEL_TOO_LONG ((mps_protocol_error_t)0x0003)
#define MPS_PROTOCOL_MESSAGE_TOO_LARGE ((mps_protocol_error_t)0x0004)
#define MPS_PROTOCOL_INVALID_CHANNEL ((mps_protocol_error_t)0x0005)
#define MPS_PROTOCOL_INVALID_REQUEST ((mps_protocol_error_t)0x0006)
#define MPS_PROTOCOL_INTERNAL_ERROR ((mps_protocol_error_t)0x0007)

typedef void (MPS_CALL *mps_event_callback_t)(const mps_event_t* event,
                                              void* user_data);

typedef struct mps_log_record {
    uint32_t struct_size;
    mps_log_severity_t severity;
    uint64_t connection_id;
    const char* component;
    const char* event_name;
    const char* message;
} mps_log_record_t;

typedef void (MPS_CALL* mps_log_callback_t)(const mps_log_record_t* record,
                                            void* user_data);

#define MPS_LOG_DEBUG ((mps_log_severity_t)1)
#define MPS_LOG_INFO ((mps_log_severity_t)2)
#define MPS_LOG_WARNING ((mps_log_severity_t)3)
#define MPS_LOG_ERROR ((mps_log_severity_t)4)

#ifdef __cplusplus
}
#endif

#endif