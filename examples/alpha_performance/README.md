# FMT-04 / FMT-05 Result performance driver

The driver builds inputs and a workflow through the public Result API, compiles it, executes it repeatedly, and checks requested output samples through Result read windows.

The first execution is reported separately.

The positional argument order is `size member storage request algorithm dtype repetitions profile layout workers distribution managed`. `member` is `associate / unassociate / set / extract / remove / opaque`; `storage` is `generic / continuous / tiled`; `request` is `full / red / alpha / roi`; `algorithm` is `auto / scalar / simd / reference`; `dtype` is `f32 / f64`; `profile` is `strict / accelerated_apple_silicon / accelerated_x86_64`; `layout` is `auto / view / materialize`; `distribution` is `half / opaque / mixed / small`; and `managed` is `off / on`. The binary preserves the CLI ordering and option names.
