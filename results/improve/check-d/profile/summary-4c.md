| module | main | imp | epoll-main | epoll-imp |
| --- | ---: | ---: | ---: | ---: |
| RESP parser | 248 | 248 | 251 | 251 |
| reply encoders | 149 | 149 | 149 | 149 |
| engine: plan, batch, shard, hashing | 567 | 549 | 568 | 551 |
| byte copies (memcpy/memmove) | 80 | 79 | 71 | 71 |
| allocator | 61 | 3 | 2 | 2 |
| runtime (reactor, slots, cross-core) | 218 | 141 | 0 | 0 |
| front end | 54 | 54 | 50 | 47 |
| other (libc, kernel entry stubs, threads) | 22 | 18 | 22 | 22 |
| **total** | **1397** | **1241** | **1114** | **1094** |
| requests counted | 1,894,960 | 2,283,056 | 2,305,072 | 2,189,408 |
