# MiniPubSub

MiniPubSub is an embeddable binary TCP publish/subscribe broker with a C23 ABI
and a C++23 implementation. The ABI is designed for direct use from C and
foreign-function interfaces.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run `build/minipubsub-server`, then use `build/minipubsub-cli ping` or
`build/minipubsub-cli publish CHANNEL MESSAGE`.
