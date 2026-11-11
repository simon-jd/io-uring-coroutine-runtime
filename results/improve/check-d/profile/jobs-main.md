| | main io_uring 1c | main io_uring 2c | main io_uring 4c |
| --- | ---: | ---: | ---: |
| send submission | 12 | 13 | 13 |
| completions: reap, dispatch, resume | 36 | 36 | 37 |
| slot acquire and release | 32 | 53 | 96 |
| cross-core calls | 0 | 19 | 69 |
| front end (the connection coroutine) | 20 | 29 | 54 |
| other runtime | 2 | 2 | 3 |
| total | 102 | 151 | 272 |
