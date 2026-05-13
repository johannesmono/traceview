# traceview

A simple visualizer for electromagnetic traces with 8-bit vertical resolution.


## Features

* view 8-bit traces
* filter traces using simple text-based rules
* highlight specific traces or colorize all traces
* [Largest-Triangle-Three-Buckets (LTTB)](https://skemman.is/handle/1946/15343) downsampling
* configurable LTTB bucket size (click on "LTTB Buckets")

Demo: https://www.asdf.one/traceview.mov


## Usage

On MacOS with Metal, build and run traceview:

```
$ ./build.sh build run path/to/trace.dat
```

While other platforms are not officially supported, it should be straightforward to adapt the code using any other renderer supported by [sokol_app.h](https://github.com/floooh/sokol/tree/master#sokol_apph). For more information, please refer to the [sokol samples repository](https://github.com/floooh/sokol-samples#how-to-build-without-a-build-system).


## Data Format

```
struct {
	u32 traces;
	struct {
		u32 points;
		u8 input[128];
		u8 output[128];
		s8 data[points];
	} data[traces];
};
```

Currently, the implementation assumes that the number of traces fits into a `u16` and that the number of points is the same for all traces in the data file. The input and output are simply skipped during parsing.
