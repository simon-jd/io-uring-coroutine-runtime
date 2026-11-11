| | io_uring 1c | io_uring 2c | io_uring 4c | epoll 1c | epoll 2c | epoll 4c |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| send submission | 13 | 13 | 14 | 0 | 0 | 0 |
| completions: reap, dispatch, resume | 35 | 36 | 39 | 0 | 0 | 0 |
| slot acquire and release | 9 | 19 | 27 | 0 | 0 | 0 |
| cross-core calls | 0 | 14 | 60 | 0 | 0 | 0 |
| front end (the connection coroutine) | 22 | 32 | 54 | 15 | 26 | 47 |
| other runtime | 1 | 1 | 1 | 0 | 0 | 0 |
| total | 79 | 116 | 195 | 15 | 26 | 47 |
