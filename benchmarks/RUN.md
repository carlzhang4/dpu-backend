
### Benchmarks



we have three benchmarks
- Microbenchmark
    this benchmark test the bandwidth of CPU-DPU using `dpu_push_xfer`
- PID collective communication
    this benchmark test the time of four collective communication primitives based on PID
- naive collective communication
    this benchmark test the time of four collective communication primitives based on naive solution

### How to Run

Before you run benchmarks, you should make dpu-backend and pidcomm_lib. Assume you are in the `./build` directory.The output time is in ms.

#### Microbenchmark
```bash
./benchmarks/Microbenchmark/DPU_CPU_Bandwidth
```

The output will be like this:

```
*********************************************
total dpu number : 64
total data size : 4 MB
DPU 2 CPU time cost : 161.776313
CPU 2 DPU cost : 98.753089
DPU 2 CPU Bandwidth: 1.545344 GB/s 
CPU 2 DPU Bandwidth: 2.531566 GB/s
```


#### PID collective communication

```bash
./benchmarks/PID_collective_communication/test_PID
```

The output will be like this:

```
test all gather
316.199055
test all reduce
799.635276
test reduce scatter
541.252702
test all to all
680.763600
Segmentation fault
```


#### naive collective communication

```bash
./benchmarks/naive_collective_communication/test_naive
```

The output will be like this:

```
test all gather
1612.850639
test all reduce
30545.429013
test reduce scatter
30210.956032
test all to all
 58138.268256
```