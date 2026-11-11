| module | main | imp | epoll-main | epoll-imp |
| --- | ---: | ---: | ---: | ---: |
| RESP parser | 248 | 248 | 251 | 251 |
| reply encoders | 149 | 149 | 149 | 149 |
| engine: plan, batch, shard, hashing | 402 | 371 | 403 | 372 |
| byte copies (memcpy/memmove) | 58 | 58 | 49 | 49 |
| allocator | 0 | 0 | 0 | 0 |
| runtime (reactor, slots, cross-core) | 81 | 58 | 0 | 0 |
| front end | 20 | 22 | 17 | 15 |
| other (libc, kernel entry stubs, threads) | 9 | 9 | 16 | 16 |
| **total** | **967** | **913** | **886** | **853** |
| requests counted | 1,734,016 | 3,035,648 | 2,916,112 | 2,915,808 |
