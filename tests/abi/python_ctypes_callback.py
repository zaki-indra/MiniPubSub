import ctypes
import sys
import threading

lib = ctypes.CDLL(sys.argv[1])
c_void_p_p = ctypes.POINTER(ctypes.c_void_p)
callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
for name, args in {
    "mps_server_config_create": [c_void_p_p, ctypes.c_void_p],
    "mps_server_config_set_port": [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_void_p],
    "mps_server_create": [ctypes.c_void_p, c_void_p_p, ctypes.c_void_p],
    "mps_server_start": [ctypes.c_void_p, ctypes.c_void_p],
    "mps_server_bound_port": [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint16), ctypes.c_void_p],
    "mps_server_request_stop": [ctypes.c_void_p, ctypes.c_void_p],
    "mps_server_wait": [ctypes.c_void_p, ctypes.c_void_p],
    "mps_client_config_create": [c_void_p_p, ctypes.c_void_p],
    "mps_client_config_set_port": [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_void_p],
    "mps_client_config_set_event_callback": [ctypes.c_void_p, callback_type, ctypes.c_void_p, ctypes.c_void_p],
    "mps_client_create": [ctypes.c_void_p, c_void_p_p, ctypes.c_void_p],
    "mps_client_connect": [ctypes.c_void_p, ctypes.c_void_p],
    "mps_client_ping": [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p],
}.items():
    getattr(lib, name).argtypes = args
lib.mps_event_kind.argtypes = [ctypes.c_void_p]
lib.mps_event_kind.restype = ctypes.c_uint32
lib.mps_server_config_destroy.argtypes = [ctypes.c_void_p]
lib.mps_server_destroy.argtypes = [ctypes.c_void_p]
lib.mps_client_config_destroy.argtypes = [ctypes.c_void_p]
lib.mps_client_destroy.argtypes = [ctypes.c_void_p]

server_config, server = ctypes.c_void_p(), ctypes.c_void_p()
assert lib.mps_server_config_create(ctypes.byref(server_config), None) == 0
assert lib.mps_server_config_set_port(server_config, 0, None) == 0
assert lib.mps_server_create(server_config, ctypes.byref(server), None) == 0
lib.mps_server_config_destroy(server_config)
assert lib.mps_server_start(server, None) == 0
port = ctypes.c_uint16()
assert lib.mps_server_bound_port(server, ctypes.byref(port), None) == 0

called = threading.Event()
marker = ctypes.c_uint32(0x12345678)
@callback_type
def callback(event, user_data):
    pointer = ctypes.cast(user_data, ctypes.POINTER(ctypes.c_uint32))
    if pointer.contents.value == marker.value and lib.mps_event_kind(event) == 2:
        called.set()

client_config, client = ctypes.c_void_p(), ctypes.c_void_p()
assert lib.mps_client_config_create(ctypes.byref(client_config), None) == 0
assert lib.mps_client_config_set_port(client_config, port, None) == 0
assert lib.mps_client_config_set_event_callback(
    client_config, callback, ctypes.byref(marker), None) == 0
assert lib.mps_client_create(client_config, ctypes.byref(client), None) == 0
lib.mps_client_config_destroy(client_config)
assert lib.mps_client_connect(client, None) == 0
request_id = ctypes.c_uint32()
assert lib.mps_client_ping(client, ctypes.byref(request_id), None) == 0
assert called.wait(5)
lib.mps_client_destroy(client)
lib.mps_server_request_stop(server, None)
lib.mps_server_wait(server, None)
lib.mps_server_destroy(server)
