| module | main | imp | epoll-main | epoll-imp |
| --- | ---: | ---: | ---: | ---: |
| RESP parser | 248 | 248 | 251 | 251 |
| reply encoders | 149 | 149 | 149 | 149 |
| engine: plan, batch, shard, hashing | 507 | 485 | 509 | 487 |
| byte copies (memcpy/memmove) | 72 | 72 | 64 | 64 |
| allocator | 11 | 1 | 0 | 1 |
| runtime (reactor, slots, cross-core) | 122 | 84 | 0 | 0 |
| front end | 29 | 32 | 30 | 26 |
| other (libc, kernel entry stubs, threads) | 13 | 13 | 19 | 19 |
| **total** | **1152** | **1083** | **1022** | **997** |
| requests counted | 2,239,680 | 2,656,256 | 2,417,664 | 2,026,992 |
