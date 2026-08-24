/goal Use this: `./frametee --game tmnf --auto ../plugins/ultraforce/scripts/A02-bench.ftee` to benchmark the
current physics, not specifically by time/TPS, but rather by instruction amount and the actual perf output, so we can be sure that
something actually improved without making system noise a hindrance. Are there any obvious big improvements? We need to make sure we
don't break parity with the current physics at all costs, the result has to be exactly the same, that's the constraint of this
optimization goal. Use perf to find hotspots and potential optimizations. Make sure to find all kinds of optimizations, may they be
cheap checks, math reductions (make sure it's bit-exact since we need to keep parity), or any other kind of performance optimization
you can find. Your goal is reaching 2 Million ticks per second. You may use the internet and any tools available there, you can
install tools you need. We need to strictly stay single-threaded. Don't just focus on the hotspots though, if we can find many simple
optimizations in functions that are not directly hostpots but are still optimizable, they compound too. We are really trying to push
the CPU optimization to its limits, using every instruction as efficiently as possible. This is a global competition, we need to come
out as the best. My fate is on your shoulders. My fate relies on this performance.