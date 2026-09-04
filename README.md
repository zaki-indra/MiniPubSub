# MiniPubSub

MiniPubSub is an embeddable binary TCP publish/subscribe broker with a C23 ABI
and a C++23 implementation. The ABI is designed for direct use from C and
foreign-function interfaces.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run `build/debug/minipubsub-server`, then start an interactive client:

```sh
build/debug/minipubsub-cli --host 127.0.0.1 --port 7379
```

The prompt accepts `PING`, `PUBLISH`, `SUBSCRIBE`, `UNSUBSCRIBE`, `HELP`, and
`EXIT`. Existing one-shot commands such as `build/debug/minipubsub-cli ping`
remain available for scripts.
