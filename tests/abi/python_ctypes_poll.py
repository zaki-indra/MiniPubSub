import ctypes
import sys

lib = ctypes.CDLL(sys.argv[1])
c_void_p_p = ctypes.POINTER(ctypes.c_void_p)
lib.mps_server_config_create.argtypes = [c_void_p_p, ctypes.c_void_p]
lib.mps_server_config_set_port.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_void_p]
lib.mps_server_create.argtypes = [ctypes.c_void_p, c_void_p_p, ctypes.c_void_p]
lib.mps_server_start.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
lib.mps_server_bound_port.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint16), ctypes.c_void_p]
lib.mps_server_request_stop.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
lib.mps_server_wait.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
lib.mps_server_config_destroy.argtypes = [ctypes.c_void_p]
lib.mps_server_destroy.argtypes = [ctypes.c_void_p]
lib.mps_client_config_create.argtypes = [c_void_p_p, ctypes.c_void_p]
lib.mps_client_config_set_port.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_void_p]
lib.mps_client_create.argtypes = [ctypes.c_void_p, c_void_p_p, ctypes.c_void_p]
lib.mps_client_connect.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
lib.mps_client_ping.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
lib.mps_client_poll_event.argtypes = [ctypes.c_void_p, ctypes.c_uint32, c_void_p_p, ctypes.c_void_p]
lib.mps_event_kind.argtypes = [ctypes.c_void_p]
lib.mps_event_kind.restype = ctypes.c_uint32
lib.mps_event_request_id.argtypes = [ctypes.c_void_p]
lib.mps_event_request_id.restype = ctypes.c_uint32
lib.mps_event_destroy.argtypes = [ctypes.c_void_p]
lib.mps_client_config_destroy.argtypes = [ctypes.c_void_p]
lib.mps_client_destroy.argtypes = [ctypes.c_void_p]

server_config = ctypes.c_void_p()
server = ctypes.c_void_p()
assert lib.mps_server_config_create(ctypes.byref(server_config), None) == 0
assert lib.mps_server_config_set_port(server_config, 0, None) == 0
assert lib.mps_server_create(server_config, ctypes.byref(server), None) == 0
lib.mps_server_config_destroy(server_config)
assert lib.mps_server_start(server, None) == 0
port = ctypes.c_uint16()
assert lib.mps_server_bound_port(server, ctypes.byref(port), None) == 0

client_config = ctypes.c_void_p()
client = ctypes.c_void_p()
assert lib.mps_client_config_create(ctypes.byref(client_config), None) == 0
assert lib.mps_client_config_set_port(client_config, port, None) == 0
assert lib.mps_client_create(client_config, ctypes.byref(client), None) == 0
lib.mps_client_config_destroy(client_config)
assert lib.mps_client_connect(client, None) == 0
request_id = ctypes.c_uint32()
assert lib.mps_client_ping(client, ctypes.byref(request_id), None) == 0
event = ctypes.c_void_p()
assert lib.mps_client_poll_event(client, 5000, ctypes.byref(event), None) == 0
assert lib.mps_event_kind(event) == 2
assert lib.mps_event_request_id(event) == request_id.value
lib.mps_event_destroy(event)
lib.mps_client_destroy(client)
lib.mps_server_request_stop(server, None)
lib.mps_server_wait(server, None)
lib.mps_server_destroy(server)
