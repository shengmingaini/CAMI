# engine/rpc · TEST

`rpc_test`（481 断言，全绿）：单元 U1-U6 + 集成 I1-I9 + Failure F1-F3，覆盖
映射表全枚举、Options 校验、退避（指数/±20% 抖动/1s 上限）、非幂等零重试、
池复用、指标；真实服务端集成：正常/超时/取消/重试成功（3 次到达 + 同
idempotency_key）/重试耗尽/非幂等只 1 次/TraceID 透传/限流/优雅关闭；
Failure：无服务端、不可达地址、消息超限。

运行：`ctest -R Rpc`（或 `build/bin/rpc_test.exe`，需 MSYS2 mingw64 DLL 在 PATH）。
